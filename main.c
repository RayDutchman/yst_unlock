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
#include <stdlib.h>

#include "decrypt.h"
#include "gui.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    /* DPI 感知 */
    SetProcessDPIAware();

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
        for (int i = 0; i < n; i++)
            g_paths[i] = argv[2 + i];
        g_path_cnt = n;
        run_progress_window(g_paths, n);
        LocalFree(argv);
        return 0;
    }

    /* GUI 模式 */
    int n = argc - 1;
    run_main_gui(n > 0 ? &argv[1] : NULL, n);

    LocalFree(argv);
    return 0;
}
