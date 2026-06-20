/*
 * 亿赛通文件解密工具 - C 语言版本
 *
 * 编译（三文件）：
 *   x86_64-w64-mingw32-gcc -Os -s -DUNICODE -D_UNICODE -mwindows -Wall \
 *     -Wno-unused-parameter -std=c11 \
 *     -o yst_unlock.exe main.c decrypt.c gui.c app.res \
 *     -lshlwapi -lshell32 -lcomctl32 -lcomdlg32 -lole32
 *
 * 三种运行模式：
 *   1. 无参数      -> GUI 主窗口
 *   2. --silent    -> 静默模式（右键菜单，弹进度窗口）
 *   3. --worker    -> worker 子进程（从 stdin 读任务，stdout 回写结果）
 *
 * 文件组织：
 *   main.c     本文件，入口层（WinMain、初始化）
 *   decrypt.h/c  业务层（IPC、解密线程、注册表、INI、文件收集）
 *   gui.h/c      GUI 层（主窗口、进度窗口、扩展名对话框）
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "decrypt.h"
#include "gui.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    /* INI 路径初始化 + 加载配置 */
    init_ini_path();
    load_ini();

    /* Visual Styles：必须在第一个窗口创建前调用 */
    INITCOMMONCONTROLSEX icc = {sizeof(icc),
        ICC_WIN95_CLASSES|ICC_STANDARD_CLASSES|
        ICC_PROGRESS_CLASS|ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* --worker 模式 */
    if (argc >= 2 && wcscmp(argv[1], L"--worker") == 0) {
        worker_mode();
        LocalFree(argv);
        return 0;
    }

    g_paths = (wchar_t**)calloc(MAX_PATHS, sizeof(wchar_t*));
    if (!g_paths) { LocalFree(argv); return 1; }

    /* --silent 模式 */
    if (argc >= 2 && wcscmp(argv[1], L"--silent") == 0) {
        int n = argc - 2;

        /* 单实例合并：右键多文件时共用同一个进度窗口 */
        HANDLE hMutex = CreateMutexW(NULL, FALSE, L"YST_Unlock_SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            /* 后续实例：写入路径到临时目录后退出 */
            wchar_t batch[MAX_PATH_LEN];
            GetTempPathW(MAX_PATH_LEN, batch);
            wcscat(batch, L"yst_unlock_batch\\");
            CreateDirectoryW(batch, NULL);
            wchar_t tmp[MAX_PATH_LEN * 2];
            for (int i = 0; i < n; i++) {
                _snwprintf(tmp, MAX_PATH_LEN * 2 - 1, L"%s%llu_%u",
                           batch, GetTickCount64(), i);
                HANDLE hf = CreateFileW(tmp, GENERIC_WRITE, 0, NULL,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE) {
                    DWORD cb = (DWORD)(wcslen(argv[2 + i]) * sizeof(wchar_t));
                    WriteFile(hf, argv[2 + i], cb, &cb, NULL);
                    CloseHandle(hf);
                }
            }
            CloseHandle(hMutex);
            LocalFree(argv);
            return 0;
        }

        /* 首个实例：等待后续实例写入路径 */
        Sleep(200);

        for (int i = 0; i < n; i++)
            g_paths[i] = argv[2 + i];
        int total = n;

        /* 读取后续实例写入的路径 */
        wchar_t batch[MAX_PATH_LEN];
        GetTempPathW(MAX_PATH_LEN, batch);
        wcscat(batch, L"yst_unlock_batch\\");
        WIN32_FIND_DATAW fd;
        wchar_t pattern[MAX_PATH_LEN];
        _snwprintf(pattern, MAX_PATH_LEN - 1, L"%s*", batch);
        HANDLE hf = FindFirstFileW(pattern, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                if (total >= MAX_PATHS) break;
                wchar_t full[MAX_PATH_LEN];
                _snwprintf(full, MAX_PATH_LEN - 1, L"%s%s", batch, fd.cFileName);
                HANDLE hr = CreateFileW(full, GENERIC_READ, 0, NULL,
                                         OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
                if (hr != INVALID_HANDLE_VALUE) {
                    wchar_t path[MAX_PATH_LEN];
                    DWORD rd;
                    if (ReadFile(hr, path, sizeof(path) - 2, &rd, NULL) && rd >= 2) {
                        path[rd / sizeof(wchar_t)] = 0;
                        g_paths[total++] = _wcsdup(path);
                    }
                    CloseHandle(hr);
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        RemoveDirectoryW(batch);
        CloseHandle(hMutex);

        g_path_cnt = total;
        run_progress_window(g_paths, total);
        LocalFree(argv);
        return 0;
    }

    /* GUI 模式 */
    int n = argc - 1;
    run_main_gui(n > 0 ? &argv[1] : NULL, n);

    LocalFree(argv);
    return 0;
}
