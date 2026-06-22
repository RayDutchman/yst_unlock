/*
 * gui.c — GUI 层
 *
 * 包含：进度窗口、主窗口（已拆子函数）、扩展名映射对话框、消息泵
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "decrypt.h"
#include "gui.h"

/* ── DPI 支持 ─────────────────────────────────────────────── */

/* 当前窗口 DPI（进程启动时从系统获取，WM_DPICHANGED 时更新） */
static UINT g_dpi = 96;

/* 按 DPI 缩放像素值（所有硬编码的像素常量都应通过此宏转换） */
#define S(n) MulDiv((n), g_dpi, 96)

/* 创建与系统 UI 一致的字体
 * SPI_GETNONCLIENTMETRICS 返回的 lfMessageFont.lfHeight 已经是当前系统 DPI
 * 下的像素值，直接用 CreateFontIndirectW 即可，不需要再做 DPI 缩放。 */
static HFONT make_gui_font(void) {
    NONCLIENTMETRICSW ncm = {0};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

/* 将字体应用到所有子控件（并销毁旧字体） */
static void apply_font_to_children(HWND hwnd, HFONT hNew, HFONT *pOld) {
    if (*pOld) DeleteObject(*pOld);
    *pOld = hNew;
    HWND hc = GetWindow(hwnd, GW_CHILD);
    while (hc) {
        SendMessageW(hc, WM_SETFONT, (WPARAM)hNew, TRUE);
        hc = GetWindow(hc, GW_HWNDNEXT);
    }
}

/* ── 全局控件句柄（声明在 gui.h） ─────────────────────────── */
HWND g_hwnd          = NULL;
HWND g_hwndList      = NULL;
HWND g_hwndLog       = NULL;
HWND g_hwndProgress  = NULL;
HWND g_hwndProcEdit  = NULL;
HWND g_hwndOutDir    = NULL;
HWND g_hwndBtnDecrypt= NULL;

wchar_t **g_paths    = NULL;
int       g_path_cnt = 0;

/* ── 进度窗口全局 ──────────────────────────────────────────── */
/* PROG_W / PROG_H 是 96dpi 基准尺寸，运行时通过 S() 缩放 */
#define PROG_W 480
#define PROG_H 260

static HWND     g_prog_hwnd   = NULL;
static HWND     g_prog_bar    = NULL;
static HWND     g_prog_log    = NULL;
static HWND     g_prog_status = NULL;
static HWND     g_prog_btn    = NULL;
static UINT_PTR g_prog_timer  = 0;
static HANDLE   g_prog_thread = NULL;
static HANDLE   g_prog_stop_event = NULL;
static HFONT    g_prog_font   = NULL;  /* 进度窗口字体（WM_DPICHANGED 时重建） */

/* ── 进度窗口 ─────────────────────────────────────────────── */

static LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(NULL);
        int W = S(PROG_W), H = S(PROG_H);
        g_prog_status = CreateWindowW(L"STATIC", L"准备中...",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            S(10), S(8), W-S(20), S(20), hwnd, NULL, hInst, NULL);
        g_prog_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            S(10), S(34), W-S(20), S(18), hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);
        SendMessageW(g_prog_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        g_prog_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            S(10), S(58), W-S(20), H-S(58)-S(38), hwnd, (HMENU)IDC_LOG, hInst, NULL);
        SendMessageW(g_prog_log, EM_LIMITTEXT, 0x7FFFFFFE, 0);
        g_prog_btn = CreateWindowW(L"BUTTON", L"关闭",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_DISABLED,
            W/2-S(40), H-S(34), S(80), S(26), hwnd, (HMENU)IDC_BTN_DECRYPT, hInst, NULL);
        apply_font_to_children(hwnd, make_gui_font(), &g_prog_font);
        SetTimer(hwnd, 1, 100, NULL);
        break;
    }
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            g_prog_stop_event = NULL;
            g_prog_thread = start_decrypt_thread(hwnd, g_paths, g_path_cnt, NULL, NULL, NULL, &g_prog_stop_event);
            if (!g_prog_thread) {
                SetWindowTextW(g_prog_status, L"启动解密线程失败");
                EnableWindow(g_prog_btn, TRUE);
            }
        }
        break;
    case WM_WORKER_LOG: {
        wchar_t *s = (wchar_t*)lp;
        if (s) {
            int len = GetWindowTextLengthW(g_prog_log);
            SendMessageW(g_prog_log, EM_SETSEL, len, len);
            SendMessageW(g_prog_log, EM_REPLACESEL, FALSE, (LPARAM)s);
            SendMessageW(g_prog_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
            free(s);
        }
        return 0;
    }
    case WM_WORKER_PROG:
        SendMessageW(g_prog_bar, PBM_SETPOS, wp, 0);
        {
            wchar_t buf[64];
            _snwprintf(buf, 63, L"解密中... %d%%", (int)wp);
            buf[63] = 0;
            SetWindowTextW(g_prog_status, buf);
        }
        return 0;
    case WM_WORKER_DONE:
        SetWindowTextW(g_prog_status, wp ? L"解密完成" : L"解密完成（有失败项）");
        SendMessageW(g_prog_bar, PBM_SETPOS, 100, 0);
        EnableWindow(g_prog_btn, TRUE);
        return 0;
    case WM_CLOSE:
        if (g_prog_thread) {
            /* 先发停止信号，让解密线程自行清理 worker 子进程 */
            if (g_prog_stop_event) SetEvent(g_prog_stop_event);
            /* 等待线程自行退出（最长 30 秒，保障 worker 子进程被回收） */
            ULONGLONG deadline = GetTickCount64() + 30000;
            while (WaitForSingleObject(g_prog_thread, 0) == WAIT_TIMEOUT) {
                if (GetTickCount64() >= deadline) break;
                /* 用 MsgWaitForMultipleObjects 同时等线程和消息，避免界面卡死 */
                DWORD wr = MsgWaitForMultipleObjects(1, &g_prog_thread, FALSE,
                                                      100, QS_ALLINPUT);
                if (wr == WAIT_OBJECT_0 + 1) {
                    MSG m;
                    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); goto close_done; }
                        TranslateMessage(&m); DispatchMessageW(&m);
                    }
                }
            }
            close_done:
            CloseHandle(g_prog_thread);
            g_prog_thread = NULL;
        }
        if (g_prog_stop_event) { CloseHandle(g_prog_stop_event); g_prog_stop_event = NULL; }
        DestroyWindow(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_DECRYPT) {
            if (g_prog_timer) KillTimer(hwnd, g_prog_timer);
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_DPICHANGED: {
        /* 进度窗口跨显示器：更新 DPI、重建字体、按建议 RECT 移动并重设客户区大小 */
        g_dpi = HIWORD(wp);
        apply_font_to_children(hwnd, make_gui_font(), &g_prog_font);
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        /* 重新定位所有子控件 */
        int W = S(PROG_W), H = S(PROG_H);
        SetWindowPos(g_prog_status, NULL, S(10), S(8),  W-S(20), S(20),  SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_prog_bar,    NULL, S(10), S(34), W-S(20), S(18),  SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_prog_log,    NULL, S(10), S(58), W-S(20), H-S(58)-S(38), SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_prog_btn,    NULL, W/2-S(40), H-S(34), S(80), S(26), SWP_NOZORDER|SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        if (g_prog_font) { DeleteObject(g_prog_font); g_prog_font = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void run_progress_window(wchar_t **paths, int n) {
    g_paths    = paths;
    g_path_cnt = n;

    /* 获取系统 DPI（进度窗口无父窗口，用系统 DPI） */
    {
        HDC hdc = GetDC(NULL);
        g_dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
    }

    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpszClassName = L"YST_ProgressWnd";
    wc.lpfnWndProc   = ProgressWndProc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    {
        RECT pr = {0, 0, S(PROG_W), S(PROG_H)};
        AdjustWindowRect(&pr, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE);
        int pw = pr.right - pr.left;
        int ph = pr.bottom - pr.top;
        g_prog_hwnd = CreateWindowExW(0, L"YST_ProgressWnd", L"YST unlock",
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            (sw-pw)/2, (sh-ph)/2, pw, ph,
            NULL, NULL, hInst, NULL);
    }
    ShowWindow(g_prog_hwnd, SW_SHOW);
    UpdateWindow(g_prog_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ── 主窗口：布局常量（96dpi 基准，运行时通过 S() 缩放） ──── */

/* 初始客户区尺寸（96dpi 基准） */
#define GUI_CLIENT_W   500
#define GUI_CLIENT_H   560
/* 最小客户区尺寸（96dpi 基准） */
#define GUI_MIN_CW     400
#define GUI_MIN_CH     560

/* 边距与尺寸（96dpi 基准，全部通过 S() 使用） */
#define LAYOUT_P       8    /* 外边距 */
#define LAYOUT_BW      80   /* 普通按钮宽 */
#define LAYOUT_BH      24   /* 普通按钮高 */
#define LAYOUT_EH      22   /* Edit 高 */
#define LAYOUT_CFG_H   124  /* 配置区固定高（4行，去掉 checkbox 行） */

static HFONT g_main_font = NULL;  /* 主窗口字体（WM_DPICHANGED 时重建） */

/* ── 主窗口：子控件创建辅助函数（Step 4 重构） ─────────────── */

/* 文件列表 GroupBox 内的控件 */
static void create_file_group(HWND hwnd, HINSTANCE hInst,
                               int y, int gw, int iw,
                               HWND *hGbFiles, HWND *hBtnAddFile,
                               HWND *hBtnAddDir, HWND *hListBox) {
    const int P = S(LAYOUT_P), BW = S(LAYOUT_BW), BH = S(LAYOUT_BH);

    *hGbFiles = CreateWindowW(L"BUTTON", L"待解密文件 / 文件夹（可拖入）",
        WS_CHILD|WS_VISIBLE|BS_GROUPBOX|WS_CLIPCHILDREN,
        P, y, gw, S(196), hwnd, NULL, hInst, NULL);

    *hBtnAddFile = CreateWindowW(L"BUTTON", L"添加文件",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P+S(8), y+S(18), BW, BH, hwnd, (HMENU)IDC_BTN_ADDFILE, hInst, NULL);
    *hBtnAddDir = CreateWindowW(L"BUTTON", L"添加文件夹",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P+S(8)+BW+S(8), y+S(18), BW, BH, hwnd, (HMENU)IDC_BTN_ADDDIR, hInst, NULL);
    CreateWindowW(L"BUTTON", L"清空列表",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P+gw-S(8)-BW, y+S(18), BW, BH, hwnd, (HMENU)IDC_BTN_CLEAR, hInst, NULL);

    *hListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|
        LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_EXTENDEDSEL,
        P+S(8), y+S(48), iw, S(136), hwnd, (HMENU)IDC_LISTBOX, hInst, NULL);
}

/* 配置 GroupBox 内的控件（4行，无 checkbox） */
static void create_config_group(HWND hwnd, HINSTANCE hInst,
                                 int y, int gw, int iw,
                                 HWND *hGbCfg,
                                 HWND *hProcEdit,   HWND *hStaticProc,
                                 HWND *hStaticOutLabel, HWND *hOutDir) {
    const int P = S(LAYOUT_P), EH = S(LAYOUT_EH);
    const int LW = S(72);   /* 标签宽，使 Edit 左边对齐：P+S(8)+LW+S(4) */

    *hGbCfg = CreateWindowW(L"BUTTON", L"解密配置",
        WS_CHILD|WS_VISIBLE|BS_GROUPBOX|WS_CLIPCHILDREN,
        P, y, gw, S(LAYOUT_CFG_H), hwnd, NULL, hInst, NULL);

    /* 行1 y+20：覆写进程名（留空 = 自动） */
    *hStaticProc = CreateWindowW(L"STATIC", L"覆写进程名:",
        WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        P+S(8), y+S(20), LW, EH, hwnd, NULL, hInst, NULL);
    *hProcEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        P+S(8)+LW+S(4), y+S(20), S(160), EH, hwnd, (HMENU)IDC_EDIT_PROC, hInst, NULL);
    /* placeholder 灰字提示，Edit 为空时显示，替代原来的 Static hint */
    SendMessageW(*hProcEdit, EM_SETCUEBANNER, FALSE, (LPARAM)L"留空 = 自动匹配");

    /* 行2 y+46：扩展名映射 */
    CreateWindowW(L"STATIC", L"扩展名映射:",
        WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        P+S(8), y+S(46), LW, EH, hwnd, (HMENU)IDC_STATIC_EXTMAP_LBL, hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"已配置 0 条映射",
        WS_CHILD|WS_VISIBLE|ES_READONLY,
        P+S(8)+LW+S(4), y+S(46), S(160), EH, hwnd, (HMENU)IDC_LIST_EXT_MAP, hInst, NULL);
    CreateWindowW(L"BUTTON", L"管理...",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P+S(8)+LW+S(4)+S(160)+S(4), y+S(46), S(60), EH, hwnd, (HMENU)IDC_BTN_EXT_MAP, hInst, NULL);

    /* 行3 y+72：兜底进程名 */
    CreateWindowW(L"STATIC", L"兜底进程名:",
        WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        P+S(8), y+S(72), LW, EH, hwnd, (HMENU)IDC_STATIC_FALLBACK_LBL, hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_fallback_proc,
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        P+S(8)+LW+S(4), y+S(72), S(160), EH, hwnd, (HMENU)IDC_EDIT_FALLBACK, hInst, NULL);
    CreateWindowW(L"STATIC", L"(注册表未命中时)",
        WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        P+S(8)+LW+S(4)+S(160)+S(4), y+S(72), S(120), EH, hwnd, (HMENU)IDC_STATIC_FALLBACK_HINT, hInst, NULL);

    /* 行4 y+98：输出目录（弹性宽） */
    {
        const int bw2 = S(56), gap = S(4);
        const int ew = iw - LW - gap - bw2 - gap;
        *hStaticOutLabel = CreateWindowW(L"STATIC", L"输出目录:",
            WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
            P+S(8), y+S(98), LW, EH, hwnd, NULL, hInst, NULL);
        *hOutDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            P+S(8)+LW+gap, y+S(98), ew, EH, hwnd, (HMENU)IDC_EDIT_OUTDIR, hInst, NULL);
        CreateWindowW(L"BUTTON", L"浏览...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P+S(8)+LW+gap+ew+gap, y+S(98), bw2, EH, hwnd, (HMENU)IDC_BTN_BROWSE, hInst, NULL);
    }
}

/* 日志 GroupBox + 底部按钮 */
static void create_log_group(HWND hwnd, HINSTANCE hInst,
                              int y, int gw, int iw,
                              HWND *hGbLog, HWND *hProg,
                              HWND *hLog, HWND *hBtnDecrypt) {
    const int P = S(LAYOUT_P), BW = S(LAYOUT_BW), BH = S(LAYOUT_BH);
    int log_btn_y = y + S(168);

    *hGbLog = CreateWindowW(L"BUTTON", L"日志",
        WS_CHILD|WS_VISIBLE|BS_GROUPBOX|WS_CLIPCHILDREN,
        P, y, gw, S(164), hwnd, NULL, hInst, NULL);
    *hProg = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
        WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
        P+S(8), y+S(18), iw, S(16), hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);
    SendMessageW(*hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    *hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|
        ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        P+S(8), y+S(40), iw, S(112), hwnd, (HMENU)IDC_LOG, hInst, NULL);
    SendMessageW(*hLog, EM_LIMITTEXT, 0x7FFFFFFE, 0);

    /* 底部按钮行 */
    CreateWindowW(L"BUTTON", L"安装右键菜单",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P, log_btn_y, S(96), BH, hwnd, (HMENU)IDC_BTN_INSTALL, hInst, NULL);
    CreateWindowW(L"BUTTON", L"卸载右键菜单",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P+S(96)+S(6), log_btn_y, S(96), BH, hwnd, (HMENU)IDC_BTN_UNINSTALL, hInst, NULL);
    *hBtnDecrypt = CreateWindowW(L"BUTTON", L"开始解密",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P+gw-BW, log_btn_y, BW, BH, hwnd, (HMENU)IDC_BTN_DECRYPT, hInst, NULL);
}

/* ── 主窗口：WM_COMMAND 处理子函数 ──────────────────────────── */

/* 辅助：向日志追加一行 */
static void log_append(HWND hLog, const wchar_t *s) {
    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

/* 添加路径到列表（去重） */
static void listbox_add_path(HWND hListBox, const wchar_t *path) {
    BOOL dup = FALSE;
    for (int j = 0; j < g_path_cnt; j++)
        if (wcscmp(g_paths[j], path) == 0) { dup = TRUE; break; }
    if (!dup && g_path_cnt < MAX_PATHS) {
        wchar_t *dup = _wcsdup(path);
        if (!dup) return;
        g_paths[g_path_cnt++] = dup;
        SendMessageW(hListBox, LB_ADDSTRING, 0, (LPARAM)path);
    }
}

static void on_btn_addfile(HWND hwnd, HWND hListBox) {
    OPENFILENAMEW ofn = {sizeof(ofn)};
    wchar_t *files = (wchar_t*)calloc(MAX_PATH*32, sizeof(wchar_t));
    if (!files) return;
    ofn.hwndOwner  = hwnd;
    ofn.lpstrFile  = files;
    ofn.nMaxFile   = MAX_PATH*32;
    ofn.lpstrTitle = L"选择文件";
    ofn.Flags      = OFN_ALLOWMULTISELECT|OFN_EXPLORER|OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        wchar_t *dir = files;
        wchar_t *p   = files + wcslen(dir) + 1;
        if (*p == 0) {
            listbox_add_path(hListBox, dir);
        } else {
            while (*p) {
                wchar_t full[MAX_PATH_LEN];
                _snwprintf(full, MAX_PATH_LEN-1, L"%s\\%s", dir, p);
                full[MAX_PATH_LEN-1] = 0;
                listbox_add_path(hListBox, full);
                p += wcslen(p) + 1;
            }
        }
    }
    free(files);
}

static void on_btn_adddir(HWND hwnd, HWND hListBox) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"选择文件夹";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS|BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH_LEN];
        if (SHGetPathFromIDListW(pidl, path))
            listbox_add_path(hListBox, path);
        CoTaskMemFree(pidl);
    }
}

static void on_btn_browse(HWND hwnd, HWND hOutDir) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"选择输出目录";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS|BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH_LEN];
        if (SHGetPathFromIDListW(pidl, path))
            SetWindowTextW(hOutDir, path);
        CoTaskMemFree(pidl);
    }
}

static void on_btn_decrypt(HWND hwnd,
                            HWND hProcEdit,
                            HWND hBtnDecrypt, HWND hBtnAddFile, HWND hBtnAddDir,
                            HWND hProg, HWND hLog, HWND hListBox,
                            BOOL *is_decrypting) {
    if (g_path_cnt == 0) {
        MessageBoxW(hwnd, L"请先添加要解密的文件或文件夹", L"提示",
                    MB_OK|MB_ICONWARNING);
        return;
    }
    /* 覆写进程名：留空=自动匹配，非空=强制使用 */
    wchar_t proc_buf[MAX_PATH_LEN] = {0};
    GetWindowTextW(hProcEdit, proc_buf, MAX_PATH_LEN);

    wchar_t fallback_buf[MAX_PATH_LEN];
    wcsncpy(fallback_buf, g_fallback_proc, MAX_PATH_LEN-1);
    fallback_buf[MAX_PATH_LEN-1] = 0;
    if (!fallback_buf[0]) wcscpy(fallback_buf, L"POWERPNT.EXE");
    wchar_t out_buf[MAX_PATH_LEN] = {0};
    GetWindowTextW(g_hwndOutDir, out_buf, MAX_PATH_LEN);
    if (out_buf[0]) CreateDirectoryW(out_buf, NULL);

     *is_decrypting = TRUE;
     EnableWindow(hBtnDecrypt, FALSE);
     EnableWindow(hBtnAddFile, FALSE);
     EnableWindow(hBtnAddDir,  FALSE);
     EnableWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR),   FALSE);
     EnableWindow(GetDlgItem(hwnd, IDC_BTN_EXT_MAP), FALSE);
     EnableWindow(hListBox, FALSE);
    SendMessageW(hProg, PBM_SETPOS, 0, 0);
    SetWindowTextW(hLog, L"");

    wchar_t mode_buf[512];
    _snwprintf(mode_buf, 511, L"模式：%s  兜底：%s",
               proc_buf[0] ? proc_buf : L"自动", fallback_buf);
    mode_buf[511] = 0;
    log_append(hLog, mode_buf);

    HANDLE ht = start_decrypt_thread(hwnd, g_paths, g_path_cnt,
                         proc_buf[0] ? proc_buf : NULL,
                         fallback_buf,
                         out_buf[0] ? out_buf : NULL,
                         NULL);
    if (ht) CloseHandle(ht);
    else {
        *is_decrypting = FALSE;
        EnableWindow(hBtnDecrypt, TRUE);
        EnableWindow(hBtnAddFile, TRUE);
        EnableWindow(hBtnAddDir,  TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR),   TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_EXT_MAP), TRUE);
        EnableWindow(hListBox, TRUE);
        MessageBoxW(hwnd, L"启动解密线程失败（内存不足）", L"错误", MB_OK|MB_ICONERROR);
    }
}

/* ── 主窗口：WM_SIZE 布局函数 ─────────────────────────────── */

static void layout_main_window(HWND hwnd, int cw, int ch,
                                HWND hGbFiles, HWND hBtnAddFile, HWND hBtnAddDir,
                                HWND hGbCfg,
                                HWND hStaticProc,
                                HWND hStaticOutLabel,
                                HWND hGbLog) {
    const int P      = S(LAYOUT_P);
    const int BH     = S(LAYOUT_BH);
    const int EH     = S(LAYOUT_EH);
    const int BW     = S(LAYOUT_BW);
    const int LW     = S(72);   /* 标签宽 */
    const int GW     = cw - 2*P;
    const int IW     = GW - S(16);
    const int BTN_H  = BH + S(10);   /* 底部按钮行预留高度 */
    const int CFG_H  = S(LAYOUT_CFG_H);

    /* 文件列表高度：客户区 35%，最小 S(80) */
    int list_h = ch * 35 / 100;
    if (list_h < S(80)) list_h = S(80);
    int gb1_h = S(48) + list_h;
    int y = S(4);

    /* 文件 GroupBox */
    SetWindowPos(hGbFiles, NULL, P, y, GW, gb1_h, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(hBtnAddFile, NULL, P+S(8), y+S(18), BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(hBtnAddDir,  NULL, P+S(8)+BW+S(8), y+S(18), BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(hwnd, IDC_BTN_CLEAR), NULL, P+GW-S(8)-BW, y+S(18), BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g_hwndList, NULL, P+S(8), y+S(48), IW, list_h, SWP_NOZORDER|SWP_NOACTIVATE);

    /* 配置 GroupBox */
    y += gb1_h + S(4);
    SetWindowPos(hGbCfg, NULL, P, y, GW, CFG_H, SWP_NOZORDER|SWP_NOACTIVATE);
    /* 行1：覆写进程名 */
    SetWindowPos(hStaticProc,      NULL, P+S(8),         y+S(20), LW,    EH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g_hwndProcEdit,   NULL, P+S(8)+LW+S(4), y+S(20), S(160),EH, SWP_NOZORDER|SWP_NOACTIVATE);
    /* 行2：扩展名映射 */
    SetWindowPos(GetDlgItem(hwnd, IDC_STATIC_EXTMAP_LBL), NULL,
        P+S(8),                       y+S(46), LW,    EH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(hwnd, IDC_LIST_EXT_MAP), NULL,
        P+S(8)+LW+S(4),               y+S(46), S(160),EH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(hwnd, IDC_BTN_EXT_MAP), NULL,
        P+S(8)+LW+S(4)+S(160)+S(4),   y+S(46), S(60), EH, SWP_NOZORDER|SWP_NOACTIVATE);
    /* 行3：兜底进程名 */
    SetWindowPos(GetDlgItem(hwnd, IDC_STATIC_FALLBACK_LBL), NULL,
        P+S(8),                       y+S(72), LW,    EH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(hwnd, IDC_EDIT_FALLBACK), NULL,
        P+S(8)+LW+S(4),               y+S(72), S(160),EH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(hwnd, IDC_STATIC_FALLBACK_HINT), NULL,
        P+S(8)+LW+S(4)+S(160)+S(4),  y+S(72), S(120),EH, SWP_NOZORDER|SWP_NOACTIVATE);
    /* 行4：输出目录（弹性宽） */
    {
        const int bw2 = S(56), gap = S(4);
        int ew = IW - LW - gap - bw2 - gap;
        if (ew < S(60)) ew = S(60);
        SetWindowPos(hStaticOutLabel, NULL, P+S(8),           y+S(98), LW,  EH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_hwndOutDir,    NULL, P+S(8)+LW+gap,    y+S(98), ew,  EH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd, IDC_BTN_BROWSE), NULL,
            P+S(8)+LW+gap+ew+gap, y+S(98), bw2, EH, SWP_NOZORDER|SWP_NOACTIVATE);
    }

    /* 日志 GroupBox（填满剩余） */
    y += CFG_H + S(4);
    int log_gb_h = ch - y - BTN_H - S(4);
    if (log_gb_h < S(60)) log_gb_h = S(60);
    SetWindowPos(hGbLog, NULL, P, y, GW, log_gb_h, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g_hwndProgress, NULL, P+S(8), y+S(18), IW, S(16), SWP_NOZORDER|SWP_NOACTIVATE);
    int log_edit_h = log_gb_h - S(40) - S(8);
    if (log_edit_h < S(30)) log_edit_h = S(30);
    SetWindowPos(g_hwndLog, NULL, P+S(8), y+S(40), IW, log_edit_h, SWP_NOZORDER|SWP_NOACTIVATE);

    /* 底部按钮行 */
    int by = ch - BH - S(10);
    SetWindowPos(GetDlgItem(hwnd, IDC_BTN_INSTALL),   NULL, P,          by, S(96), BH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(hwnd, IDC_BTN_UNINSTALL), NULL, P+S(96)+S(6), by, S(96), BH, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g_hwndBtnDecrypt, NULL, P+GW-BW, by, BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);

    InvalidateRect(hwnd, NULL, TRUE);
}

/* ── 主窗口消息处理 ──────────────────────────────────────── */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    /* 需要在 WM_SIZE 里移动的控件句柄（static 保留跨消息） */
    static HWND hLog=NULL, hProg=NULL;
    static HWND hGbFiles=NULL, hGbCfg=NULL, hGbLog=NULL;
    static HWND hBtnAddFile=NULL, hBtnAddDir=NULL;
    static HWND hBtnDecrypt=NULL;
    static HWND hStaticProc=NULL, hStaticOutLabel=NULL;
    static BOOL is_decrypting = FALSE;

    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(NULL);
        const int P  = S(LAYOUT_P);
        const int GW = S(GUI_CLIENT_W) - 2*P;
        const int IW = GW - S(16);
        int y = S(4);

        /* 文件列表区 */
        HWND hListBox = NULL;
        create_file_group(hwnd, hInst, y, GW, IW,
                          &hGbFiles, &hBtnAddFile, &hBtnAddDir, &hListBox);

        /* 配置区 */
        HWND hProcEdit=NULL, hOutDir=NULL;
        y += S(200);
        create_config_group(hwnd, hInst, y, GW, IW,
                            &hGbCfg,
                            &hProcEdit, &hStaticProc,
                            &hStaticOutLabel, &hOutDir);

        /* 日志区 + 底部按钮 */
        y += S(96);
        create_log_group(hwnd, hInst, y, GW, IW,
                         &hGbLog, &hProg, &hLog, &hBtnDecrypt);

        /* 统一应用 DPI 感知字体 */
        apply_font_to_children(hwnd, make_gui_font(), &g_main_font);

        /* 发布到全局句柄 */
        g_hwndLog      = hLog;
        g_hwndProgress = hProg;
        g_hwndProcEdit = hProcEdit;
        g_hwndOutDir   = hOutDir;
        g_hwndBtnDecrypt = hBtnDecrypt;
        g_hwndList     = hListBox;

        /* ListBox 行高适配字体 */
        {
            HDC hdc = GetDC(hListBox);
            HFONT hOldF = (HFONT)SelectObject(hdc, g_main_font);
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, hOldF);
            ReleaseDC(hListBox, hdc);
            SendMessageW(hListBox, LB_SETITEMHEIGHT, 0, tm.tmHeight + 2);
        }

        /* 初始化摘要 / 兜底进程名 */
        {
            wchar_t summary[128];
            _snwprintf(summary, 127, L"已配置 %d 条映射", g_ext_map_cnt);
            summary[127] = 0;
            SetWindowTextW(GetDlgItem(hwnd, IDC_LIST_EXT_MAP), summary);
        }
        SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_FALLBACK), g_fallback_proc);

        /* 触发首次 WM_SIZE 以应用初始布局 */
        RECT cr; GetClientRect(hwnd, &cr);
        SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(cr.right, cr.bottom));
        break;
    }

    case WM_GETMINMAXINFO: {
        RECT rc = {0, 0, S(GUI_MIN_CW), S(GUI_MIN_CH)};
        AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_ACCEPTFILES);
        MINMAXINFO *mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = rc.right - rc.left;
        mm->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }

    case WM_CONTEXTMENU: {
        if ((HWND)wp != g_hwndList) break;
        if (is_decrypting) break;
        if (SendMessageW(g_hwndList, LB_GETCOUNT, 0, 0) <= 0) break;

        int sx = (short)LOWORD(lp);
        int sy = (short)HIWORD(lp);
        int hit_idx = -1;
        if (sx == -1 && sy == -1) {
            int total_k = (int)SendMessageW(g_hwndList, LB_GETCOUNT, 0, 0);
            for (int ci = 0; ci < total_k; ci++)
                if (SendMessageW(g_hwndList, LB_GETSEL, ci, 0) > 0) { hit_idx = ci; break; }
            if (hit_idx < 0) hit_idx = 0;
            RECT rc; GetWindowRect(g_hwndList, &rc);
            sx = rc.left; sy = rc.top;
        } else {
            POINT pt = {sx, sy};
            ScreenToClient(g_hwndList, &pt);
            LRESULT res = SendMessageW(g_hwndList, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
            if (HIWORD(res) == 0) hit_idx = (int)LOWORD(res);
        }
        if (hit_idx < 0) break;

        SendMessageW(g_hwndList, LB_SETSEL, FALSE, -1);
        SendMessageW(g_hwndList, LB_SETSEL, TRUE, hit_idx);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, 1, L"删除");
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD|TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
        DestroyMenu(hMenu);
        if (cmd == 1) {
            int total = (int)SendMessageW(g_hwndList, LB_GETCOUNT, 0, 0);
            for (int i = total - 1; i >= 0; i--) {
                if (SendMessageW(g_hwndList, LB_GETSEL, i, 0) > 0) {
                    SendMessageW(g_hwndList, LB_DELETESTRING, i, 0);
                    free(g_paths[i]);
                    for (int j = i; j < g_path_cnt - 1; j++)
                        g_paths[j] = g_paths[j+1];
                    g_paths[--g_path_cnt] = NULL;
                }
            }
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hdrop = (HDROP)wp;
        UINT cnt = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < cnt; i++) {
            wchar_t path[MAX_PATH_LEN];
            if (DragQueryFileW(hdrop, i, path, MAX_PATH_LEN))
                listbox_add_path(g_hwndList, path);
        }
        DragFinish(hdrop);
        return 0;
    }

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        layout_main_window(hwnd, LOWORD(lp), HIWORD(lp),
                           hGbFiles, hBtnAddFile, hBtnAddDir,
                           hGbCfg,
                           hStaticProc, hStaticOutLabel,
                           hGbLog);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BTN_ADDFILE) {
            on_btn_addfile(hwnd, g_hwndList);
        } else if (id == IDC_BTN_ADDDIR) {
            on_btn_adddir(hwnd, g_hwndList);
        } else if (id == IDC_BTN_CLEAR) {
            for (int i=0; i<g_path_cnt; i++) free(g_paths[i]);
            g_path_cnt = 0;
            SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
        } else if (id == IDC_BTN_BROWSE) {
            on_btn_browse(hwnd, g_hwndOutDir);
        } else if (id == IDC_EDIT_FALLBACK &&
                   HIWORD(wp) == EN_KILLFOCUS) {
            HWND hFb = GetDlgItem(hwnd, IDC_EDIT_FALLBACK);
            GetWindowTextW(hFb, g_fallback_proc, MAX_PATH_LEN);
            if (!g_fallback_proc[0]) wcscpy(g_fallback_proc, L"POWERPNT.EXE");
            save_ini();
        } else if (id == IDC_BTN_EXT_MAP) {
            show_ext_map_dialog(hwnd);
            {
                wchar_t summary[128];
                _snwprintf(summary, 127, L"已配置 %d 条映射", g_ext_map_cnt);
                summary[127] = 0;
                SetWindowTextW(GetDlgItem(hwnd, IDC_LIST_EXT_MAP), summary);
            }
        } else if (id == IDC_BTN_INSTALL) {
            wchar_t msg2[256];
            install_context_menu(msg2, 256);
            MessageBoxW(hwnd, msg2, L"右键菜单", MB_OK|MB_ICONINFORMATION);
        } else if (id == IDC_BTN_UNINSTALL) {
            wchar_t msg2[256];
            uninstall_context_menu(msg2, 256);
            MessageBoxW(hwnd, msg2, L"右键菜单", MB_OK|MB_ICONINFORMATION);
        } else if (id == IDC_BTN_DECRYPT && !is_decrypting) {
            on_btn_decrypt(hwnd, g_hwndProcEdit,
                           hBtnDecrypt, hBtnAddFile, hBtnAddDir,
                           hProg, hLog, g_hwndList,
                           &is_decrypting);
        }
        return 0;
    }

    case WM_WORKER_LOG: {
        wchar_t *s = (wchar_t*)lp;
        if (s && hLog) { log_append(hLog, s); free(s); }
        return 0;
    }
    case WM_WORKER_PROG:
        if (hProg) SendMessageW(hProg, PBM_SETPOS, wp, 0);
        return 0;
    case WM_WORKER_DONE:
        is_decrypting = FALSE;
        if (hBtnDecrypt) EnableWindow(hBtnDecrypt, TRUE);
        EnableWindow(hBtnAddFile, TRUE);
        EnableWindow(hBtnAddDir,  TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR),   TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_EXT_MAP), TRUE);
        EnableWindow(g_hwndList, TRUE);
        return 0;

    case WM_CLOSE:
        if (is_decrypting) {
            int r = MessageBoxW(hwnd, L"解密正在进行中，确定要退出吗？\n退出后当前批次将中断。",
                                L"确认退出", MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);
            if (r != IDYES) return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DPICHANGED: {
        /* 更新 DPI、重建字体，按 Windows 建议的 RECT 移动 + 缩放窗口 */
        g_dpi = HIWORD(wp);
        apply_font_to_children(hwnd, make_gui_font(), &g_main_font);
        /* 更新 ListBox 行高 */
        {
            HDC hdc = GetDC(g_hwndList);
            HFONT hOld = (HFONT)SelectObject(hdc, g_main_font);
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, hOld);
            ReleaseDC(g_hwndList, hdc);
            SendMessageW(g_hwndList, LB_SETITEMHEIGHT, 0, tm.tmHeight + 2);
        }
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        /* WM_SIZE 会由 SetWindowPos 自动触发，重排布局 */
        return 0;
    }

    case WM_DESTROY:
        if (g_main_font) { DeleteObject(g_main_font); g_main_font = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void run_main_gui(wchar_t **init_paths, int n) {
    HINSTANCE hInst = GetModuleHandleW(NULL);

    /* 获取主显示器 DPI */
    {
        HDC hdc = GetDC(NULL);
        g_dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpszClassName = L"YST_MainWnd";
    wc.lpfnWndProc   = MainWndProc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    DWORD wstyle = WS_OVERLAPPEDWINDOW;
    RECT rc2 = {0, 0, S(GUI_CLIENT_W), S(GUI_CLIENT_H)};
    AdjustWindowRectEx(&rc2, wstyle, FALSE, WS_EX_ACCEPTFILES);
    int fw = rc2.right - rc2.left;
    int fh = rc2.bottom - rc2.top;
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"YST_MainWnd", APP_TITLE,
        wstyle, (sw-fw)/2, (sh-fh)/2, fw, fh,
        NULL, NULL, hInst, NULL);

    ChangeWindowMessageFilterEx(g_hwnd, WM_DROPFILES, MSGFLT_ALLOW, NULL);
    /* WM_COPYDATA / 0x0049 无 handler，不需要 filter allow */
    DragAcceptFiles(g_hwnd, TRUE);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (init_paths) {
        for (int i = 0; i < n; i++) {
            if (g_path_cnt < MAX_PATHS) {
                wchar_t *dup = _wcsdup(init_paths[i]);
                if (!dup) continue;
                g_paths[g_path_cnt++] = dup;
                SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)init_paths[i]);
            }
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ── 扩展名映射管理对话框 ─────────────────────────────────────── */

#define EXTDLG_W 330
#define EXTDLG_H 360
#define IDC_EXTDLG_LIST   201
#define IDC_EXTDLG_EXT    202
#define IDC_EXTDLG_PROC   203
#define IDC_EXTDLG_ADD    204
#define IDC_EXTDLG_DEL    205
#define IDC_EXTDLG_OK     206

static HFONT g_extdlg_font = NULL;  /* 对话框字体 */

static void extdlg_refresh_list(HWND hList) {
    ListView_DeleteAllItems(hList);
    for (int i = 0; i < g_ext_map_cnt; i++) {
        LVITEMW lvi = {0};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = i;
        lvi.iSubItem= 0;
        lvi.pszText = g_ext_map[i].ext;
        ListView_InsertItem(hList, &lvi);
        ListView_SetItemText(hList, i, 1, g_ext_map[i].proc);
    }
}

static void extdlg_reposition_children(HWND hwnd) {
    int W = S(EXTDLG_W), H = S(EXTDLG_H);
    HWND hList = GetDlgItem(hwnd, IDC_EXTDLG_LIST);
    if (hList) {
        SetWindowPos(hList, NULL, S(8), S(8), W-S(16), H-S(98), SWP_NOZORDER);
        ListView_SetColumnWidth(hList, 0, S(120));
        ListView_SetColumnWidth(hList, 1, W-S(16)-S(120)-S(4));
    }
    /* 子控件按 Z-order 遍历定位（两个 STATIC 无 ID，无法用 GetDlgItem） */
    HWND hChild = GetWindow(hList, GW_HWNDNEXT);
    if (hChild) {
        SetWindowPos(hChild, NULL, S(8), H-S(84), S(110), S(18), SWP_NOZORDER);
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
    if (hChild) {
        SetWindowPos(hChild, NULL, S(124), H-S(84), S(140), S(18), SWP_NOZORDER);
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
    if (hChild) {
        SetWindowPos(hChild, NULL, S(8), H-S(66), S(112), S(22), SWP_NOZORDER);
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
    if (hChild) {
        SetWindowPos(hChild, NULL, S(124), H-S(66), W-S(124)-S(86)-S(8), S(22), SWP_NOZORDER);
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
    if (hChild) {
        SetWindowPos(hChild, NULL, W-S(82), H-S(66), S(74), S(22), SWP_NOZORDER);
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
    if (hChild) {
        SetWindowPos(hChild, NULL, S(8), H-S(36), S(90), S(26), SWP_NOZORDER);
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }
    if (hChild) {
        SetWindowPos(hChild, NULL, W-S(90), H-S(36), S(82), S(26), SWP_NOZORDER);
    }
}

static LRESULT CALLBACK ExtMapDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(NULL);
        int W = S(EXTDLG_W), H = S(EXTDLG_H);
        HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
            S(8), S(8), W-S(16), H-S(98), hwnd, (HMENU)IDC_EXTDLG_LIST, hInst, NULL);
        LVCOLUMNW col = {0};
        col.mask = LVCF_TEXT|LVCF_WIDTH;
        col.cx   = S(120); col.pszText = L"扩展名";
        ListView_InsertColumn(hList, 0, &col);
        col.cx   = W-S(16)-S(120)-S(4); col.pszText = L"进程名";
        ListView_InsertColumn(hList, 1, &col);
        extdlg_refresh_list(hList);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT);
        /* 标签行 */
        CreateWindowW(L"STATIC", L"扩展名（如 .xlsx）",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            S(8), H-S(84), S(110), S(18), hwnd, NULL, hInst, NULL);
        CreateWindowW(L"STATIC", L"进程名（如 EXCEL.EXE）",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            S(124), H-S(84), S(140), S(18), hwnd, NULL, hInst, NULL);
        /* 输入行 */
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            S(8), H-S(66), S(112), S(22), hwnd, (HMENU)IDC_EXTDLG_EXT, hInst, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            S(124), H-S(66), W-S(124)-S(86)-S(8), S(22), hwnd, (HMENU)IDC_EXTDLG_PROC, hInst, NULL);
        CreateWindowW(L"BUTTON", L"添加",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            W-S(82), H-S(66), S(74), S(22), hwnd, (HMENU)IDC_EXTDLG_ADD, hInst, NULL);
        /* 底部按钮行 */
        CreateWindowW(L"BUTTON", L"删除选中",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            S(8), H-S(36), S(90), S(26), hwnd, (HMENU)IDC_EXTDLG_DEL, hInst, NULL);
        CreateWindowW(L"BUTTON", L"确定",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            W-S(90), H-S(36), S(82), S(26), hwnd, (HMENU)IDC_EXTDLG_OK, hInst, NULL);
        apply_font_to_children(hwnd, make_gui_font(), &g_extdlg_font);
        /* 对 ListView 子控件也设字体 */
        SendMessageW(hList, WM_SETFONT, (WPARAM)g_extdlg_font, FALSE);
        extdlg_reposition_children(hwnd);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_EXTDLG_ADD) {
            wchar_t ext[32]={0}, proc[MAX_PATH_LEN]={0};
            GetWindowTextW(GetDlgItem(hwnd, IDC_EXTDLG_EXT),  ext,  32);
            GetWindowTextW(GetDlgItem(hwnd, IDC_EXTDLG_PROC), proc, MAX_PATH_LEN);
            if (!ext[0] || !proc[0]) break;
            wchar_t ext_norm[32]={0};
            if (ext[0] != L'.') { ext_norm[0]=L'.'; wcsncpy(ext_norm+1, ext, 30); }
            else wcsncpy(ext_norm, ext, 31);
            wcs_lower(ext_norm);
            BOOL updated = FALSE;
            for (int i=0; i<g_ext_map_cnt; i++) {
                if (wcscmp(g_ext_map[i].ext, ext_norm)==0) {
                    wcsncpy(g_ext_map[i].proc, proc, MAX_PATH_LEN-1);
                    g_ext_map[i].proc[MAX_PATH_LEN-1] = 0;
                    updated = TRUE; break;
                }
            }
            if (!updated && g_ext_map_cnt < MAX_EXT_MAP) {
                wcsncpy(g_ext_map[g_ext_map_cnt].ext,  ext_norm, 31);
                g_ext_map[g_ext_map_cnt].ext[31] = 0;
                wcsncpy(g_ext_map[g_ext_map_cnt].proc, proc, MAX_PATH_LEN-1);
                g_ext_map[g_ext_map_cnt].proc[MAX_PATH_LEN-1] = 0;
                g_ext_map_cnt++;
            }
            save_ini();
            extdlg_refresh_list(GetDlgItem(hwnd, IDC_EXTDLG_LIST));
            SetWindowTextW(GetDlgItem(hwnd, IDC_EXTDLG_EXT),  L"");
            SetWindowTextW(GetDlgItem(hwnd, IDC_EXTDLG_PROC), L"");
        } else if (id == IDC_EXTDLG_DEL) {
            HWND hList = GetDlgItem(hwnd, IDC_EXTDLG_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= g_ext_map_cnt) break;
            for (int i=sel; i<g_ext_map_cnt-1; i++)
                g_ext_map[i] = g_ext_map[i+1];
            g_ext_map_cnt--;
            save_ini();
            extdlg_refresh_list(hList);
        } else if (id == IDC_EXTDLG_OK || id == IDOK) {
            DestroyWindow(hwnd);
        }
        break;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        apply_font_to_children(hwnd, make_gui_font(), &g_extdlg_font);
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        extdlg_reposition_children(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
        } else if (wp == VK_RETURN) {
            HWND hFocus = GetFocus();
            if (hFocus == GetDlgItem(hwnd, IDC_EXTDLG_OK)) {
                DestroyWindow(hwnd);
            } else {
                SendMessageW(hwnd, WM_COMMAND, IDC_EXTDLG_ADD, 0);
            }
        }
        return 0;
    case WM_DESTROY:
        if (g_extdlg_font) { DeleteObject(g_extdlg_font); g_extdlg_font = NULL; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_ext_map_dialog(HWND parent) {
    HINSTANCE hInst = GetModuleHandleW(NULL);
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpszClassName = L"YST_ExtMapDlg";
        wc.lpfnWndProc   = ExtMapDlgProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
        RegisterClassExW(&wc);
        registered = TRUE;
    }
    RECT pr; GetWindowRect(parent, &pr);
    RECT dr = {0,0,S(EXTDLG_W),S(EXTDLG_H)};
    AdjustWindowRect(&dr, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE);
    int dw = dr.right-dr.left, dh = dr.bottom-dr.top;
    int dx = pr.left + (pr.right-pr.left-dw)/2;
    int dy = pr.top  + (pr.bottom-pr.top-dh)/2;
    HWND hdlg = CreateWindowExW(0, L"YST_ExtMapDlg", L"扩展名-进程名 映射管理",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        dx, dy, dw, dh, parent, NULL, hInst, NULL);
    ShowWindow(hdlg, SW_SHOW);
    UpdateWindow(hdlg);
    EnableWindow(parent, FALSE);
    MSG msg;
    while (IsWindow(hdlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hdlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
