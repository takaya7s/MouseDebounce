// mouse_ll_debounce_console.c
// 低レベルマウスフック(WH_MOUSE_LL)で左クリックのチャタリング/瞬断対策
// ・Down→Up < CHATTER_MS はUpを無視（チャタリング）
// ・Upは常に RECONTACT_MS 遅延確定。遅延中にDownが来たら瞬断扱いで Up/Down 両方無視（押下継続）
// ・押下中の重複Downは常に無視
// ・起動: 「チャタリング監視中...」、通常終了: 「終了しました。何かキーを押すと終了します...」
//   ※コンソール×ボタン等の強制終了ではキー待ちはできません（OSが即終了させるため）
//
// ビルド例(Release/x64, Unicode, /O2, user32.lib):
//   cl /O2 /W4 /DUNICODE /D_UNICODE mouse_ll_debounce_console.c user32.lib
// 閾値を変える場合（例）：/DCHATTER_MS=80 /DRECONTACT_MS=30

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <conio.h>

#ifndef CHATTER_MS
#define CHATTER_MS   100  // ★チャタリング(Down→Up)判定ms
#endif
#ifndef RECONTACT_MS
#define RECONTACT_MS 30   // Up確定遅延/Up→Down瞬断判定ms
#endif

static HHOOK    g_hook = NULL;
static volatile LONG g_exiting = 0;    // 1=終了中（素通し）
static BOOL     g_pressed_forwarded = FALSE;// Downを外へ通して押下中か
static DWORD    g_down_time_ms = 0;    // 通したDownの時刻
static BOOL     g_up_pending = FALSE;// Up確定待ち（遅延判定中）
static DWORD    g_up_cand_time_ms = 0;    // Up候補の時刻
static UINT_PTR g_up_timer_id = 0;    // SetTimer ID（1つだけ運用）

// ---- 1行出力（UTF-16 / 低コスト / リダイレクト時はwprintf）----
static void print_line(const wchar_t* msg) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) == FILE_TYPE_CHAR) {
        DWORD w = 0; WriteConsoleW(hOut, msg, (DWORD)wcslen(msg), &w, NULL);
        WriteConsoleW(hOut, L"\r\n", 2, &w, NULL);
    }
    else {
        fputws(msg, stdout); fputws(L"\n", stdout); fflush(stdout);
    }
}

static __inline BOOL elapsed_ge(DWORD start, DWORD now, DWORD ms) {
    return (DWORD)(now - start) >= ms; // DWORDラップ安全
}

// ---- Upを確定（注入）----
static void inject_left_up(void) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

// ---- Up確定タイマー（RECONTACT_MS後に1回）----
static VOID CALLBACK UpTimerProc(HWND hwnd, UINT uMsg, UINT_PTR id, DWORD dwTime) {
    if (g_exiting) { if (g_up_timer_id) { KillTimer(NULL, g_up_timer_id); g_up_timer_id = 0; } return; }
    if (g_up_pending && g_pressed_forwarded) {
        g_up_pending = FALSE;
        g_pressed_forwarded = FALSE; // 論理的に解放
        inject_left_up();            // 外部へUpを1回だけ見せる
    }
    if (g_up_timer_id) { KillTimer(NULL, g_up_timer_id); g_up_timer_id = 0; }
}

// ---- 低レベルマウスフック ----
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (g_exiting) return CallNextHookEx(NULL, nCode, wParam, lParam);

        const MSLLHOOKSTRUCT* p = (const MSLLHOOKSTRUCT*)lParam;
        const DWORD now = p->time;

        // 自分が注入したUpは処理対象外（再帰防止）
        if (p->flags & LLMHF_INJECTED) {
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        switch (wParam) {
        case WM_LBUTTONDOWN:
            if (g_up_pending) {
                // Up確定待ち中にDown = 瞬断（Up/Downとも無視して押下継続）
                g_up_pending = FALSE;
                if (g_up_timer_id) { KillTimer(NULL, g_up_timer_id); g_up_timer_id = 0; }
                return 1; // このDownは食う
            }
            if (!g_pressed_forwarded) {
                g_pressed_forwarded = TRUE;
                g_down_time_ms = now; // 正規のDown開始
            }
            else {
                // 既に押下中の重複Downは常に無視
                return 1;
            }
            break;

        case WM_LBUTTONUP:
            if (!g_pressed_forwarded) {
                // 対応Downを外へ通していないUpは無視
                return 1;
            }
            // 【チャタリング対策】Down→Up < CHATTER_MS のUpは即時無視（遅延確定もしない）
            if (!elapsed_ge(g_down_time_ms, now, CHATTER_MS)) {
                return 1;
            }
            // 【接触不良対策】Upは常に遅延確定：RECONTACT_MS内にDownが来なければUp注入
            g_up_pending = TRUE;
            g_up_cand_time_ms = now;
            if (g_up_timer_id) { KillTimer(NULL, g_up_timer_id); g_up_timer_id = 0; }
            g_up_timer_id = SetTimer(NULL, 1, RECONTACT_MS, UpTimerProc);
            return 1; // 今回のUpは食う（確定はタイマーで）
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---- Ctrl+C / コンソール×ボタン等 ----
static BOOL WINAPI ConsoleCtrlHandler(DWORD ev) {
    // まず素通しモードへ（カクつき最小化）
    InterlockedExchange(&g_exiting, 1);
    switch (ev) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        PostQuitMessage(0); // 通常経路で終了
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // 強制終了はキー待ちができないため、メッセージだけ出す
        print_line(L"終了しました。");
        if (g_up_timer_id) { KillTimer(NULL, g_up_timer_id); g_up_timer_id = 0; }
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
        return FALSE; // 既定処理へ（即終了の可能性）
    }
    return FALSE;
}

int wmain(void) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
    if (!g_hook) {
        fwprintf(stderr, L"[MouseLLDebounce] SetWindowsHookExW failed: %lu\n", GetLastError());
        return 1;
    }

    // 起動時に1回だけ表示
    wchar_t banner[128];
    swprintf(banner, 128, L"チャタリング監視中... (CHATTER=%dms, RECONTACT=%dms)", (int)CHATTER_MS, (int)RECONTACT_MS);
    print_line(banner);

    // 常駐（ウィンドウ無し）
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 通常終了処理（Ctrl+C 等でここに来る）
    if (g_up_timer_id) { KillTimer(NULL, g_up_timer_id); g_up_timer_id = 0; }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }

    print_line(L"終了しました。何かキーを押すと終了します...");
    _getwch(); // ← キー1回待ち（非エコー）

    return 0;
}
