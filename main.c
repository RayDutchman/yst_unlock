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
#define IDC_EDIT_FALLBACK      114  /* 兜底进程名 Edit */
#define IDC_BTN_EXT_MAP        115  /* 打开扩展名映射管理对话框 */
#define IDC_LIST_EXT_MAP       116  /* 映射摘要 Edit（主窗口） */
#define IDC_STATIC_EXTMAP_LBL  117  /* 行3 扩展名映射 Static 标签 */
#define IDC_STATIC_FALLBACK_LBL 118 /* 行4 兜底进程名 Static 标签 */
#define IDC_STATIC_FALLBACK_HINT 119 /* 行4 提示文字 Static */

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

/* ── 扩展名→进程名映射表（从 INI 加载，优先于注册表） ─── */
#define MAX_EXT_MAP 64
typedef struct { wchar_t ext[32]; wchar_t proc[MAX_PATH_LEN]; } ExtMapEntry;
static ExtMapEntry g_ext_map[MAX_EXT_MAP];
static int         g_ext_map_cnt = 0;
static wchar_t     g_fallback_proc[MAX_PATH_LEN] = L"POWERPNT.EXE";
static wchar_t     g_ini_path[MAX_PATH_LEN];  /* exe 同目录的 ini 文件路径 */

/* ── 工具函数 ─────────────────────────────────────────────── */

/* 获取当前 exe 的完整路径 */
static void get_self_path(wchar_t *buf, int len) {
    GetModuleFileNameW(NULL, buf, len);
}

/* 字符串转小写（in-place） */
static void wcs_lower(wchar_t *s) {
    for (; *s; s++) *s = (wchar_t)towlower(*s);
}

/* 保存文件时间戳到目标文件 */
static void preserve_file_time(const wchar_t *path,
                                const FILETIME *create,
                                const FILETIME *access,
                                const FILETIME *write) {
    HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES,
                           0, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, create, access, write);
        CloseHandle(h);
    }
}

/* ── INI 持久化 ──────────────────────────────────────────── */

static void init_ini_path(void) {
    wchar_t exe[MAX_PATH_LEN];
    get_self_path(exe, MAX_PATH_LEN);
    wchar_t *bs = wcsrchr(exe, L'\\');
    if (bs) { *(bs+1) = 0; } else { exe[0] = 0; }
    _snwprintf(g_ini_path, MAX_PATH_LEN-1, L"%syst_unlock.ini", exe);
    g_ini_path[MAX_PATH_LEN-1] = 0;
}

/* 从 INI 加载配置（FallbackProc 和 ExtMap） */
static void load_ini(void) {
    /* FallbackProc */
    wchar_t buf[MAX_PATH_LEN];
    if (GetPrivateProfileStringW(L"Config", L"FallbackProc", L"POWERPNT.EXE",
                                 buf, MAX_PATH_LEN, g_ini_path) > 0)
        wcsncpy(g_fallback_proc, buf, MAX_PATH_LEN-1);

    /* ExtMap：遍历 [ExtMap] 段所有键 */
    wchar_t *keys = (wchar_t*)calloc(8192, sizeof(wchar_t));
    if (!keys) return;
    DWORD ret = GetPrivateProfileSectionW(L"ExtMap", keys, 8192, g_ini_path);
    g_ext_map_cnt = 0;
    if (ret > 0) {
        wchar_t *p = keys;
        while (*p && g_ext_map_cnt < MAX_EXT_MAP) {
            /* 每条格式：.xlsx=EXCEL.EXE */
            wchar_t *eq = wcschr(p, L'=');
            if (eq) {
                int elen = (int)(eq - p);
                if (elen > 0 && elen < 32) {
                    wcsncpy(g_ext_map[g_ext_map_cnt].ext, p, elen);
                    g_ext_map[g_ext_map_cnt].ext[elen] = 0;
                    wcs_lower(g_ext_map[g_ext_map_cnt].ext);
                    wcsncpy(g_ext_map[g_ext_map_cnt].proc, eq+1, MAX_PATH_LEN-1);
                    g_ext_map_cnt++;
                }
            }
            p += wcslen(p) + 1;
        }
    }
    free(keys);
}

/* 保存 INI（覆写整个文件） */
static void save_ini(void) {
    /* FallbackProc */
    WritePrivateProfileStringW(L"Config", L"FallbackProc",
                               g_fallback_proc, g_ini_path);
    /* 清空旧 ExtMap 段，逐条写入 */
    WritePrivateProfileStringW(L"ExtMap", NULL, NULL, g_ini_path);
    for (int i = 0; i < g_ext_map_cnt; i++)
        WritePrivateProfileStringW(L"ExtMap",
                                   g_ext_map[i].ext,
                                   g_ext_map[i].proc,
                                   g_ini_path);
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
    wchar_t tmp[MAX_PATH_LEN];  /* 路径含参数，留足空间 */
    wcsncpy(tmp, cmd, MAX_PATH_LEN - 1);
    tmp[MAX_PATH_LEN - 1] = 0;
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
    wchar_t *cmd     = (wchar_t*)calloc(MAX_PATH_LEN, sizeof(wchar_t));
    if (!prog_id || !uc_path || !cmd_path || !cmd) {
        free(prog_id); free(uc_path); free(cmd_path); free(cmd);
        return FALSE;
    }
    BOOL result = FALSE;

    /* 用户级关联 UserChoice */
    _snwprintf(uc_path, 511,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
        ext);
    uc_path[511] = 0;
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
            cmd_path[1023] = 0;
            if (!reg_query_str(HKEY_CLASSES_ROOT, cmd_path, L"", cmd, MAX_PATH_LEN))
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
        shell_path[1023] = 0;
        _snwprintf(cmd_path,   1023, L"%s\\command", shell_path);
        cmd_path[1023] = 0;
        /* Background\shell 用 %V，其余用 %1 */
        const wchar_t *ph = wcsstr(REG_ROOTS[i], L"Background") ? L"%V" : L"%1";
        _snwprintf(cmd, MAX_PATH_LEN - 1,
                   L"\"%s\" --silent \"%s\"", self_exe, ph);
        cmd[MAX_PATH_LEN - 1] = 0;
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
        shell_path[1023] = 0;
        _snwprintf(cmd_path,   1023, L"%s\\command",    shell_path);
        cmd_path[1023] = 0;
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
    wchar_t lower[MAX_PATH_LEN];
    wcsncpy(lower, name, MAX_PATH_LEN - 1);
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

/* 递归遍历目录 — 使用显式堆栈，避免深层目录导致的栈溢出；
 * 同时跳过符号链接/目录交汇点（FILE_ATTRIBUTE_REPARSE_POINT），
 * 防止循环链接引发无限遍历。 */
static void walk_dir(const wchar_t *dir_root, FileList *fl) {
    /* 显式目录队列 */
    int   cap  = 256;
    int   head = 0, tail = 0;
    wchar_t **queue = (wchar_t**)malloc(cap * sizeof(wchar_t*));
    if (!queue) return;
    queue[tail++] = _wcsdup(dir_root);

    while (head < tail) {
        wchar_t *cur = queue[head++];

        wchar_t pattern[MAX_PATH_LEN];
        _snwprintf(pattern, MAX_PATH_LEN - 1, L"%s\\*", cur);
        pattern[MAX_PATH_LEN - 1] = 0;
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW(pattern, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 ||
                    wcscmp(fd.cFileName, L"..") == 0) continue;
                /* 跳过符号链接 / 目录交汇点（防循环） */
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

                wchar_t full[MAX_PATH_LEN];
                _snwprintf(full, MAX_PATH_LEN - 1, L"%s\\%s", cur, fd.cFileName);
                full[MAX_PATH_LEN - 1] = 0;

                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!should_skip_dir(fd.cFileName)) {
                        /* 入队 */
                        if (tail >= cap) {
                            cap *= 2;
                            wchar_t **tmp = (wchar_t**)realloc(queue, cap * sizeof(wchar_t*));
                            if (!tmp) {
                                /* realloc 失败：关闭句柄并退出全部遍历 */
                                FindClose(hf);
                                free(cur);
                                goto walk_cleanup;
                            }
                            queue = tmp;
                        }
                        queue[tail++] = _wcsdup(full);
                    }
                } else {
                    if (!should_skip_file(full))
                        filelist_push(fl, full);
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        free(cur);
    }
walk_cleanup:
    /* 释放队列中尚未处理的条目 */
    for (int i = head; i < tail; i++) free(queue[i]);
    free(queue);
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

#define COPY_BUF_SIZE (4 * 1024 * 1024)  /* 读写缓冲块大小：4MB */

/*
 * --worker 模式：从 stdin 循环读取任务行 "src\tdst\n"
 * 每条回写 "OK\n" 或 "ERR:msg\n" 到 stdout，EOF 时退出。
 * 以当前进程名（伪装白名单进程）运行。
 */
static void worker_mode(void) {
    /* 切换为二进制模式，避免翻译问题 */
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    /* IPC 协议：父进程发 "src_utf8\tdst_utf8\n"
     * worker（白名单进程）ReadFile(src) 触发亿赛通解密，再 WriteFile(dst)
     * 回报 "OK\n" 或 "ERR:msg\n"，父进程只需做 MoveFileExW(dst→src) */
    char line[MAX_PATH_LEN * 8];
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
        if (!MultiByteToWideChar(CP_UTF8, 0, src_u8, -1, src_w, MAX_PATH_LEN) ||
            !MultiByteToWideChar(CP_UTF8, 0, dst_u8, -1, dst_w, MAX_PATH_LEN)) {
            fputs("ERR:utf8 decode failed\n", stdout); fflush(stdout); continue;
        }

        /* 白名单进程打开源文件（触发亿赛通解密过滤器），读入内存 */
        HANDLE hSrc = CreateFileW(src_w, GENERIC_READ,
                                  FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hSrc == INVALID_HANDLE_VALUE) {
            fprintf(stdout, "ERR:open src failed (%lu)\n", GetLastError());
            fflush(stdout); continue;
        }

        /* 获取文件大小（仅用于缓冲预分配，实际以读到字节数为准） */
        LARGE_INTEGER fsz; fsz.QuadPart = 0;
        GetFileSizeEx(hSrc, &fsz);

        /* 读入堆内存（xlsx/docx 等 ZIP 格式加密后体积与密文不同，必须以实际读到字节数为准） */
        size_t buf_cap = (size_t)(fsz.QuadPart > 0 ? fsz.QuadPart : 65536) + 65536;
        char *fbuf = (char*)malloc(buf_cap);
        if (!fbuf) {
            fputs("ERR:out of memory\n", stdout); fflush(stdout);
            CloseHandle(hSrc); continue;
        }
        size_t fbuf_len = 0;
        BOOL read_ok = TRUE;
        for (;;) {
            if (fbuf_len + COPY_BUF_SIZE > buf_cap) {
                buf_cap = buf_cap * 2 + COPY_BUF_SIZE;
                char *tmp = (char*)realloc(fbuf, buf_cap);
                if (!tmp) {
                    free(fbuf);
                    fputs("ERR:out of memory\n", stdout); fflush(stdout);
                    read_ok = FALSE; break;
                }
                fbuf = tmp;
            }
            DWORD rd = 0;
            if (!ReadFile(hSrc, fbuf + fbuf_len, COPY_BUF_SIZE, &rd, NULL)) {
                free(fbuf);
                fprintf(stdout, "ERR:read failed (%lu)\n", GetLastError());
                fflush(stdout); read_ok = FALSE; break;
            }
            if (rd == 0) break;  /* EOF */
            fbuf_len += rd;
        }
        CloseHandle(hSrc);
        if (!read_ok) continue;

        /* 白名单进程写目标文件（dst = src.yst_tmp，与 src 同盘同目录，不被拦截） */
        HANDLE hDst = CreateFileW(dst_w, GENERIC_WRITE, 0, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hDst == INVALID_HANDLE_VALUE) {
            free(fbuf);
            fprintf(stdout, "ERR:open dst failed (%lu)\n", GetLastError());
            fflush(stdout); continue;
        }
        BOOL write_ok = TRUE;
        size_t sent = 0;
        while (sent < fbuf_len) {
            DWORD chunk = (DWORD)((fbuf_len - sent > COPY_BUF_SIZE)
                                  ? COPY_BUF_SIZE : fbuf_len - sent);
            DWORD wr = 0;
            if (!WriteFile(hDst, fbuf + sent, chunk, &wr, NULL) || wr == 0) {
                fprintf(stdout, "ERR:write dst failed (%lu)\n", GetLastError());
                fflush(stdout); write_ok = FALSE; break;
            }
            sent += wr;
        }
        CloseHandle(hDst);
        free(fbuf);
        if (!write_ok) { DeleteFileW(dst_w); continue; }

        fputs("OK\n", stdout);
        fflush(stdout);
    }
}


/* 递归删除目录（等价于 rmdir /s /q），跳过符号链接/交汇点防止误删 */
static void rmdir_recursive(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN-1, L"%s\\*", dir);
    pattern[MAX_PATH_LEN-1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pattern, &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            /* 跳过符号链接/交汇点，只删本目录内真实文件 */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
            wchar_t full[MAX_PATH_LEN];
            _snwprintf(full, MAX_PATH_LEN-1, L"%s\\%s", dir, fd.cFileName);
            full[MAX_PATH_LEN-1] = 0;
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
    cmd[MAX_PATH_LEN-1] = 0;
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

/* 向 worker 发送任务，返回 TRUE=成功。
 *
 * 协议：
 *   父进程发：src_utf8\n
 *   worker 回：SIZE:N\n + N字节二进制数据（白名单进程读文件，触发亿赛通解密）
 *   父进程收数据后，由自己（非白名单）写目标文件，不触发重新加密。
 */
static BOOL send_task(WorkerProc *wp,
                      const wchar_t *src, const wchar_t *dst,
                      char *err_out, int err_len) {
    /* 路径转 UTF-8 */
    int su = MAX_PATH_LEN * 4, du = MAX_PATH_LEN * 4;
    char *src_u8 = (char*)malloc(su);
    char *dst_u8 = (char*)malloc(du);
    if (!src_u8 || !dst_u8) {
        free(src_u8); free(dst_u8);
        strncpy(err_out, "out of memory", err_len-1); return FALSE;
    }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, src_u8, su, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, dst, -1, dst_u8, du, NULL, NULL);

    /* 发任务行：src_utf8 TAB dst_utf8 \n
     * worker（白名单进程）自己完成 ReadFile(src)+WriteFile(dst)，只回报 OK/ERR */
    int lu = su + du + 4;
    char *line = (char*)malloc(lu);
    if (!line) {
        free(src_u8); free(dst_u8);
        strncpy(err_out, "out of memory", err_len-1); return FALSE;
    }
    int n = snprintf(line, lu, "%s\t%s\n", src_u8, dst_u8);
    free(src_u8); free(dst_u8);
    DWORD written;
    BOOL pipe_ok = WriteFile(wp->hStdin_W, line, (DWORD)n, &written, NULL);
    free(line);
    if (!pipe_ok) { strncpy(err_out, "write to worker pipe failed", err_len-1); return FALSE; }

    /* 读响应行：OK 或 ERR:msg */
    char resp[512] = {0};
    int ri = 0;
    char c; DWORD rd;
    while (ri < (int)sizeof(resp)-1) {
        if (!ReadFile(wp->hStdout_R, &c, 1, &rd, NULL) || rd == 0) {
            strncpy(err_out, "worker pipe closed unexpectedly", err_len-1); return FALSE;
        }
        if (c == '\n') break;
        resp[ri++] = c;
    }
        resp[ri] = 0;
    if (strcmp(resp, "OK") == 0) return TRUE;
    if (strncmp(resp, "ERR:", 4) == 0)
        strncpy(err_out, resp+4, err_len-1);
    else {
        strncpy(err_out, "unexpected response", err_len-1);
        strncat(err_out, resp, (size_t)(err_len - 1 - (int)strlen(err_out)));
    }
    return FALSE;
}


/* ── 解密线程参数结构 ─────────────────────────────────────────── */

typedef struct {
    wchar_t **paths;
    int       path_cnt;
    wchar_t   proc_override[MAX_PATH_LEN];  /* 非空 = 全局覆盖，所有文件用此进程名 */
    wchar_t   fallback_proc[MAX_PATH_LEN];  /* 注册表查不到时的兜底进程名 */
    wchar_t   output_dir[MAX_PATH_LEN];     /* 空 = 覆盖原文件 */
    /* 扩展名映射表（指向全局 g_ext_map，只读）*/
    const ExtMapEntry *ext_map;
    int                ext_map_cnt;
    HWND      notify_hwnd;
} DecryptArgs;

/* 工作线程输出：日志字符串（内存由接收方 free）、进度小数 */
#define NOTIFY_LOG(hwnd, s)  PostMessageW((hwnd), WM_WORKER_LOG,  0, (LPARAM)_wcsdup(s))
#define NOTIFY_PROG(hwnd, v) PostMessageW((hwnd), WM_WORKER_PROG, (WPARAM)(int)(v), 0)
#define NOTIFY_DONE(hwnd, ok) PostMessageW((hwnd), WM_WORKER_DONE, (WPARAM)(ok), 0)

/* 每种进程名对应一个 WorkerProc（最多 16 种） */
#define MAX_WORKERS 16
typedef struct { wchar_t name[MAX_PATH_LEN]; WorkerProc wp; } WorkerEntry;

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
        buf[127] = 0;
        NOTIFY_LOG(hwnd, buf);
    }

    /* 自身 exe 路径 */
    wchar_t self_exe[MAX_PATH_LEN];
    get_self_path(self_exe, MAX_PATH_LEN);

    /* 创建临时目录 */
    wchar_t tmp_dir[MAX_PATH_LEN];
    GetTempPathW(MAX_PATH_LEN, tmp_dir);
    {
        wchar_t sub[64];
        _snwprintf(sub, 63, L"yst_unlock_%lu\\", GetCurrentProcessId());
        sub[63] = 0;
        wcscat(tmp_dir, sub);
    }
    CreateDirectoryW(tmp_dir, NULL);

    wchar_t common_root[MAX_PATH_LEN] = {0};
    if (args->output_dir[0]) {
        /* 简化：公共根 = 第一个路径的父目录 */
        if (args->path_cnt >= 1) {
            wcsncpy(common_root, args->paths[0], MAX_PATH_LEN-1);
            DWORD attr = GetFileAttributesW(common_root);
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t *bs = wcsrchr(common_root, L'\\');
                if (bs) *bs = 0;
            }
        }
        CreateDirectoryW(args->output_dir, NULL);  /* 单级；深层目录由每文件的 SHCreateDirectoryExW 保障 */
    }

    WorkerEntry workers[MAX_WORKERS];
    int worker_cnt = 0;
    int success_count = 0, fail_count = 0;

    /* 记录兜底扩展名（最多 64 种，去重） */
#define MAX_FALLBACK_EXTS 64
    wchar_t fallback_exts[MAX_FALLBACK_EXTS][32];
    int fallback_ext_cnt = 0;

    for (int idx = 0; idx < fl.count; idx++) {
        const wchar_t *src = fl.items[idx];

        /* 确定进程名：映射表 > 注册表 > 兜底 */
        wchar_t proc_name[MAX_PATH_LEN];
        /* 先查扩展名映射表（优先级最高，覆盖注册表） */
        BOOL found_in_map = FALSE;
        if (!args->proc_override[0]) {
            const wchar_t *dot2 = wcsrchr(src, L'.');
            if (dot2) {
                wchar_t ext2[32];
                wcsncpy(ext2, dot2, 31); ext2[31]=0;
                wcs_lower(ext2);
                for (int mi=0; mi < args->ext_map_cnt; mi++) {
                    if (wcscmp(args->ext_map[mi].ext, ext2) == 0) {
                        wcsncpy(proc_name, args->ext_map[mi].proc, MAX_PATH_LEN-1);
                        found_in_map = TRUE;
                        break;
                    }
                }
            }
        }
        if (!found_in_map &&
            !resolve_process_name(src, args->proc_override, proc_name, MAX_PATH_LEN)) {
            /* 记录兜底扩展名（去重） */
            const wchar_t *dot = wcsrchr(src, L'.');
            if (dot && fallback_ext_cnt < MAX_FALLBACK_EXTS) {
                wchar_t ext_lower[32];
                wcsncpy(ext_lower, dot, 31); ext_lower[31] = 0;
                wcs_lower(ext_lower);
                BOOL found = FALSE;
                for (int ei = 0; ei < fallback_ext_cnt; ei++)
                    if (wcscmp(fallback_exts[ei], ext_lower) == 0) { found = TRUE; break; }
                if (!found)
                    wcsncpy(fallback_exts[fallback_ext_cnt++], ext_lower, 31);
            }
            /* 兜底：用用户配置的兜底进程名 */
            wcsncpy(proc_name, args->fallback_proc, MAX_PATH_LEN-1);
        }

        /* 查找或创建 worker */
        WorkerEntry *we = NULL;
        for (int w = 0; w < worker_cnt; w++)
            if (wcscmp(workers[w].name, proc_name) == 0) { we = &workers[w]; break; }
        if (!we) {
            if (worker_cnt < MAX_WORKERS) {
                we = &workers[worker_cnt++];
                wcsncpy(we->name, proc_name, MAX_PATH_LEN-1);
                memset(&we->wp, 0, sizeof(we->wp));
            }
        }
        /* worker 进程未在运行，启动之 */
        if (we && (!we->wp.hProcess ||
                   WaitForSingleObject(we->wp.hProcess, 0) != WAIT_TIMEOUT)) {
            wchar_t fake_exe[MAX_PATH_LEN];
            _snwprintf(fake_exe, MAX_PATH_LEN-1, L"%s%s", tmp_dir, proc_name);
            fake_exe[MAX_PATH_LEN-1] = 0;
            if (GetFileAttributesW(fake_exe) == INVALID_FILE_ATTRIBUTES)
                CopyFileW(self_exe, fake_exe, FALSE);
            stop_worker(&we->wp);
            if (!start_worker(fake_exe, &we->wp)) {
                wchar_t buf[512];
                _snwprintf(buf, 511, L"[%d/%d] 失败 [%s]: 启动 worker 失败",
                           idx+1, total, proc_name);
                buf[511] = 0;
                NOTIFY_LOG(hwnd, buf);
                fail_count++;
                NOTIFY_PROG(hwnd, (idx+1)*100/total);
                continue;
            }
        }

        /* 确定输出路径：
         * - 有输出目录：写到输出目录（该目录不在亿赛通保护范围内）
         * - 覆盖模式：dst = src + ".yst_tmp"（与源文件同盘同目录）
         *   worker（白名单进程）ReadFile(src)+WriteFile(dst)，均在同盘，不被拦截；
         *   父进程只做 MoveFileExW(dst→src)，同盘原子操作，必然成功。 */
        wchar_t dst[MAX_PATH_LEN];
        if (args->output_dir[0] && common_root[0]) {
            /* 计算相对路径 */
            const wchar_t *rel = src;
            int cr_len = (int)wcslen(common_root);
            if (wcsncmp(src, common_root, cr_len) == 0)
                rel = src + cr_len + 1;
            _snwprintf(dst, MAX_PATH_LEN-1, L"%s\\%s", args->output_dir, rel);
            dst[MAX_PATH_LEN-1] = 0;
            /* 创建父目录（SHCreateDirectoryExW 可多级创建，已存在时返回 ERROR_ALREADY_EXISTS） */
            wchar_t parent[MAX_PATH_LEN];
            wcsncpy(parent, dst, MAX_PATH_LEN-1);
            wchar_t *bs = wcsrchr(parent, L'\\');
            if (bs) { *bs = 0; SHCreateDirectoryExW(NULL, parent, NULL); }
        } else {
            /* 覆盖模式：临时文件与源文件同盘同目录，保证 MoveFileExW 同盘成功 */
            _snwprintf(dst, MAX_PATH_LEN-1, L"%s.yst_tmp", src);
            dst[MAX_PATH_LEN-1] = 0;
        }

        /* 发送任务 */
        char err[512] = {0};
        BOOL ok = we ? send_task(&we->wp, src, dst, err, 512) : FALSE;

        if (ok && !args->output_dir[0]) {
            /* 保存原文件时间戳 */
            HANDLE hTimeSrc = CreateFileW(src, FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS, NULL);
            FILETIME ft_create, ft_access, ft_write;
            BOOL got_time = (hTimeSrc != INVALID_HANDLE_VALUE) &&
                            GetFileTime(hTimeSrc, &ft_create, &ft_access, &ft_write);
            if (hTimeSrc != INVALID_HANDLE_VALUE) CloseHandle(hTimeSrc);

            /* 覆盖原文件 */
            if (!MoveFileExW(dst, src, MOVEFILE_REPLACE_EXISTING)) {
                DWORD e = GetLastError();
                snprintf(err, 512, "替换原文件失败 (%lu)", e);
                ok = FALSE;
                DeleteFileW(dst);
            } else if (got_time) {
                preserve_file_time(src, &ft_create, &ft_access, &ft_write);
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
            buf[1023] = 0;
        } else {
            fail_count++;
            wchar_t werr[512] = {0};
            MultiByteToWideChar(CP_UTF8, 0, err, -1, werr, 512);
            _snwprintf(buf, 1023, L"[%d/%d] 失败 [%s]: %s — %s",
                       idx+1, total, proc_name, fname, werr);
            buf[1023] = 0;
        }
        NOTIFY_LOG(hwnd, buf);
        NOTIFY_PROG(hwnd, (idx+1)*100/total);
    }

    /* 关闭所有 worker */
    for (int w = 0; w < worker_cnt; w++) stop_worker(&workers[w].wp);
    /* 删除临时目录（同步，避免竞态） */
    rmdir_recursive(tmp_dir);
    {
        wchar_t buf[256];
        _snwprintf(buf, 255, L"\n完成：成功 %d 个，失败 %d 个", success_count, fail_count);
        buf[255] = 0;
        NOTIFY_LOG(hwnd, buf);
    }
    /* 输出兜底扩展名摘要 */
    if (fallback_ext_cnt > 0) {
        wchar_t hint[128];
        _snwprintf(hint, 127, L"[提示] 以下扩展名未找到注册表默认进程，已用 %s 兜底解密：",
                   args->fallback_proc);
        hint[127] = 0;
        NOTIFY_LOG(hwnd, hint);
        /* 把所有扩展名拼成一行，每个用空格分隔 */
        wchar_t extline[MAX_FALLBACK_EXTS * 34];
        extline[0] = L' '; extline[1] = L' '; extline[2] = 0;
        for (int ei = 0; ei < fallback_ext_cnt; ei++) {
            if (ei > 0) wcsncat(extline, L"  ", sizeof(extline)/sizeof(wchar_t) - wcslen(extline) - 1);
            wcsncat(extline, fallback_exts[ei], sizeof(extline)/sizeof(wchar_t) - wcslen(extline) - 1);
        }
        NOTIFY_LOG(hwnd, extline);
    }
    NOTIFY_DONE(hwnd, success_count > 0);

cleanup_args:
    filelist_free(&fl);
    for (int i = 0; i < args->path_cnt; i++) free(args->paths[i]);
    free(args->paths);
    free(args);
    return 0;
}

/* ── 辅助：启动解密线程，返回线程句柄（调用方负责 CloseHandle）
 * 返回 NULL 表示启动失败（内存不足或 CreateThread 失败）。 */
static HANDLE start_decrypt_thread(HWND notify_hwnd,
                                   wchar_t **paths, int path_cnt,
                                   const wchar_t *proc_override,
                                   const wchar_t *fallback_proc,
                                   const wchar_t *output_dir) {
    DecryptArgs *args = (DecryptArgs*)calloc(1, sizeof(DecryptArgs));
    if (!args) return NULL;
    args->notify_hwnd = notify_hwnd;
    args->path_cnt    = path_cnt;
    args->paths       = (wchar_t**)malloc(path_cnt * sizeof(wchar_t*));
    if (!args->paths) { free(args); return NULL; }
    for (int i = 0; i < path_cnt; i++)
        args->paths[i] = _wcsdup(paths[i]);
    if (proc_override)
        wcsncpy(args->proc_override, proc_override, MAX_PATH_LEN-1);
    /* 兜底进程名：有传入就用，否则用全局 g_fallback_proc */
    if (fallback_proc && fallback_proc[0])
        wcsncpy(args->fallback_proc, fallback_proc, MAX_PATH_LEN-1);
    else
        wcsncpy(args->fallback_proc, g_fallback_proc, MAX_PATH_LEN-1);
    /* 映射表：共享全局指针（只读，线程安全：解密期间映射表不会被修改） */
    args->ext_map     = g_ext_map;
    args->ext_map_cnt = g_ext_map_cnt;
    if (output_dir)
        wcsncpy(args->output_dir, output_dir, MAX_PATH_LEN-1);
    HANDLE ht = CreateThread(NULL, 0, decrypt_thread, args, 0, NULL);
    if (!ht) {
        for (int i = 0; i < path_cnt; i++) free(args->paths[i]);
        free(args->paths);
        free(args);
        return NULL;
    }
    return ht;
}

/* ── 进度窗口（--silent 模式） ─────────────────────────────────────── */

#define PROG_W 480
#define PROG_H 260  /* 客户区高，外框约 295px */

static HWND g_prog_hwnd     = NULL;
static HWND g_prog_bar      = NULL;
static HWND g_prog_log      = NULL;
static HWND g_prog_status   = NULL;
static HWND g_prog_btn      = NULL;
static UINT_PTR g_prog_timer= 0;
static HANDLE g_prog_thread = NULL;  /* 解密线程句柄，用于关闭前等待 */

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
            g_prog_thread = start_decrypt_thread(hwnd, g_paths, g_path_cnt, NULL, NULL, NULL);
            if (!g_prog_thread) {
                /* 启动失败，立即显示错误并启用关闭按钮 */
                SetWindowTextW(g_prog_status, L"启动解密线程失败");
                EnableWindow(g_prog_btn, TRUE);
            }
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
            buf[63] = 0;
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
            /* 等待解密线程结束，防止线程向已销毁窗口 PostMessage 导致内存泄漏 */
            if (g_prog_thread) {
                /* 在消息循环中等待，避免 UI 冻结（最多等 5 秒） */
                DWORD deadline = GetTickCount() + 5000;
                while (WaitForSingleObject(g_prog_thread, 0) == WAIT_TIMEOUT) {
                    if (GetTickCount() > deadline) {
                        /* 线程仍在运行，直接退出进程；
                         * TerminateThread 会留下堆锁/CRT 锁，
                         * 此处程序本来就要关闭，ExitProcess 更安全 */
                        ExitProcess(0);
                    }
                    MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&m); DispatchMessageW(&m);
                    }
                    Sleep(20);
                }
                CloseHandle(g_prog_thread);
                g_prog_thread = NULL;
            }
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
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

/* ── GUI 主窗口 ─────────────────────────────────────────────────── */

/* 最小客户区尺寸 */
#define GUI_MIN_CW 480
#define GUI_MIN_CH 470
/* 兼容旧引用（WM_CREATE 初始布局用） */
#define GUI_CLIENT_W 540
#define GUI_CLIENT_H 568
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
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX|WS_CLIPCHILDREN,
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
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|
            LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_EXTENDEDSEL,
            P+8, y+48, IW, 136, hwnd, (HMENU)IDC_LISTBOX, hInst, NULL);

        /* ── 配置 GroupBox (y+200, h=150, 5行, 行间距26px, 全部左对齐) ── */
        y += 200;
        hGbCfg = CreateWindowW(L"BUTTON", L"解密配置",
        WS_CHILD|WS_VISIBLE|BS_GROUPBOX|WS_CLIPCHILDREN,
        P, y, GW, 150, hwnd, NULL, hInst, NULL);
        /* 行1：自动模式复选框  y+20 */
        hCheckAuto = CreateWindowW(L"BUTTON", L"自动匹配进程名（推荐）",
        WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        P+8, y+20, 220, 20, hwnd, (HMENU)IDC_CHECK_AUTO, hInst, NULL);
        SendMessageW(hCheckAuto, BM_SETCHECK, BST_CHECKED, 0);
        /* 行2：覆写进程名  y+46 */
        hStaticProc = CreateWindowW(L"STATIC", L"覆写进程名:",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        P+8, y+47, 72, 20, hwnd, NULL, hInst, NULL);
        hProcEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        P+84, y+46, 160, EH, hwnd, (HMENU)IDC_EDIT_PROC, hInst, NULL);
        hStaticHint = CreateWindowW(L"STATIC", L"(留空=自动)",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        P+84+160+4, y+47, 90, 20, hwnd, NULL, hInst, NULL);
        /* 行3：扩展名映射  y+72 */
        CreateWindowW(L"STATIC", L"扩展名映射:",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        P+8, y+73, 72, 20, hwnd, (HMENU)IDC_STATIC_EXTMAP_LBL, hInst, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"已配置 0 条映射",
        WS_CHILD|WS_VISIBLE|ES_READONLY,
        P+84, y+72, 180, EH, hwnd, (HMENU)IDC_LIST_EXT_MAP, hInst, NULL);
        CreateWindowW(L"BUTTON", L"管理...",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        P+84+180+4, y+72, 60, EH, hwnd, (HMENU)IDC_BTN_EXT_MAP, hInst, NULL);
        /* 行4：兜底进程名  y+98 */
        CreateWindowW(L"STATIC", L"兜底进程名:",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        P+8, y+99, 72, 20, hwnd, (HMENU)IDC_STATIC_FALLBACK_LBL, hInst, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_fallback_proc,
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        P+84, y+98, 160, EH, hwnd, (HMENU)IDC_EDIT_FALLBACK, hInst, NULL);
        CreateWindowW(L"STATIC", L"(注册表未命中时)",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        P+84+160+4, y+99, 120, 20, hwnd, (HMENU)IDC_STATIC_FALLBACK_HINT, hInst, NULL);
        /* 行5：输出目录  y+124 */
        {
        const int lw = 68, bw2 = 56, gap = 4;
        const int ew = IW - lw - gap - bw2 - gap;
            hStaticOutLabel = CreateWindowW(L"STATIC", L"输出目录:",
                WS_CHILD|WS_VISIBLE|SS_LEFT,
                P+8, y+125, lw, 20, hwnd, NULL, hInst, NULL);
        hOutDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            P+8+lw+gap, y+124, ew, EH, hwnd, (HMENU)IDC_EDIT_OUTDIR, hInst, NULL);
        CreateWindowW(L"BUTTON", L"浏览...",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            P+8+lw+gap+ew+gap, y+124, bw2, EH, hwnd, (HMENU)IDC_BTN_BROWSE, hInst, NULL);
        }

        /* ── 日志 GroupBox (y+96, h=164) ── */
        y += 96;
        hGbLog = CreateWindowW(L"BUTTON", L"日志",
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX|WS_CLIPCHILDREN,
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
        /* 设置 ListBox 行高：与字体实际高度匹配，避免文字悬空 */
        {
            HDC hdc = GetDC(g_hwndListBox);
            HFONT hOldF = (HFONT)SelectObject(hdc, hFont);
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, hOldF);
            ReleaseDC(g_hwndListBox, hdc);
            /* 行高 = 字体高度 + 2px 上下各1px 内边距 */
            SendMessageW(g_hwndListBox, LB_SETITEMHEIGHT, 0, tm.tmHeight + 2);
        }

        /* 初始化映射摘要 Edit */
        {
            wchar_t summary[128];
            _snwprintf(summary, 127, L"已配置 %d 条映射", g_ext_map_cnt);
            summary[127] = 0;
            SetWindowTextW(GetDlgItem(hwnd, IDC_LIST_EXT_MAP), summary);
        }

        /* 初始化兜底进程名 Edit */
        SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_FALLBACK), g_fallback_proc);

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

    case WM_CONTEXTMENU: {
        /* 只处理 ListBox 上的右键 */
        if ((HWND)wp != g_hwndListBox) break;
        if (is_decrypting) break;  /* 解密中不允许修改列表 */
        if (SendMessageW(g_hwndListBox, LB_GETCOUNT, 0, 0) <= 0) break;

        int sx = (short)LOWORD(lp);
        int sy = (short)HIWORD(lp);

        int hit_idx = -1;
        if (sx == -1 && sy == -1) {
            /* 键盘触发（Shift+F10）：用当前选中项，若无则第0项 */
            int total_k = (int)SendMessageW(g_hwndListBox, LB_GETCOUNT, 0, 0);
            for (int ci = 0; ci < total_k; ci++)
                if (SendMessageW(g_hwndListBox, LB_GETSEL, ci, 0) > 0) { hit_idx = ci; break; }
            if (hit_idx < 0) hit_idx = 0;
            /* 弹出位置用 ListBox 左上角 */
            RECT rc; GetWindowRect(g_hwndListBox, &rc);
            sx = rc.left; sy = rc.top;
        } else {
            /* 鼠标触发：把屏幕坐标转成 ListBox 客户区坐标 */
            POINT pt = {sx, sy};
            ScreenToClient(g_hwndListBox, &pt);
            LRESULT res = SendMessageW(g_hwndListBox, LB_ITEMFROMPOINT, 0,
                                       MAKELPARAM(pt.x, pt.y));
            /* 高位为1表示鼠标在列表范围外 */
            if (HIWORD(res) == 0)
                hit_idx = (int)LOWORD(res);
        }

        if (hit_idx < 0) break;

        /* 选中右键点击的那一行（清掉其他选中） */
        SendMessageW(g_hwndListBox, LB_SETSEL, FALSE, -1); /* 全部取消 */
        SendMessageW(g_hwndListBox, LB_SETSEL, TRUE, hit_idx);

        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, 1, L"删除");
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD|TPM_RIGHTBUTTON,
                                 sx, sy, 0, hwnd, NULL);
        DestroyMenu(hMenu);

        if (cmd == 1) {
            /* 倒序删除所有选中项（键盘模式可能多选，鼠标模式只有1项） */
            int total = (int)SendMessageW(g_hwndListBox, LB_GETCOUNT, 0, 0);
            for (int i = total - 1; i >= 0; i--) {
                if (SendMessageW(g_hwndListBox, LB_GETSEL, i, 0) > 0) {
                    SendMessageW(g_hwndListBox, LB_DELETESTRING, i, 0);
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
        const int BTN_H = BH + 10;  /* 为底部按钮行预留空间：按钮24 + 上4 + 下12 = 40 */
        const int CFG_H = 150;      /* 配置区固定高（5行，行间距26px）*/

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

        /* 配置 GroupBox（5行，全部左对齐） */
        y += gb1_h + 4;
        SetWindowPos(hGbCfg, NULL, P, y, GW, CFG_H, SWP_NOZORDER|SWP_NOACTIVATE);
        /* 行1 y+20 */
        SetWindowPos(hCheckAuto, NULL, P+8, y+20, 220, 20, SWP_NOZORDER|SWP_NOACTIVATE);
        /* 行2 y+46 */
        SetWindowPos(hStaticProc,    NULL, P+8,        y+47, 72,  20, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_hwndProcEdit, NULL, P+84,       y+46, 160, EH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(hStaticHint,    NULL, P+84+160+4, y+47, 90,  20, SWP_NOZORDER|SWP_NOACTIVATE);
        /* 行3 y+72 */
        SetWindowPos(GetDlgItem(hwnd, IDC_STATIC_EXTMAP_LBL), NULL,
        P+8, y+73, 72, 20, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd, IDC_LIST_EXT_MAP), NULL,
        P+84, y+72, 180, EH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd, IDC_BTN_EXT_MAP), NULL,
            P+84+180+4, y+72, 60, EH, SWP_NOZORDER|SWP_NOACTIVATE);
        /* 行4 y+98 */
        SetWindowPos(GetDlgItem(hwnd, IDC_STATIC_FALLBACK_LBL), NULL,
        P+8, y+99, 72, 20, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd, IDC_EDIT_FALLBACK), NULL,
            P+84, y+98, 160, EH, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd, IDC_STATIC_FALLBACK_HINT), NULL,
            P+84+160+4, y+99, 120, 20, SWP_NOZORDER|SWP_NOACTIVATE);
        /* 行5 y+124 输出目录（弹性宽） */
        {
        const int lw=68, bw2=56, gap=4;
        int ew = IW - lw - gap - bw2 - gap;
        if (ew < 60) ew = 60;
        SetWindowPos(hStaticOutLabel, NULL, P+8,             y+125, lw,  20, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(g_hwndOutDir,    NULL, P+8+lw+gap,      y+124, ew,  EH, SWP_NOZORDER|SWP_NOACTIVATE);
            SetWindowPos(GetDlgItem(hwnd, IDC_BTN_BROWSE), NULL,
                 P+8+lw+gap+ew+gap, y+124, bw2, EH, SWP_NOZORDER|SWP_NOACTIVATE);
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
        int by = ch - BH - 10;  /* 按钮底边距窗口底部 6px */
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
                        wchar_t full[MAX_PATH_LEN];
                        _snwprintf(full, MAX_PATH_LEN-1, L"%s\\%s", dir, p);
                        full[MAX_PATH_LEN-1] = 0;
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
            (void)0;  /* 无需额外操作 */
        } else if (id == IDC_EDIT_FALLBACK &&
                   (HIWORD(wp) == EN_KILLFOCUS || HIWORD(wp) == EN_CHANGE)) {
            /* 兜底进程名更新：同步到全局变量并写 INI */
            HWND hFb = GetDlgItem(hwnd, IDC_EDIT_FALLBACK);
            GetWindowTextW(hFb, g_fallback_proc, MAX_PATH_LEN);
            if (!g_fallback_proc[0]) wcscpy(g_fallback_proc, L"POWERPNT.EXE");
            save_ini();
        } else if (id == IDC_BTN_EXT_MAP) {
            /* 弹出扩展名映射管理对话框 */
            extern void show_ext_map_dialog(HWND parent);
            show_ext_map_dialog(hwnd);
            /* 更新映射摘要显示 */
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
            if (g_path_cnt == 0) {
                MessageBoxW(hwnd, L"请先添加要解密的文件或文件夹", L"提示",
                            MB_OK|MB_ICONWARNING);
                break;
            }
            BOOL auto_mode = (SendMessageW(hCheckAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
            /* 覆写进程名（仅手动模式有效；自动模式下此栏留空表示不覆写） */
            wchar_t proc_buf[MAX_PATH_LEN] = {0};
            if (!auto_mode) {
                GetWindowTextW(hProcEdit, proc_buf, MAX_PATH_LEN);
                if (!proc_buf[0]) {
                    MessageBoxW(hwnd, L"手动模式下请填写覆写进程名", L"提示",
                                MB_OK|MB_ICONWARNING);
                    break;
                }
            }
            /* 兜底进程名从全局取（已由 IDC_EDIT_FALLBACK 实时同步） */
            wchar_t fallback_buf[MAX_PATH_LEN];
            wcsncpy(fallback_buf, g_fallback_proc, MAX_PATH_LEN-1);
            if (!fallback_buf[0]) wcscpy(fallback_buf, L"POWERPNT.EXE");
            wchar_t out_buf[MAX_PATH_LEN] = {0};
            GetWindowTextW(hOutDir, out_buf, MAX_PATH_LEN);
            if (out_buf[0]) CreateDirectoryW(out_buf, NULL);

            is_decrypting = TRUE;
            EnableWindow(hBtnDecrypt, FALSE);
            /* 解密中禁用所有列表修改操作 */
            EnableWindow(hBtnAddFile, FALSE);
            EnableWindow(hBtnAddDir,  FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR), FALSE);
            EnableWindow(g_hwndListBox, FALSE);
            SendMessageW(hProg, PBM_SETPOS, 0, 0);
            SetWindowTextW(hLog, L"");

            wchar_t mode_buf[512];
            _snwprintf(mode_buf, 511, L"模式：%s  兜底：%s",
                       auto_mode ? L"自动" : proc_buf,
                       fallback_buf);
            mode_buf[511] = 0;
            int mlen = GetWindowTextLengthW(hLog);
            SendMessageW(hLog, EM_SETSEL, mlen, mlen);
            SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)mode_buf);
            SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");

            HANDLE ht = start_decrypt_thread(hwnd, g_paths, g_path_cnt,
                                 auto_mode ? NULL : proc_buf,  /* override */
                                 fallback_buf,                  /* fallback */
                                 out_buf[0] ? out_buf : NULL);
            if (ht) CloseHandle(ht);
            else {
                /* 启动失败：恢复按钮状态 */
                is_decrypting = FALSE;
                EnableWindow(hBtnDecrypt, TRUE);
                EnableWindow(hBtnAddFile, TRUE);
                EnableWindow(hBtnAddDir,  TRUE);
                EnableWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR), TRUE);
                EnableWindow(g_hwndListBox, TRUE);
                MessageBoxW(hwnd, L"启动解密线程失败（内存不足）", L"错误", MB_OK|MB_ICONERROR);
            }
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
        /* 恢复列表操作按钮 */
        EnableWindow(hBtnAddFile, TRUE);
        EnableWindow(hBtnAddDir,  TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR), TRUE);
        EnableWindow(g_hwndListBox, TRUE);
        return 0;

    case WM_CLOSE:
        if (is_decrypting) {
            int r = MessageBoxW(hwnd, L"解密正在进行中，确定要退出吗？\n退出后当前批次将中断。",
                                L"确认退出", MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);
            if (r != IDYES) return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ── 扩展名映射管理对话框 ─────────────────────────────────────────────── */

#define EXTDLG_W 440
#define EXTDLG_H 320
#define IDC_EXTDLG_LIST   201
#define IDC_EXTDLG_EXT    202
#define IDC_EXTDLG_PROC   203
#define IDC_EXTDLG_ADD    204
#define IDC_EXTDLG_DEL    205
#define IDC_EXTDLG_OK     206

/* 刷新 ListView 内容（从 g_ext_map 读取） */
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

static LRESULT CALLBACK ExtMapDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(NULL);
        HFONT hF = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int W = EXTDLG_W, H = EXTDLG_H;
        /* ListView：占上方大部分空间，底部留 110px 给标签+输入+按钮 */
        HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
            8, 8, W-16, H-110, hwnd, (HMENU)IDC_EXTDLG_LIST, hInst, NULL);
        SendMessageW(hList, WM_SETFONT, (WPARAM)hF, FALSE);
        /* 列头 */
        LVCOLUMNW col = {0};
        col.mask = LVCF_TEXT|LVCF_WIDTH;
        col.cx   = 120; col.pszText = L"扩展名";
        ListView_InsertColumn(hList, 0, &col);
        col.cx   = W-16-120-4; col.pszText = L"进程名";
        ListView_InsertColumn(hList, 1, &col);
        extdlg_refresh_list(hList);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT);
        /* 标签行（ListView 下方 6px）*/
        CreateWindowW(L"STATIC", L"扩展名（如 .xlsx）",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            8, H-100, 140, 18, hwnd, NULL, hInst, NULL);
        CreateWindowW(L"STATIC", L"进程名（如 EXCEL.EXE）",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            154, H-100, 200, 18, hwnd, NULL, hInst, NULL);
        /* 输入行（标签下方 2px）*/
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            8, H-78, 140, 22, hwnd, (HMENU)IDC_EXTDLG_EXT, hInst, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            154, H-78, W-154-86-8, 22, hwnd, (HMENU)IDC_EXTDLG_PROC, hInst, NULL);
        CreateWindowW(L"BUTTON", L"添加",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            W-82, H-78, 74, 22, hwnd, (HMENU)IDC_EXTDLG_ADD, hInst, NULL);
        /* 底部按钮行 */
        CreateWindowW(L"BUTTON", L"删除选中",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            8, H-48, 90, 26, hwnd, (HMENU)IDC_EXTDLG_DEL, hInst, NULL);
        CreateWindowW(L"BUTTON", L"确定",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
            W-90, H-48, 82, 26, hwnd, (HMENU)IDC_EXTDLG_OK, hInst, NULL);
        /* 字体 */
        HWND hc = GetWindow(hwnd, GW_CHILD);
        while (hc) { SendMessageW(hc, WM_SETFONT, (WPARAM)hF, FALSE); hc = GetWindow(hc, GW_HWNDNEXT); }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_EXTDLG_ADD) {
            wchar_t ext[32]={0}, proc[MAX_PATH_LEN]={0};
            GetWindowTextW(GetDlgItem(hwnd, IDC_EXTDLG_EXT),  ext,  32);
            GetWindowTextW(GetDlgItem(hwnd, IDC_EXTDLG_PROC), proc, MAX_PATH_LEN);
            if (!ext[0] || !proc[0]) break;
            /* 确保以 . 开头，转小写 */
            wchar_t ext_norm[32]={0};
            if (ext[0] != L'.') { ext_norm[0]=L'.'; wcsncpy(ext_norm+1, ext, 30); }
            else wcsncpy(ext_norm, ext, 31);
            wcs_lower(ext_norm);
            /* 查重：已存在则更新 */
            BOOL updated = FALSE;
            for (int i=0; i<g_ext_map_cnt; i++) {
                if (wcscmp(g_ext_map[i].ext, ext_norm)==0) {
                    wcsncpy(g_ext_map[i].proc, proc, MAX_PATH_LEN-1);
                    updated = TRUE; break;
                }
            }
            if (!updated && g_ext_map_cnt < MAX_EXT_MAP) {
                wcsncpy(g_ext_map[g_ext_map_cnt].ext,  ext_norm, 31);
                wcsncpy(g_ext_map[g_ext_map_cnt].proc, proc, MAX_PATH_LEN-1);
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
            /* 从数组中删除，后面的前移 */
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
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN) DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        /* 非模态对话框销毁，不 PostQuitMessage，只返回 */
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_ext_map_dialog(HWND parent) {
    HINSTANCE hInst = GetModuleHandleW(NULL);
    /* 注册窗口类（只注册一次） */
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
    /* 居中于父窗口 */
    RECT pr; GetWindowRect(parent, &pr);
    RECT dr = {0,0,EXTDLG_W,EXTDLG_H};
    AdjustWindowRect(&dr, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE);
    int dw = dr.right-dr.left, dh = dr.bottom-dr.top;
    int dx = pr.left + (pr.right-pr.left-dw)/2;
    int dy = pr.top  + (pr.bottom-pr.top-dh)/2;
    HWND hdlg = CreateWindowExW(0, L"YST_ExtMapDlg", L"扩展名→进程名 映射管理",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        dx, dy, dw, dh, parent, NULL, hInst, NULL);
    ShowWindow(hdlg, SW_SHOW);
    UpdateWindow(hdlg);
    /* 禁用父窗口，实现真正模态（防止用户操作主窗口时竞争 g_ext_map） */
    EnableWindow(parent, FALSE);
    /* 模态消息循环：直到对话框窗口被销毁 */
    MSG msg;
    while (IsWindow(hdlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)) {
            DestroyWindow(hdlg);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    /* 恢复父窗口并将焦点还给它 */
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
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
