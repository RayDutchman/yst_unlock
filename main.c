/*
 * 亿赛通文件解密工具 - C 语言版本
 * 编译：MinGW gcc -Os -s -mwindows -o yst_unlock.exe main.c -lshlwapi -lshell32
 *
 * 三种运行模式：
 *   1. 无参数      -> GUI 主窗口
 *   2. --silent    -> 静默模式（右键菜单，弹进度窗口）
 *   3. --worker    -> worker 子进程（从 stdin 读任务，stdout 回写结果）
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include <commdlg.h>
#include <shlobj.h>

/* ── 常量定义 ─────────────────────────────────────────────── */
#define APP_TITLE       L"YST unlock"
#define MENU_LABEL      L"YST unlock"
#define MAX_PATHS       4096
#define MAX_PATH_LEN    4096
#define WM_WORKER_LOG   (WM_USER + 1)
#define WM_WORKER_PROG  (WM_USER + 2)
#define WM_WORKER_DONE  (WM_USER + 3)

/* 控件 ID */
#define IDC_LISTBOX     101
#define IDC_BTN_ADDFILE 102
#define IDC_BTN_ADDDIR  103
#define IDC_BTN_CLEAR   104
#define IDC_BTN_DECRYPT 105
#define IDC_BTN_INSTALL 106
#define IDC_BTN_UNINSTALL 107
#define IDC_PROGRESS    108
#define IDC_LOG         109
#define IDC_CHECK_AUTO  110
#define IDC_EDIT_PROC   111
#define IDC_EDIT_OUTDIR 112
#define IDC_BTN_BROWSE  113

/* 跳过的目录名（小写，逗号分隔存储在数组中） */
static const wchar_t *SKIP_DIRS[] = {
    L".git", L".svn", L".hg", L".idea", L".vscode",
    L"__pycache__", L"node_modules", L".cache",
    L"$recycle.bin", L"system volume information",
    L"windows", L"program files", L"program files (x86)",
    NULL
};

/* 跳过的文件扩展名（小写） */
static const wchar_t *SKIP_EXTS[] = {
    L".lnk", L".tmp", L".temp", L".yst_tmp",
    L".db", L".ds_store", L".ini", L".log",
    NULL
};

/* 注册表根路径 */
static const wchar_t *REG_ROOTS[] = {
    L"Software\\Classes\\*\\shell",
    L"Software\\Classes\\Directory\\shell",
    L"Software\\Classes\\Directory\\Background\\shell",
    NULL
};

/* ── 全局状态 ─────────────────────────────────────────────── */
static HWND  g_hwnd        = NULL;  /* 主窗口句柄 */
static HWND  g_hwndList    = NULL;
static HWND  g_hwndLog     = NULL;
static HWND  g_hwndProgress= NULL;
static HWND  g_hwndProcEdit= NULL;
static HWND  g_hwndOutDir  = NULL;
static HWND  g_hwndCheckAuto = NULL;
static HWND  g_hwndBtnDecrypt = NULL;

/* 路径列表 */
static wchar_t **g_paths   = NULL;
static int       g_path_cnt= 0;

/* ── 工具函数 ─────────────────────────────────────────────── */

/* 获取当前 exe 的完整路径 */
static void get_self_path(wchar_t *buf, int len) {
    GetModuleFileNameW(NULL, buf, len);
}

/* 字符串转小写（in-place） */
static void wcs_lower(wchar_t *s) {
    for (; *s; s++) *s = (wchar_t)towlower(*s);
}

/* ── 注册表：查询扩展名默认进程名 ──────────────────────────── */

static BOOL reg_query_str(HKEY root, const wchar_t *path,
                          const wchar_t *val, wchar_t *out, DWORD outlen) {
    HKEY hk;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return FALSE;
    DWORD type, cb = outlen * sizeof(wchar_t);
    BOOL ok = (RegQueryValueExW(hk, val, NULL, &type,
                                (BYTE*)out, &cb) == ERROR_SUCCESS
               && (type == REG_SZ || type == REG_EXPAND_SZ));
    RegCloseKey(hk);
    return ok;
}

/* 从命令行字符串提取 exe 文件名，如 "C:\foo\WINWORD.EXE" /n "%1" -> WINWORD.EXE */
static void extract_exe_name(const wchar_t *cmd, wchar_t *out, int outlen) {
    wchar_t tmp[MAX_PATH * 2];  /* 路径含参数，留足空间 */
    wcsncpy(tmp, cmd, MAX_PATH * 2 - 1);
    tmp[MAX_PATH * 2 - 1] = 0;
    wchar_t *p = tmp;
    wchar_t *end;
    if (*p == L'"') {
        /* 有引号：找闭合引号作为路径终止，正确处理含空格的路径 */
        p++;
        end = wcschr(p, L'"');
        if (end) *end = 0;
    } else {
        /* 无引号：找第一个空格 */
        end = wcschr(p, L' ');
        if (end) *end = 0;
    }
    /* 取文件名部分 */
    wchar_t *base = wcsrchr(p, L'\\');
    if (!base) base = wcsrchr(p, L'/');
    const wchar_t *name = base ? base + 1 : p;
    wcsncpy(out, name, outlen - 1);
    out[outlen - 1] = 0;
}

/* 根据扩展名查注册表，返回默认打开程序进程名（如 WINWORD.EXE） */
static BOOL get_default_process_for_ext(const wchar_t *ext, wchar_t *out, int outlen) {
    /* 全部用堆，避免在工作线程中耗尽栈 */
    wchar_t *prog_id = (wchar_t*)calloc(512, sizeof(wchar_t));
    wchar_t *uc_path = (wchar_t*)calloc(512, sizeof(wchar_t));
    wchar_t *cmd_path= (wchar_t*)calloc(1024, sizeof(wchar_t));
    wchar_t *cmd     = (wchar_t*)calloc(MAX_PATH, sizeof(wchar_t));
    if (!prog_id || !uc_path || !cmd_path || !cmd) {
        free(prog_id); free(uc_path); free(cmd_path); free(cmd);
        return FALSE;
    }
    BOOL result = FALSE;

    /* 用户级关联 UserChoice */
    _snwprintf(uc_path, 511,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
        ext);
    if (!reg_query_str(HKEY_CURRENT_USER, uc_path, L"ProgId", prog_id, 512)) {
        /* 回退 HKCR */
        if (!reg_query_str(HKEY_CLASSES_ROOT, ext, L"", prog_id, 512))
            goto done;
    }
    if (!prog_id[0]) goto done;

    /* 从 ProgId shell\open\command 提取 exe */
    {
        static const wchar_t *verbs[] = {L"open", L"Open", L"edit", NULL};
        for (int i = 0; verbs[i]; i++) {
            _snwprintf(cmd_path, 1023, L"%s\\shell\\%s\\command", prog_id, verbs[i]);
            if (!reg_query_str(HKEY_CLASSES_ROOT, cmd_path, L"", cmd, MAX_PATH))
                continue;
            extract_exe_name(cmd, out, outlen);
            /* 检查是否是 .exe */
            wchar_t lower[64];
            wcsncpy(lower, out, 63); lower[63] = 0;
            wcs_lower(lower);
            if (wcsstr(lower, L".exe")) { result = TRUE; goto done; }
        }
    }
done:
    free(prog_id); free(uc_path); free(cmd_path); free(cmd);
    return result;
}

/* 返回伪装进程名：override > 注册表
 * 返回 FALSE 表示注册表未找到，使用了兜底（此时 out 为空），调用方应记为失败 */
static BOOL resolve_process_name(const wchar_t *filepath,
                                 const wchar_t *override_name,
                                 wchar_t *out, int outlen) {
    if (override_name && override_name[0]) {
        wcsncpy(out, override_name, outlen - 1);
        out[outlen - 1] = 0;
        return TRUE;
    }
    /* 取扩展名 */
    const wchar_t *dot = wcsrchr(filepath, L'.');
    if (dot && get_default_process_for_ext(dot, out, outlen))
        return TRUE;
    /* 注册表未找到，返回 FALSE，调用方记为失败 */
    out[0] = 0;
    return FALSE;
}

/* ── 右键菜单注册表管理 ──────────────────────────────── */

static void install_context_menu(wchar_t *msg_out, int msg_len) {
    wchar_t self_exe[MAX_PATH_LEN];
    get_self_path(self_exe, MAX_PATH_LEN);
    int errors = 0;
    for (int i = 0; REG_ROOTS[i]; i++) {
        wchar_t shell_path[1024], cmd_path[1024], cmd[MAX_PATH_LEN];
        _snwprintf(shell_path, 1023, L"%s\\%s", REG_ROOTS[i], MENU_LABEL);
        _snwprintf(cmd_path,   1023, L"%s\\command", shell_path);
        /* Background\shell 用 %V，其余用 %1 */
        const wchar_t *ph = wcsstr(REG_ROOTS[i], L"Background") ? L"%V" : L"%1";
        _snwprintf(cmd, MAX_PATH_LEN - 1,
                   L"\"%s\" --silent \"%s\"", self_exe, ph);
        HKEY hk;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, shell_path, 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL)
                != ERROR_SUCCESS) { errors++; continue; }
        RegSetValueExW(hk, L"",     0, REG_SZ,
                       (BYTE*)MENU_LABEL, (DWORD)((wcslen(MENU_LABEL)+1)*2));
        RegSetValueExW(hk, L"Icon", 0, REG_SZ,
                       (BYTE*)self_exe,   (DWORD)((wcslen(self_exe)+1)*2));
        RegCloseKey(hk);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, cmd_path, 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL)
                != ERROR_SUCCESS) { errors++; continue; }
        RegSetValueExW(hk, L"", 0, REG_SZ,
                       (BYTE*)cmd, (DWORD)((wcslen(cmd)+1)*2));
        RegCloseKey(hk);
    }
    wcsncpy(msg_out, errors ? L"部分失败" : L"右键菜单安装成功", msg_len - 1);
}

static void uninstall_context_menu(wchar_t *msg_out, int msg_len) {
    int errors = 0;
    for (int i = 0; REG_ROOTS[i]; i++) {
        wchar_t shell_path[1024], cmd_path[1024];
        _snwprintf(shell_path, 1023, L"%s\\%s",         REG_ROOTS[i], MENU_LABEL);
        _snwprintf(cmd_path,   1023, L"%s\\command",    shell_path);
        /* 先删子键再删父键 */
        RegDeleteKeyW(HKEY_CURRENT_USER, cmd_path);
        LONG r = RegDeleteKeyW(HKEY_CURRENT_USER, shell_path);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) errors++;
    }
    wcsncpy(msg_out, errors ? L"部分失败" : L"右键菜单已卸载", msg_len - 1);
}

/* ── 文件过滤与收集 ─────────────────────────────────────── */

static BOOL should_skip_dir(const wchar_t *name) {
    if (name[0] == L'.' || name[0] == L'_') return TRUE;
    wchar_t lower[MAX_PATH];
    wcsncpy(lower, name, MAX_PATH - 1);
    wcs_lower(lower);
    for (int i = 0; SKIP_DIRS[i]; i++)
        if (wcscmp(lower, SKIP_DIRS[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL should_skip_file(const wchar_t *path) {
    /* 取文件名部分 */
    const wchar_t *base = wcsrchr(path, L'\\');
    if (!base) base = path; else base++;
    if (base[0] == L'.' ) return TRUE;
    /* 跳过 ~$ 开头的临时文件 */
    if (base[0] == L'~' && base[1] == L'$') return TRUE;
    /* 扩展名匹配 */
    const wchar_t *dot = wcsrchr(base, L'.');
    if (!dot) return FALSE;
    wchar_t lower[64];
    wcsncpy(lower, dot, 63); lower[63] = 0;
    wcs_lower(lower);
    for (int i = 0; SKIP_EXTS[i]; i++)
        if (wcscmp(lower, SKIP_EXTS[i]) == 0) return TRUE;
    return FALSE;
}

/* 动态数组：收集到的文件列表 */
typedef struct {
    wchar_t **items;
    int       count;
    int       capacity;
} FileList;

static void filelist_init(FileList *fl) {
    fl->capacity = 256;
    fl->items = (wchar_t**)malloc(fl->capacity * sizeof(wchar_t*));
    fl->count = 0;
}

static void filelist_push(FileList *fl, const wchar_t *path) {
    if (fl->count >= fl->capacity) {
        fl->capacity *= 2;
        fl->items = (wchar_t**)realloc(fl->items, fl->capacity * sizeof(wchar_t*));
    }
    fl->items[fl->count++] = _wcsdup(path);
}

static void filelist_free(FileList *fl) {
    for (int i = 0; i < fl->count; i++) free(fl->items[i]);
    free(fl->items);
    fl->items = NULL; fl->count = fl->capacity = 0;
}

/* 递归遍历目录 */
static void walk_dir(const wchar_t *dir, FileList *fl) {
    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN - 1, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pattern, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[MAX_PATH_LEN];
        _snwprintf(full, MAX_PATH_LEN - 1, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!should_skip_dir(fd.cFileName))
                walk_dir(full, fl);
        } else {
            if (!should_skip_file(full))
                filelist_push(fl, full);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

/* 收集 paths[] 中所有有效文件 */
static void collect_files(wchar_t **paths, int n, FileList *fl) {
    for (int i = 0; i < n; i++) {
        DWORD attr = GetFileAttributesW(paths[i]);
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            walk_dir(paths[i], fl);
        else if (!should_skip_file(paths[i]))
            filelist_push(fl, paths[i]);
    }
}

/* ── Worker 子进程模式 ───────────────────────────────────────── */

/*
 * --worker 模式：从 stdin 循环读取任务行 "src\tdst\n"
 * 每条回写 "OK\n" 或 "ERR:msg\n" 到 stdout，EOF 时退出。
 * 以当前进程名（伪装白名单进程）运行。
 */
static void worker_mode(void) {
    /* 切换为二进制模式，避免翻译问题 */
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    char line[MAX_PATH_LEN * 4];
    while (fgets(line, sizeof(line), stdin)) {
        /* 去掉行尾 \r\n */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = 0;
        if (!len) continue;
        /* 找 tab 分隔符 */
        char *tab = strchr(line, '\t');
        if (!tab) { fputs("ERR:no tab\n", stdout); fflush(stdout); continue; }
        *tab = 0;
        const char *src_u8 = line;
        const char *dst_u8 = tab + 1;
        /* UTF-8 -> wchar */
        wchar_t src_w[MAX_PATH_LEN], dst_w[MAX_PATH_LEN];
        MultiByteToWideChar(CP_UTF8, 0, src_u8, -1, src_w, MAX_PATH_LEN);
        MultiByteToWideChar(CP_UTF8, 0, dst_u8, -1, dst_w, MAX_PATH_LEN);
        /* 复制文件（元数据无损复制） */
        if (CopyFileW(src_w, dst_w, FALSE)) {
            fputs("OK\n", stdout);
        } else {
            DWORD e = GetLastError();
            fprintf(stdout, "ERR:CopyFile failed (%lu)\n", e);
        }
        fflush(stdout);
    }
}


/* 递归删除目录（等价于 rmdir /s /q），在调用线程中同步执行 */
static void rmdir_recursive(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN-1, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pattern, &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            wchar_t full[MAX_PATH_LEN];
            _snwprintf(full, MAX_PATH_LEN-1, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rmdir_recursive(full);
            else {
                SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full);
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    RemoveDirectoryW(dir);
}

/* ── 核心解密逻辑（运行在工作线程中） ─────────────────────────── */

/* 启动 worker 子进程，返回句柄/管道 */
typedef struct {
    HANDLE hProcess;
    HANDLE hStdin_W;   /* 写端（主进程向 worker 写） */
    HANDLE hStdout_R;  /* 读端（主进程读 worker 输出） */
} WorkerProc;

static BOOL start_worker(const wchar_t *fake_exe, WorkerProc *wp) {
    HANDLE r_in=NULL, w_in=NULL, r_out=NULL, w_out=NULL;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE}; /* 子进程可继承 */
    if (!CreatePipe(&r_in,  &w_in,  &sa, 0)) return FALSE;
    if (!CreatePipe(&r_out, &w_out, &sa, 0)) { CloseHandle(r_in); CloseHandle(w_in); return FALSE; }
    /* 主进程一侧不继承 */
    SetHandleInformation(w_in,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(r_out, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = r_in;
    si.hStdOutput  = w_out;
    /* 用 NUL 设备作 stderr，避免子进程写 stderr 阻塞或崩溃 */
    SECURITY_ATTRIBUTES sa_null = {sizeof(sa_null), NULL, TRUE};
    HANDLE hNull = CreateFileW(L"nul", GENERIC_WRITE, FILE_SHARE_WRITE,
                               &sa_null, OPEN_EXISTING, 0, NULL);
    si.hStdError = hNull;
    wchar_t cmd[MAX_PATH_LEN];
    _snwprintf(cmd, MAX_PATH_LEN-1, L"\"%s\" --worker", fake_exe);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(r_in); CloseHandle(w_out);
    if (hNull != INVALID_HANDLE_VALUE) CloseHandle(hNull);
    if (!ok) { CloseHandle(w_in); CloseHandle(r_out); return FALSE; }
    CloseHandle(pi.hThread);
    wp->hProcess  = pi.hProcess;
    wp->hStdin_W  = w_in;
    wp->hStdout_R = r_out;
    return TRUE;
}

static void stop_worker(WorkerProc *wp) {
    if (wp->hStdin_W)  { CloseHandle(wp->hStdin_W);  wp->hStdin_W  = NULL; }
    if (wp->hProcess)  {
        WaitForSingleObject(wp->hProcess, 3000);
        TerminateProcess(wp->hProcess, 0);
        CloseHandle(wp->hProcess); wp->hProcess = NULL;
    }
    if (wp->hStdout_R) { CloseHandle(wp->hStdout_R); wp->hStdout_R = NULL; }
}

/* 向 worker 发送任务，返回 TRUE=成功 */
static BOOL send_task(WorkerProc *wp,
                      const wchar_t *src, const wchar_t *dst,
                      char *err_out, int err_len) {
    /* 拼 UTF-8 行：堆分配避免大栈 */
    int su = MAX_PATH_LEN*3, du = MAX_PATH_LEN*3, lu = MAX_PATH_LEN*6;
    char *src_u8 = (char*)malloc(su);
    char *dst_u8 = (char*)malloc(du);
    char *line   = (char*)malloc(lu);
    if (!src_u8 || !dst_u8 || !line) {
        free(src_u8); free(dst_u8); free(line);
        strncpy(err_out, "out of memory", err_len-1); return FALSE;
    }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, src_u8, su, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, dst, -1, dst_u8, du, NULL, NULL);
    int n = snprintf(line, lu, "%s\t%s\n", src_u8, dst_u8);
    DWORD written;
    if (!WriteFile(wp->hStdin_W, line, (DWORD)n, &written, NULL)) {
        free(src_u8); free(dst_u8); free(line); return FALSE;
    }
    free(src_u8); free(dst_u8); free(line);
    /* 读响应行 */
    char resp[512] = {0};
    int ri = 0;
    char c;
    DWORD rd;
    while (ri < (int)sizeof(resp)-1) {
        if (!ReadFile(wp->hStdout_R, &c, 1, &rd, NULL) || rd == 0) break;
        if (c == '\n') break;
        resp[ri++] = c;
    }
    resp[ri] = 0;
    if (strcmp(resp, "OK") == 0) return TRUE;
    if (strncmp(resp, "ERR:", 4) == 0)
        strncpy(err_out, resp+4, err_len-1);
    else
        strncpy(err_out, resp, err_len-1);
    return FALSE;
}


/* ── 解密线程参数结构 ─────────────────────────────────────────── */

typedef struct {
    wchar_t **paths;
    int       path_cnt;
    wchar_t   proc_override[MAX_PATH];  /* 空 = 自动 */
    wchar_t   output_dir[MAX_PATH];     /* 空 = 覆盖原文件 */
    HWND      notify_hwnd;  /* 接收 WM_WORKER_LOG / PROG / DONE 的窗口 */
} DecryptArgs;

/* 工作线程输出：日志字符串（内存由接收方 free）、进度小数 */
#define NOTIFY_LOG(hwnd, s)  PostMessageW((hwnd), WM_WORKER_LOG,  0, (LPARAM)_wcsdup(s))
#define NOTIFY_PROG(hwnd, v) PostMessageW((hwnd), WM_WORKER_PROG, (WPARAM)(int)(v), 0)
#define NOTIFY_DONE(hwnd, ok) PostMessageW((hwnd), WM_WORKER_DONE, (WPARAM)(ok), 0)

/* 每种进程名对应一个 WorkerProc（最多 16 种） */
#define MAX_WORKERS 16
typedef struct { wchar_t name[MAX_PATH]; WorkerProc wp; } WorkerEntry;

static DWORD WINAPI decrypt_thread(LPVOID param) {
    DecryptArgs *args = (DecryptArgs*)param;
    HWND hwnd = args->notify_hwnd;

    /* 收集文件 */
    FileList fl;
    filelist_init(&fl);
    collect_files(args->paths, args->path_cnt, &fl);

    if (fl.count == 0) {
        NOTIFY_LOG(hwnd, L"[警告] 未找到任何文件");
        NOTIFY_DONE(hwnd, FALSE);
        goto cleanup_args;
    }

    /* 更新进度条总数 */
    int total = fl.count;
    {
        wchar_t buf[128];
        _snwprintf(buf, 127, L"共找到 %d 个文件，开始解密...", total);
        NOTIFY_LOG(hwnd, buf);
    }

    /* 自身 exe 路径（MAX_PATH 够用） */
    wchar_t self_exe[MAX_PATH];
    get_self_path(self_exe, MAX_PATH);

    /* 创建临时目录 */
    wchar_t tmp_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_dir);
    {
        wchar_t sub[64];
        _snwprintf(sub, 63, L"yst_unlock_%lu\\", GetCurrentProcessId());
        wcscat(tmp_dir, sub);
    }
    CreateDirectoryW(tmp_dir, NULL);

    wchar_t common_root[MAX_PATH] = {0};
    if (args->output_dir[0]) {
        /* 简化：公共根 = 第一个路径的父目录 */
        if (args->path_cnt >= 1) {
            wcsncpy(common_root, args->paths[0], MAX_PATH-1);
            DWORD attr = GetFileAttributesW(common_root);
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t *bs = wcsrchr(common_root, L'\\');
                if (bs) *bs = 0;
            }
        }
        CreateDirectoryW(args->output_dir, NULL);
    }

    WorkerEntry workers[MAX_WORKERS];
    int worker_cnt = 0;
    int success_count = 0, fail_count = 0;

    for (int idx = 0; idx < fl.count; idx++) {
        const wchar_t *src = fl.items[idx];

        /* 确定进程名 */
        wchar_t proc_name[MAX_PATH];
        if (!resolve_process_name(src, args->proc_override, proc_name, MAX_PATH)) {
            const wchar_t *fn = wcsrchr(src, L'\\');
            if (!fn) fn = src; else fn++;
            wchar_t fbuf[512];
            _snwprintf(fbuf, 511, L"[%d/%d] 跳过（未知扩展名，无注册表关联）: %s",
                       idx+1, total, fn);
            NOTIFY_LOG(hwnd, fbuf);
            fail_count++;
            NOTIFY_PROG(hwnd, (idx+1)*100/total);
            continue;
        }

        /* 查找或创建 worker */
        WorkerEntry *we = NULL;
        for (int w = 0; w < worker_cnt; w++)
            if (wcscmp(workers[w].name, proc_name) == 0) { we = &workers[w]; break; }
        if (!we) {
            if (worker_cnt < MAX_WORKERS) {
                we = &workers[worker_cnt++];
                wcsncpy(we->name, proc_name, MAX_PATH-1);
                memset(&we->wp, 0, sizeof(we->wp));
            }
        }
        /* worker 进程未在运行，启动之 */
        if (we && (!we->wp.hProcess ||
                   WaitForSingleObject(we->wp.hProcess, 0) != WAIT_TIMEOUT)) {
            wchar_t fake_exe[MAX_PATH_LEN];
            _snwprintf(fake_exe, MAX_PATH_LEN-1, L"%s%s", tmp_dir, proc_name);
            if (GetFileAttributesW(fake_exe) == INVALID_FILE_ATTRIBUTES)
                CopyFileW(self_exe, fake_exe, FALSE);
            stop_worker(&we->wp);
            if (!start_worker(fake_exe, &we->wp)) {
                wchar_t buf[512];
                _snwprintf(buf, 511, L"[%d/%d] 失败 [%s]: 启动 worker 失败",
                           idx+1, total, proc_name);
                NOTIFY_LOG(hwnd, buf);
                fail_count++;
                NOTIFY_PROG(hwnd, (idx+1)*100/total);
                continue;
            }
        }

        /* 确定输出路径 */
        wchar_t dst[MAX_PATH];
        if (args->output_dir[0] && common_root[0]) {
            /* 计算相对路径 */
            const wchar_t *rel = src;
            int cr_len = (int)wcslen(common_root);
            if (wcsncmp(src, common_root, cr_len) == 0)
                rel = src + cr_len + 1;
            _snwprintf(dst, MAX_PATH-1, L"%s\\%s", args->output_dir, rel);
            /* 创建父目录 */
            wchar_t parent[MAX_PATH_LEN];
            wcsncpy(parent, dst, MAX_PATH_LEN-1);
            wchar_t *bs = wcsrchr(parent, L'\\');
            if (bs) { *bs = 0; CreateDirectoryW(parent, NULL); }
        } else {
            _snwprintf(dst, MAX_PATH-1, L"%s.yst_tmp", src);
        }

        /* 发送任务 */
        char err[512] = {0};
        BOOL ok = we ? send_task(&we->wp, src, dst, err, 512) : FALSE;

        if (ok && !args->output_dir[0]) {
            /* 覆盖原文件 */
            if (!MoveFileExW(dst, src, MOVEFILE_REPLACE_EXISTING)) {
                DWORD e = GetLastError();
                snprintf(err, 512, "替换原文件失败 (%lu)", e);
                ok = FALSE;
                DeleteFileW(dst);
            }
        } else if (!ok && !args->output_dir[0]) {
            DeleteFileW(dst);
        }

        const wchar_t *fname = wcsrchr(src, L'\\');
        if (!fname) fname = src; else fname++;
        wchar_t buf[1024];
        if (ok) {
            success_count++;
            _snwprintf(buf, 1023, L"[%d/%d] 成功 [%s]: %s", idx+1, total, proc_name, fname);
        } else {
            fail_count++;
            wchar_t werr[512];
            MultiByteToWideChar(CP_UTF8, 0, err, -1, werr, 512);
            _snwprintf(buf, 1023, L"[%d/%d] 失败 [%s]: %s — %s",
                       idx+1, total, proc_name, fname, werr);
        }
        NOTIFY_LOG(hwnd, buf);
        NOTIFY_PROG(hwnd, (idx+1)*100/total);
    }

    /* 关闭所有 worker */
    for (int w = 0; w < worker_cnt; w++) stop_worker(&workers[w].wp);
    /* 删除临时目录（同步，避免竞态） */
    rmdir_recursive(tmp_dir);
    {
        wchar_t buf[128];
        _snwprintf(buf, 127, L"\n完成：成功 %d 个，失败 %d 个", success_count, fail_count);
        NOTIFY_LOG(hwnd, buf);
    }
    NOTIFY_DONE(hwnd, success_count > 0);

cleanup_args:
    filelist_free(&fl);
    for (int i = 0; i < args->path_cnt; i++) free(args->paths[i]);
    free(args->paths);
    free(args);
    return 0;
}

/* ── 辅助：启动解密线程 ───────────────────────────────────────────── */

static void start_decrypt_thread(HWND notify_hwnd,
                                  wchar_t **paths, int path_cnt,
                                  const wchar_t *proc_override,
                                  const wchar_t *output_dir) {
    DecryptArgs *args = (DecryptArgs*)calloc(1, sizeof(DecryptArgs));
    args->notify_hwnd = notify_hwnd;
    args->path_cnt    = path_cnt;
    args->paths       = (wchar_t**)malloc(path_cnt * sizeof(wchar_t*));
    for (int i = 0; i < path_cnt; i++)
        args->paths[i] = _wcsdup(paths[i]);
    if (proc_override)
        wcsncpy(args->proc_override, proc_override, MAX_PATH-1);
    if (output_dir)
        wcsncpy(args->output_dir, output_dir, MAX_PATH-1);
    HANDLE ht = CreateThread(NULL, 0, decrypt_thread, args, 0, NULL);
    if (ht) CloseHandle(ht);
}

/* ── 进度窗口（--silent 模式） ─────────────────────────────────────── */

#define PROG_W 480
#define PROG_H 260  /* 客户区高，外框约 295px */

static HWND g_prog_hwnd     = NULL;
static HWND g_prog_bar      = NULL;
static HWND g_prog_log      = NULL;
static HWND g_prog_status   = NULL;
static HWND g_prog_btn      = NULL;
static BOOL g_prog_destroyed= FALSE;
static UINT_PTR g_prog_timer= 0;

static LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(NULL);
        /* 状态标签 */
        g_prog_status = CreateWindowW(L"STATIC", L"准备中...",
            WS_CHILD|WS_VISIBLE|SS_LEFT, 10,8, PROG_W-20,20, hwnd, NULL, hInst, NULL);
        /* 进度条 */
        g_prog_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD|WS_VISIBLE|PBS_SMOOTH, 10,34, PROG_W-20,18, hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);
        SendMessageW(g_prog_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        /* 日志 */
        g_prog_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            10,58, PROG_W-20, PROG_H-58-38, hwnd, (HMENU)IDC_LOG, hInst, NULL);
        SendMessageW(g_prog_log, EM_LIMITTEXT, 0, 0);
        /* 关闭按钮：位于客户区底部，初始禁用 */
        g_prog_btn = CreateWindowW(L"BUTTON", L"关闭",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_DISABLED,
            PROG_W/2-40, PROG_H-34, 80, 26, hwnd, (HMENU)IDC_BTN_DECRYPT, hInst, NULL);
        /* 应用系统 GUI 字体 */
        {
            HFONT hF = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HWND hc = GetWindow(hwnd, GW_CHILD);
            while (hc) { SendMessageW(hc, WM_SETFONT, (WPARAM)hF, FALSE); hc = GetWindow(hc, GW_HWNDNEXT); }
        }
        /* 线程 100ms 后启动 */
        SetTimer(hwnd, 1, 100, NULL);
        break;
    }
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            start_decrypt_thread(hwnd, g_paths, g_path_cnt, NULL, NULL);
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            g_prog_timer = 0;
            DestroyWindow(hwnd);
        }
        break;
    case WM_WORKER_LOG: {
        wchar_t *s = (wchar_t*)lp;
        if (s) {
            /* 向日志添加行 */
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
            SetWindowTextW(g_prog_status, buf);
        }
        return 0;
    case WM_WORKER_DONE:
        SetWindowTextW(g_prog_status, wp ? L"解密完成" : L"解密完成（有失败项）");
        SendMessageW(g_prog_bar, PBM_SETPOS, 100, 0);
        EnableWindow(g_prog_btn, TRUE);
        /* 不自动关闭，等用户手动点关闭 */
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_DECRYPT) {
            if (g_prog_timer) KillTimer(hwnd, g_prog_timer);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        g_prog_destroyed = TRUE;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void run_progress_window(wchar_t **paths, int n) {
    /* 存入全局，窗口过程中使用 */
    g_paths    = paths;
    g_path_cnt = n;

    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpszClassName = L"YST_ProgressWnd";
    wc.lpfnWndProc   = ProgressWndProc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    /* 居中显示 */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    /* 从客户区尺寸推算外框，避免控件超出 */
    {
        RECT pr = {0, 0, PROG_W, PROG_H};
        AdjustWindowRect(&pr, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE);
        int pw = pr.right - pr.left;
        int ph = pr.bottom - pr.top;
        g_prog_hwnd = CreateWindowExW(0, L"YST_ProgressWnd", L"亿赛通解密",
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

/* ── GUI 主窗口 ─────────────────────────────────────────────────── */

/* 最小客户区尺寸 */
#define GUI_MIN_CW 480
#define GUI_MIN_CH 420
/* 兼容旧引用（WM_CREATE 初始布局用） */
#define GUI_CLIENT_W 540
#define GUI_CLIENT_H 518
#define GUI_MIN_W GUI_CLIENT_W
#define GUI_MIN_H GUI_CLIENT_H

static HWND    g_hwndListBox    = NULL; /* 主 GUI 的 ListBox 句柄 */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    /* 所有需要在 WM_SIZE 里移动的控件句柄 */
    static HWND hLog=NULL, hProg=NULL, hProcEdit=NULL, hOutDir=NULL;
    static HWND hCheckAuto=NULL, hBtnDecrypt=NULL;
    /* GroupBox 和文件区按钮（WM_SIZE 需要跟随宽度） */
    static HWND hGbFiles=NULL, hGbCfg=NULL, hGbLog=NULL;
    static HWND hBtnAddFile=NULL, hBtnAddDir=NULL;
    static HWND hStaticProc=NULL, hStaticHint=NULL;
    static HWND hStaticOutLabel=NULL;
    static BOOL is_decrypting = FALSE;

    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(NULL);
        /* 布局常量：客户区宽 GUI_CLIENT_W=540 */
        const int P  = 8;    /* 外边距 */
        const int GW = GUI_CLIENT_W - 2*P;          /* GroupBox 宽 = 524 */
        const int IW = GW - 16;                     /* GroupBox 内控件宽 = 508 */
        const int BW = 80;   /* 普通按钮宽 */
        const int BH = 24;   /* 普通按钮高 */
        const int EH = 22;   /* Edit 高 */
        int y = 4;

        /* ── 文件列表 GroupBox (h=196) ── */
        hGbFiles = CreateWindowW(L"BUTTON", L"待解密文件 / 文件夹（可拖入）",
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
            P, y, GW, 196, hwnd, NULL, hInst, NULL);
        /* 按钮行 y+18 */
        hBtnAddFile = CreateWindowW(L"BUTTON", L"添加文件",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P+8, y+18, BW, BH, hwnd, (HMENU)IDC_BTN_ADDFILE, hInst, NULL);
        hBtnAddDir = CreateWindowW(L"BUTTON", L"添加文件夹",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P+8+BW+8, y+18, BW, BH, hwnd, (HMENU)IDC_BTN_ADDDIR, hInst, NULL);
        CreateWindowW(L"BUTTON", L"清空列表",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P+GW-8-BW, y+18, BW, BH, hwnd, (HMENU)IDC_BTN_CLEAR, hInst, NULL);
        /* ListBox y+48, h=136 */
        g_hwndListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
            P+8, y+48, IW, 136, hwnd, (HMENU)IDC_LISTBOX, hInst, NULL);

        /* ── 配置 GroupBox (y+200, h=92) ── */
        y += 200;
        hGbCfg = CreateWindowW(L"BUTTON", L"解密配置",
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
            P, y, GW, 92, hwnd, NULL, hInst, NULL);
        /* 复选框 */
        hCheckAuto = CreateWindowW(L"BUTTON", L"推荐设置（自动匹配进程名）",
            WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            P+8, y+18, 280, 20, hwnd, (HMENU)IDC_CHECK_AUTO, hInst, NULL);
        SendMessageW(hCheckAuto, BM_SETCHECK, BST_CHECKED, 0);
        /* 进程名行 */
        hStaticProc = CreateWindowW(L"STATIC", L"指定进程名:",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            P+8, y+44, 76, 20, hwnd, NULL, hInst, NULL);
        hProcEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|WS_DISABLED|ES_AUTOHSCROLL,
            P+8+76+4, y+42, 148, EH, hwnd, (HMENU)IDC_EDIT_PROC, hInst, NULL);
        hStaticHint = CreateWindowW(L"STATIC", L"(如 EXCEL.EXE)",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            P+8+76+4+148+6, y+44, 120, 20, hwnd, NULL, hInst, NULL);
        /* 输出目录行：Edit 弹性宽 = IW - label - btn - gaps */
        {
            const int lw = 68, bw2 = 56, gap = 4;
            const int ew = IW - lw - gap - bw2 - gap;  /* 508-68-4-56-4 = 376 */
            hStaticOutLabel = CreateWindowW(L"STATIC", L"输出目录:",
                WS_CHILD|WS_VISIBLE|SS_LEFT,
                P+8, y+68, lw, 20, hwnd, NULL, hInst, NULL);
            hOutDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                P+8+lw+gap, y+66, ew, EH, hwnd, (HMENU)IDC_EDIT_OUTDIR, hInst, NULL);
            CreateWindowW(L"BUTTON", L"浏览...",
                WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                P+8+lw+gap+ew+gap, y+66, bw2, EH, hwnd, (HMENU)IDC_BTN_BROWSE, hInst, NULL);
        }

        /* ── 日志 GroupBox (y+96, h=164) ── */
        y += 96;
        hGbLog = CreateWindowW(L"BUTTON", L"日志",
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
            P, y, GW, 164, hwnd, NULL, hInst, NULL);
        hProg = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            P+8, y+18, IW, 16, hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);
        SendMessageW(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|
            ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            P+8, y+40, IW, 112, hwnd, (HMENU)IDC_LOG, hInst, NULL);
        SendMessageW(hLog, EM_LIMITTEXT, 0, 0); /* 0 = 最大限制（约2GB） */

        /* ── 底部按钮行 (y+168) ── */
        y += 168;
        CreateWindowW(L"BUTTON", L"安装右键菜单",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P, y, 96, BH, hwnd, (HMENU)IDC_BTN_INSTALL, hInst, NULL);
        CreateWindowW(L"BUTTON", L"卸载右键菜单",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P+96+6, y, 96, BH, hwnd, (HMENU)IDC_BTN_UNINSTALL, hInst, NULL);
        hBtnDecrypt = CreateWindowW(L"BUTTON", L"开始解密",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P+GW-BW, y, BW, BH, hwnd, (HMENU)IDC_BTN_DECRYPT, hInst, NULL);

        /* ── 应用系统 GUI 字体到所有子控件 ── */
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        {
            HWND hc = GetWindow(hwnd, GW_CHILD);
            while (hc) {
                SendMessageW(hc, WM_SETFONT, (WPARAM)hFont, FALSE);
                hc = GetWindow(hc, GW_HWNDNEXT);
            }
        }

        g_hwndLog = hLog; g_hwndProgress = hProg;
        g_hwndProcEdit = hProcEdit; g_hwndOutDir = hOutDir;
        g_hwndCheckAuto = hCheckAuto; g_hwndBtnDecrypt = hBtnDecrypt;
        g_hwndList = g_hwndListBox;
        /* 触发一次 WM_SIZE 以应用初始布局 */
        RECT cr; GetClientRect(hwnd, &cr);
        SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED,
                     MAKELPARAM(cr.right, cr.bottom));
        break;
    }

    case WM_GETMINMAXINFO: {
        /* 只限最小尺寸，用与 CreateWindowExW 相同的 WS_OVERLAPPEDWINDOW 计算 */
        RECT rc = {0, 0, GUI_MIN_CW, GUI_MIN_CH};
        AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_ACCEPTFILES);
        MINMAXINFO *mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = rc.right - rc.left;
        mm->ptMinTrackSize.y = rc.bottom - rc.top;
        /* 不设 ptMaxTrackSize，允许任意拉大 */
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hdrop = (HDROP)wp;
        UINT cnt = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < cnt; i++) {
            wchar_t path[MAX_PATH_LEN];
            if (DragQueryFileW(hdrop, i, path, MAX_PATH_LEN)) {
                BOOL dup = FALSE;
                for (int j = 0; j < g_path_cnt; j++)
                    if (wcscmp(g_paths[j], path) == 0) { dup = TRUE; break; }
                if (!dup && g_path_cnt < MAX_PATHS) {
                    g_paths[g_path_cnt++] = _wcsdup(path);
                    SendMessageW(g_hwndListBox, LB_ADDSTRING, 0, (LPARAM)path);
                }
            }
        }
        DragFinish(hdrop);
        return 0;
    }


    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        int cw = LOWORD(lp);
        int ch = HIWORD(lp);
        const int P   = 8;
        const int BH  = 24;
        const int EH  = 22;
        const int BW  = 80;
        const int GW  = cw - 2*P;
        const int IW  = GW - 16;
        const int BTN_H = BH + 8;   /* 底部按钮行高 32 */
        const int CFG_H = 92;       /* 配置区固定高 */

        /* 文件列表高度 = 客户区高 35%，最小 80 */
        int list_h = ch * 35 / 100;
        if (list_h < 80) list_h = 80;
        int gb1_h = 48 + list_h;
        int y = 4;

        /* 文件 GroupBox */
        SetWindowPos(hGbFiles, NULL, P, y, GW, gb1_h, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(hBtnAddFile, NULL, P+8, y+18, BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(hBtnAddDir,  NULL, P+8+BW+8, y+18, BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd, IDC_BTN_CLEAR), NULL, P+GW-8-BW, y+18, BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_hwndListBox, NULL, P+8, y+48, IW, list_h, SWP_NOZORDER|SWP_NOACTIVATE);

        /* 配置 GroupBox */
        y += gb1_h + 4;
        SetWindowPos(hGbCfg, NULL, P, y, GW, CFG_H, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(hCheckAuto,    NULL, P+8,         y+18, 280, 20, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(hStaticProc,   NULL, P+8,         y+44, 76,  20, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_hwndProcEdit,NULL, P+8+76+4,    y+42, 148, EH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(hStaticHint,   NULL, P+8+76+4+148+6, y+44, 120, 20, SWP_NOZORDER|SWP_NOACTIVATE);
        {
            const int lw=68, bw2=56, gap=4;
            int ew = IW - lw - gap - bw2 - gap;
            if (ew < 60) ew = 60;
            SetWindowPos(hStaticOutLabel, NULL, P+8,              y+68, lw,  20, SWP_NOZORDER|SWP_NOACTIVATE);
            SetWindowPos(g_hwndOutDir,    NULL, P+8+lw+gap,       y+66, ew,  EH, SWP_NOZORDER|SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(hwnd, IDC_BTN_BROWSE), NULL,
                P+8+lw+gap+ew+gap, y+66, bw2, EH, SWP_NOZORDER|SWP_NOACTIVATE);
        }

        /* 日志 GroupBox（填满剩余） */
        y += CFG_H + 4;
        int log_gb_h = ch - y - BTN_H - 4;
        if (log_gb_h < 60) log_gb_h = 60;
        SetWindowPos(hGbLog, NULL, P, y, GW, log_gb_h, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_hwndProgress, NULL, P+8, y+18, IW, 16, SWP_NOZORDER|SWP_NOACTIVATE);
        int log_edit_h = log_gb_h - 40 - 8;
        if (log_edit_h < 30) log_edit_h = 30;
        SetWindowPos(g_hwndLog, NULL, P+8, y+40, IW, log_edit_h, SWP_NOZORDER|SWP_NOACTIVATE);

        /* 底部按钮行 */
        int by = ch - BTN_H + 4;
        SetWindowPos(GetDlgItem(hwnd, IDC_BTN_INSTALL),   NULL, P,        by, 96, BH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd, IDC_BTN_UNINSTALL), NULL, P+96+6,   by, 96, BH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_hwndBtnDecrypt, NULL, P+GW-BW, by, BW, BH, SWP_NOZORDER|SWP_NOACTIVATE);

        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BTN_ADDFILE) {
            OPENFILENAMEW ofn = {sizeof(ofn)};
            /* 多文件选择缓冲：目录名+文件名列表，32KB wchar 够几十个路径 */
            wchar_t *files = (wchar_t*)calloc(MAX_PATH*32, sizeof(wchar_t));
            if (!files) break;
            ofn.hwndOwner  = hwnd;
            ofn.lpstrFile  = files;
            ofn.nMaxFile   = MAX_PATH*32;
            ofn.lpstrTitle = L"选择文件";
            ofn.Flags      = OFN_ALLOWMULTISELECT|OFN_EXPLORER|OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                wchar_t *dir = files;
                wchar_t *p   = files + wcslen(dir) + 1;
                if (*p == 0) {
                    if (g_path_cnt < MAX_PATHS) {
                        BOOL dup=FALSE;
                        for(int j=0;j<g_path_cnt;j++)
                            if(wcscmp(g_paths[j],dir)==0){dup=TRUE;break;}
                        if(!dup){
                            g_paths[g_path_cnt++] = _wcsdup(dir);
                            SendMessageW(g_hwndListBox, LB_ADDSTRING, 0, (LPARAM)dir);
                        }
                    }
                } else {
                    while (*p) {
                        wchar_t full[MAX_PATH];
                        _snwprintf(full, MAX_PATH-1, L"%s\\%s", dir, p);
                        BOOL dup=FALSE;
                        for(int j=0;j<g_path_cnt;j++)
                            if(wcscmp(g_paths[j],full)==0){dup=TRUE;break;}
                        if (!dup && g_path_cnt < MAX_PATHS) {
                            g_paths[g_path_cnt++] = _wcsdup(full);
                            SendMessageW(g_hwndListBox, LB_ADDSTRING, 0, (LPARAM)full);
                        }
                        p += wcslen(p) + 1;
                    }
                }
            }
            free(files);
        } else if (id == IDC_BTN_ADDDIR) {
            BROWSEINFOW bi = {0};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"选择文件夹";
            bi.ulFlags   = BIF_RETURNONLYFSDIRS|BIF_USENEWUI;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH_LEN];
                if (SHGetPathFromIDListW(pidl, path)) {
                    BOOL dup=FALSE;
                    for(int j=0;j<g_path_cnt;j++)
                        if(wcscmp(g_paths[j],path)==0){dup=TRUE;break;}
                    if (!dup && g_path_cnt < MAX_PATHS) {
                        g_paths[g_path_cnt++] = _wcsdup(path);
                        SendMessageW(g_hwndListBox, LB_ADDSTRING, 0, (LPARAM)path);
                    }
                }
                CoTaskMemFree(pidl);
            }
        } else if (id == IDC_BTN_CLEAR) {
            for (int i=0;i<g_path_cnt;i++) free(g_paths[i]);
            g_path_cnt = 0;
            SendMessageW(g_hwndListBox, LB_RESETCONTENT, 0, 0);
        } else if (id == IDC_BTN_BROWSE) {
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
        } else if (id == IDC_CHECK_AUTO) {
            BOOL checked = (SendMessageW(hCheckAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(hProcEdit, !checked);
            if (checked) SetWindowTextW(hProcEdit, L"");
        } else if (id == IDC_BTN_INSTALL) {
            wchar_t msg2[256];
            install_context_menu(msg2, 256);
            MessageBoxW(hwnd, msg2, L"右键菜单", MB_OK|MB_ICONINFORMATION);
        } else if (id == IDC_BTN_UNINSTALL) {
            wchar_t msg2[256];
            uninstall_context_menu(msg2, 256);
            MessageBoxW(hwnd, msg2, L"右键菜单", MB_OK|MB_ICONINFORMATION);
        } else if (id == IDC_BTN_DECRYPT && !is_decrypting) {
            if (g_path_cnt == 0) {
                MessageBoxW(hwnd, L"请先添加要解密的文件或文件夹", L"提示",
                            MB_OK|MB_ICONWARNING);
                break;
            }
            BOOL auto_mode = (SendMessageW(hCheckAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
            wchar_t proc_buf[MAX_PATH] = {0};
            if (!auto_mode) GetWindowTextW(hProcEdit, proc_buf, MAX_PATH);
            if (!auto_mode && !proc_buf[0]) {
                MessageBoxW(hwnd, L"已关闭自动模式，请填写进程名", L"提示",
                            MB_OK|MB_ICONWARNING);
                break;
            }
            wchar_t out_buf[MAX_PATH] = {0};
            GetWindowTextW(hOutDir, out_buf, MAX_PATH);
            if (out_buf[0]) CreateDirectoryW(out_buf, NULL);

            is_decrypting = TRUE;
            EnableWindow(hBtnDecrypt, FALSE);
            SendMessageW(hProg, PBM_SETPOS, 0, 0);
            SetWindowTextW(hLog, L"");

            wchar_t mode_buf[512];
            _snwprintf(mode_buf, 511, L"模式：%s", auto_mode ? L"自动" : proc_buf);
            int mlen = GetWindowTextLengthW(hLog);
            SendMessageW(hLog, EM_SETSEL, mlen, mlen);
            SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)mode_buf);
            SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");

            start_decrypt_thread(hwnd, g_paths, g_path_cnt,
                                 auto_mode ? NULL : proc_buf,
                                 out_buf[0] ? out_buf : NULL);
        }
        return 0;
    }

    case WM_WORKER_LOG: {
        wchar_t *s = (wchar_t*)lp;
        if (s && hLog) {
            int len = GetWindowTextLengthW(hLog);
            SendMessageW(hLog, EM_SETSEL, len, len);
            SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)s);
            SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
            free(s);
        }
        return 0;
    }
    case WM_WORKER_PROG:
        if (hProg) SendMessageW(hProg, PBM_SETPOS, wp, 0);
        return 0;
    case WM_WORKER_DONE:
        is_decrypting = FALSE;
        if (hBtnDecrypt) EnableWindow(hBtnDecrypt, TRUE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void run_main_gui(wchar_t **init_paths, int n) {
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpszClassName = L"YST_MainWnd";
    wc.lpfnWndProc   = MainWndProc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    /* 初始外框尺寸由 GUI_CLIENT_W×GUI_CLIENT_H 推算，窗口可拖大 */
    DWORD wstyle = WS_OVERLAPPEDWINDOW;
    RECT rc2 = {0, 0, GUI_CLIENT_W, GUI_CLIENT_H};
    AdjustWindowRectEx(&rc2, wstyle, FALSE, WS_EX_ACCEPTFILES);
    int fw = rc2.right - rc2.left;
    int fh = rc2.bottom - rc2.top;
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"YST_MainWnd", APP_TITLE,
        wstyle,
        (sw-fw)/2, (sh-fh)/2, fw, fh,
        NULL, NULL, hInst, NULL);

    /* 解除 UIPI 过滤，允许资源管理器（低完整性）发 WM_DROPFILES 到本窗口 */
    ChangeWindowMessageFilterEx(g_hwnd, WM_DROPFILES,    MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA,     MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, NULL);
    DragAcceptFiles(g_hwnd, TRUE);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    /* 初始路径（WM_CREATE 已执行，g_hwndListBox 已有效） */
    if (init_paths) {
        for (int i = 0; i < n; i++) {
            if (g_path_cnt < MAX_PATHS) {
                g_paths[g_path_cnt++] = _wcsdup(init_paths[i]);
                SendMessageW(g_hwndListBox, LB_ADDSTRING, 0, (LPARAM)init_paths[i]);
            }
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ── WinMain 入口 ──────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

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
