// mouse_ll_debounce_console_3btn.c
// WH_MOUSE_LL で 左・中・右ボタンのチャタリング/瞬断対策（3ボタン統合版）
//
// ・Down→Up < CHATTER_MS はUpを無視（チャタリング）
// ・Upは常に RECONTACT_MS 遅延確定。遅延中にDownが来たら瞬断扱いで Up/Down 両方無視（押下継続）
// ・押下中の重複Downは常に無視
// ・起動: 「チャタリング監視中...」、通常終了: 「終了しました。何かキーを押すと終了します...」
//   ※コンソール×ボタン等の強制終了ではキー待ちはできません（OSが即終了）
//
// ビルド例(Release/x64, Unicode, /O2, user32.lib):
//   cl /O2 /W4 /DUNICODE /D_UNICODE mouse_ll_debounce_console_3btn.c user32.lib
// 閾値変更（例）：/DCHATTER_MS=80 /DRECONTACT_MS=30

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <conio.h>

#ifndef CHATTER_MS
#define CHATTER_MS   100
#endif
#ifndef RECONTACT_MS
#define RECONTACT_MS 30
#endif

static HHOOK g_hook = NULL;
static volatile LONG g_exiting = 0;

typedef struct ButtonState {
    BOOL     pressed_forwarded; // このボタンのDownを外へ通したか（押下中フラグ）
    BOOL     up_pending;        // Up遅延確定待ち中か
    DWORD    down_time_ms;      // 通したDownの時刻
    DWORD    up_cand_time_ms;   // 参考情報（ログ用など）
    UINT_PTR timer_id;          // SetTimer ID（ボタンごとに固有IDを割り当て）
} ButtonState;

enum { BTN_LEFT = 0, BTN_MIDDLE = 1, BTN_RIGHT = 2, BTN_COUNT = 3 };
static ButtonState g_btn[BTN_COUNT] = { 0 };

// --- 1行出力（UTF-16 / 低コスト / リダイレクト時はwprintf） ---
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

// --- 指定ボタンのUpを注入 ---
static void inject_up(int btn) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    switch (btn) {
    case BTN_LEFT:   in.mi.dwFlags = MOUSEEVENTF_LEFTUP;   break;
    case BTN_MIDDLE: in.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
    case BTN_RIGHT:  in.mi.dwFlags = MOUSEEVENTF_RIGHTUP;  break;
    default: return;
    }
    SendInput(1, &in, sizeof(in));
}

// --- Up確定タイマー：タイマーIDからボタンを特定してUp注入 ---
static VOID CALLBACK UpTimerProc(HWND hwnd, UINT uMsg, UINT_PTR id, DWORD dwTime) {
    if (g_exiting) {
        for (int i = 0; i < BTN_COUNT; ++i) {
            if (g_btn[i].timer_id) { KillTimer(NULL, g_btn[i].timer_id); g_btn[i].timer_id = 0; }
        }
        return;
    }
    for (int i = 0; i < BTN_COUNT; ++i) {
        if (g_btn[i].timer_id == id) {
            ButtonState* s = &g_btn[i];
            s->timer_id = 0;
            if (s->up_pending && s->pressed_forwarded) {
                s->up_pending = FALSE;
                s->pressed_forwarded = FALSE; // 論理的に解放
                inject_up(i);                 // Upを1回だけ見せる
            }
            break;
        }
    }
}

// --- WM_* と ボタンindexのマッピング ---
static int msg_to_btn(WPARAM wParam) {
    switch (wParam) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return BTN_LEFT;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return BTN_MIDDLE;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return BTN_RIGHT;
    default: return -1;
    }
}
static BOOL is_down_msg(WPARAM wParam) {
    return (wParam == WM_LBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_RBUTTONDOWN);
}
static BOOL is_up_msg(WPARAM wParam) {
    return (wParam == WM_LBUTTONUP || wParam == WM_MBUTTONUP || wParam == WM_RBUTTONUP);
}

// --- 低レベルマウスフック ---
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (g_exiting) return CallNextHookEx(NULL, nCode, wParam, lParam);

        const MSLLHOOKSTRUCT* p = (const MSLLHOOKSTRUCT*)lParam;
        const DWORD now = p->time;

        // 自分（または他プロセス）注入の入力は対象外：再帰や他ツール干渉を避ける
        if (p->flags & LLMHF_INJECTED) {
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        int btn = msg_to_btn(wParam);
        if (btn >= 0) {
            ButtonState* s = &g_btn[btn];

            if (is_down_msg(wParam)) {
                if (s->up_pending) {
                    // Up確定待ち中にDown = 瞬断（Up/Downとも無視して押下継続）
                    s->up_pending = FALSE;
                    if (s->timer_id) { KillTimer(NULL, s->timer_id); s->timer_id = 0; }
                    return 1; // このDownは食う
                }
                if (!s->pressed_forwarded) {
                    s->pressed_forwarded = TRUE;
                    s->down_time_ms = now; // 正規のDown開始
                }
                else {
                    // 押下中の重複Downは常に無視
                    return 1;
                }
            }
            else if (is_up_msg(wParam)) {
                if (!s->pressed_forwarded) {
                    // 対応Downを通していないUpは無視
                    return 1;
                }
                // チャタリング：Down→Up < CHATTER_MS は即捨て
                if (!elapsed_ge(s->down_time_ms, now, CHATTER_MS)) {
                    return 1;
                }
                // Upは常に遅延確定：RECONTACT_MS 内にDownが来なければUp注入
                s->up_pending = TRUE;
                s->up_cand_time_ms = now;
                if (s->timer_id) { KillTimer(NULL, s->timer_id); s->timer_id = 0; }
                // タイマーIDはボタン固有（1,2,3）で運用
                UINT_PTR tid = (btn == BTN_LEFT) ? 1 : (btn == BTN_MIDDLE ? 2 : 3);
                s->timer_id = SetTimer(NULL, tid, RECONTACT_MS, UpTimerProc);
                return 1; // 今回のUpは食う（確定はタイマーで）
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// --- コンソール制御（Ctrl+C / ×ボタンなど） ---
static BOOL WINAPI ConsoleCtrlHandler(DWORD ev) {
    InterlockedExchange(&g_exiting, 1); // まず素通しへ
    switch (ev) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        PostQuitMessage(0);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        print_line(L"終了しました。");
        for (int i = 0; i < BTN_COUNT; ++i) {
            if (g_btn[i].timer_id) { KillTimer(NULL, g_btn[i].timer_id); g_btn[i].timer_id = 0; }
        }
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
        return FALSE; // 既定処理へ
    }
    return FALSE;
}

int wmain(void) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
    if (!g_hook) {
        fwprintf(stderr, L"[MouseLLDebounce3] SetWindowsHookExW failed: %lu\n", GetLastError());
        return 1;
    }

    wchar_t banner[160];
    swprintf(banner, 160, L"チャタリング監視中...（対象: 左/中/右 / CHATTER=%dms, RECONTACT=%dms）",
        (int)CHATTER_MS, (int)RECONTACT_MS);
    print_line(banner);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (int i = 0; i < BTN_COUNT; ++i) {
        if (g_btn[i].timer_id) { KillTimer(NULL, g_btn[i].timer_id); g_btn[i].timer_id = 0; }
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }

    print_line(L"終了しました。何かキーを押すと終了します...");
    _getwch();
    return 0;
}
