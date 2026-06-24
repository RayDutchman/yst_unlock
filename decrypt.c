/*
 * decrypt.c — 解密业务层
 *
 * 包含：工具函数、INI持久化、注册表查询、右键菜单、
 *        文件收集、Worker IPC、解密线程
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

#include "decrypt.h"

/* ── 亿赛通加密文件魔数 ────────────────────────────────── */
/* 字节 1-3 处有固定魔数 14 23 65，所有加密文件均保留此特征。
 * 字节 0 为文件类型码（0x62=PNG/其他, 0x63=PDF）。
 * 偏移 12 处本应有 "E-SafeNet" 供应商标记（9字节），但在部分
 * 文件中该标记可能被加密数据覆写（前 4-5 字节 "E-Saf" 尚存），
 * 因此魔数是比 E-SafeNet 更可靠的判定依据。 */

/* ── 跳过规则静态数据 ────────────────────────────────────── */

static const wchar_t *SKIP_DIRS[] = {
    L".git", L".svn", L".hg", L".idea", L".vscode",
    L"__pycache__", L"node_modules", L".cache",
    L"$recycle.bin", L"system volume information",
    L"windows", L"program files", L"program files (x86)",
    NULL
};

static const wchar_t *SKIP_EXTS[] = {
    L".lnk", L".tmp", L".temp", L".yst_tmp",
    L".db", L".ds_store", L".ini", L".log",
    NULL
};

static const wchar_t *REG_ROOTS[] = {
    L"Software\\Classes\\*\\shell",
    L"Software\\Classes\\Directory\\shell",
    L"Software\\Classes\\Directory\\Background\\shell",
    NULL
};

/* ── 全局变量定义 ───────────────────────────────────────── */

ExtMapEntry g_ext_map[MAX_EXT_MAP];
int         g_ext_map_cnt = 0;
wchar_t     g_fallback_proc[MAX_PATH_LEN] = L"POWERPNT.EXE";
wchar_t     g_ini_path[MAX_PATH_LEN];

/* ── 工具函数 ────────────────────────────────────────────── */

void get_self_path(wchar_t *buf, int len) {
    GetModuleFileNameW(NULL, buf, len);
}

void wcs_lower(wchar_t *s) {
    for (; *s; s++) *s = (wchar_t)towlower(*s);
}

void preserve_file_time(const wchar_t *path,
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

void init_ini_path(void) {
    wchar_t exe[MAX_PATH_LEN];
    get_self_path(exe, MAX_PATH_LEN);
    wchar_t *bs = wcsrchr(exe, L'\\');
    if (bs) { *(bs+1) = 0; } else { exe[0] = 0; }
    _snwprintf(g_ini_path, MAX_PATH_LEN-1, L"%syst_unlock.ini", exe);
    g_ini_path[MAX_PATH_LEN-1] = 0;
}

void load_ini(void) {
    wchar_t buf[MAX_PATH_LEN];
    if (GetPrivateProfileStringW(L"Config", L"FallbackProc", L"POWERPNT.EXE",
                                 buf, MAX_PATH_LEN, g_ini_path) > 0) {
        wcsncpy(g_fallback_proc, buf, MAX_PATH_LEN-1);
        g_fallback_proc[MAX_PATH_LEN-1] = 0;
    }

    wchar_t *keys = (wchar_t*)calloc(8192, sizeof(wchar_t));
    if (!keys) return;
    DWORD ret = GetPrivateProfileSectionW(L"ExtMap", keys, 8192, g_ini_path);
    g_ext_map_cnt = 0;
    if (ret > 0) {
        wchar_t *p = keys;
        while (*p && g_ext_map_cnt < MAX_EXT_MAP) {
            wchar_t *eq = wcschr(p, L'=');
            if (eq) {
                int elen = (int)(eq - p);
                if (elen > 0 && elen < 32) {
                    wcsncpy(g_ext_map[g_ext_map_cnt].ext, p, elen);
                    g_ext_map[g_ext_map_cnt].ext[elen] = 0;
                    wcs_lower(g_ext_map[g_ext_map_cnt].ext);
                    wcsncpy(g_ext_map[g_ext_map_cnt].proc, eq+1, MAX_PATH_LEN-1);
                    g_ext_map[g_ext_map_cnt].proc[MAX_PATH_LEN-1] = 0;
                    g_ext_map_cnt++;
                }
            }
            p += wcslen(p) + 1;
        }
    }
    free(keys);
}

void save_ini(void) {
    WritePrivateProfileStringW(L"Config", L"FallbackProc",
                               g_fallback_proc, g_ini_path);
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
    wchar_t tmp[MAX_PATH_LEN];
    wcsncpy(tmp, cmd, MAX_PATH_LEN - 1);
    tmp[MAX_PATH_LEN - 1] = 0;
    wchar_t *p = tmp;
    wchar_t *end;
    if (*p == L'"') {
        p++;
        end = wcschr(p, L'"');
        if (end) *end = 0;
    } else {
        end = wcschr(p, L' ');
        if (end) *end = 0;
    }
    wchar_t *base = wcsrchr(p, L'\\');
    if (!base) base = wcsrchr(p, L'/');
    const wchar_t *name = base ? base + 1 : p;
    wcsncpy(out, name, outlen - 1);
    out[outlen - 1] = 0;
}

static BOOL get_default_process_for_ext(const wchar_t *ext, wchar_t *out, int outlen) {
    wchar_t *prog_id = (wchar_t*)malloc(512 * sizeof(wchar_t));
    wchar_t *uc_path = (wchar_t*)malloc(512 * sizeof(wchar_t));
    wchar_t *cmd_path= (wchar_t*)malloc(1024 * sizeof(wchar_t));
    wchar_t *cmd     = (wchar_t*)malloc(MAX_PATH_LEN * sizeof(wchar_t));
    if (!prog_id || !uc_path || !cmd_path || !cmd) {
        free(prog_id); free(uc_path); free(cmd_path); free(cmd);
        return FALSE;
    }
    prog_id[0] = 0; uc_path[0] = 0; cmd_path[0] = 0; cmd[0] = 0;
    BOOL result = FALSE;

    _snwprintf(uc_path, 511,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
        ext);
    uc_path[511] = 0;
    if (!reg_query_str(HKEY_CURRENT_USER, uc_path, L"ProgId", prog_id, 512)) {
        if (!reg_query_str(HKEY_CLASSES_ROOT, ext, L"", prog_id, 512))
            goto done;
    }
    if (!prog_id[0]) goto done;

    {
        static const wchar_t *verbs[] = {L"open", L"Open", L"edit", NULL};
        for (int i = 0; verbs[i]; i++) {
            _snwprintf(cmd_path, 1023, L"%s\\shell\\%s\\command", prog_id, verbs[i]);
            cmd_path[1023] = 0;
            if (!reg_query_str(HKEY_CLASSES_ROOT, cmd_path, L"", cmd, MAX_PATH_LEN))
                continue;
            extract_exe_name(cmd, out, outlen);
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

/* 返回伪装进程名：override > 映射表(在decrypt_thread中处理) > 注册表 > 空（兜底由调用方填）*/
static BOOL resolve_process_name(const wchar_t *filepath,
                                 const wchar_t *override_name,
                                 wchar_t *out, int outlen) {
    if (override_name && override_name[0]) {
        wcsncpy(out, override_name, outlen - 1);
        out[outlen - 1] = 0;
        return TRUE;
    }
    const wchar_t *dot = wcsrchr(filepath, L'.');
    if (dot && get_default_process_for_ext(dot, out, outlen))
        return TRUE;
    out[0] = 0;
    return FALSE;
}

/* ── 右键菜单注册表管理 ──────────────────────────────── */

void install_context_menu(wchar_t *msg_out, int msg_len) {
    wchar_t self_exe[MAX_PATH_LEN];
    get_self_path(self_exe, MAX_PATH_LEN);
    int errors = 0;
    for (int i = 0; REG_ROOTS[i]; i++) {
        wchar_t shell_path[1024], cmd_path[1024], cmd[MAX_PATH_LEN];
        _snwprintf(shell_path, 1023, L"%s\\%s", REG_ROOTS[i], MENU_LABEL);
        shell_path[1023] = 0;
        _snwprintf(cmd_path, 1023, L"%s\\command", shell_path);
        cmd_path[1023] = 0;
        const wchar_t *ph = wcsstr(REG_ROOTS[i], L"Background") ? L"%V" : L"%1";
        _snwprintf(cmd, MAX_PATH_LEN - 1, L"\"%s\" --silent \"%s\"", self_exe, ph);
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

void uninstall_context_menu(wchar_t *msg_out, int msg_len) {
    int errors = 0;
    for (int i = 0; REG_ROOTS[i]; i++) {
        wchar_t shell_path[1024], cmd_path[1024];
        _snwprintf(shell_path, 1023, L"%s\\%s", REG_ROOTS[i], MENU_LABEL);
        shell_path[1023] = 0;
        _snwprintf(cmd_path, 1023, L"%s\\command", shell_path);
        cmd_path[1023] = 0;
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
    lower[MAX_PATH_LEN - 1] = 0;
    wcs_lower(lower);
    for (int i = 0; SKIP_DIRS[i]; i++)
        if (wcscmp(lower, SKIP_DIRS[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL should_skip_file(const wchar_t *path) {
    const wchar_t *base = wcsrchr(path, L'\\');
    if (!base) base = path; else base++;
    if (base[0] == L'.' ) return TRUE;
    if (base[0] == L'~' && base[1] == L'$') return TRUE;
    const wchar_t *dot = wcsrchr(base, L'.');
    if (!dot) return FALSE;
    wchar_t lower[64];
    wcsncpy(lower, dot, 63); lower[63] = 0;
    wcs_lower(lower);
    for (int i = 0; SKIP_EXTS[i]; i++)
        if (wcscmp(lower, SKIP_EXTS[i]) == 0) return TRUE;
    return FALSE;
}

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
        wchar_t **tmp = (wchar_t**)realloc(fl->items, fl->capacity * sizeof(wchar_t*));
        if (!tmp) return;
        fl->items = tmp;
    }
    wchar_t *dup = _wcsdup(path);
    if (!dup) return;
    fl->items[fl->count++] = dup;
}

static void filelist_free(FileList *fl) {
    for (int i = 0; i < fl->count; i++) free(fl->items[i]);
    free(fl->items);
    fl->items = NULL; fl->count = fl->capacity = 0;
}

static void walk_dir(const wchar_t *dir_root, FileList *fl) {
    int   cap  = 256;
    int   head = 0, tail = 0;
    wchar_t **queue = (wchar_t**)malloc(cap * sizeof(wchar_t*));
    if (!queue) return;
    {
        wchar_t *dir_copy = _wcsdup(dir_root);
        if (!dir_copy) { free(queue); return; }
        queue[tail++] = dir_copy;
    }

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
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                wchar_t full[MAX_PATH_LEN];
                _snwprintf(full, MAX_PATH_LEN - 1, L"%s\\%s", cur, fd.cFileName);
                full[MAX_PATH_LEN - 1] = 0;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!should_skip_dir(fd.cFileName)) {
                        if (tail >= cap) {
                            cap *= 2;
                            wchar_t **tmp = (wchar_t**)realloc(queue, cap * sizeof(wchar_t*));
                            if (!tmp) {
                                FindClose(hf);
                                free(cur);
                                goto walk_cleanup;
                            }
                            queue = tmp;
                        }
                        wchar_t *dir_copy2 = _wcsdup(full);
                        if (dir_copy2)
                            queue[tail++] = dir_copy2;
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
    for (int i = head; i < tail; i++) free(queue[i]);
    free(queue);
}

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

/* ── 亿赛通加密文件检测 ──────────────────────────────────── */
/* 读取前 24 字节，检查字节 1-3 处是否存在亿赛通固定魔数
 * 14 23 65。该魔数在所有加密文件中一致保留，而偏移 12 处的
 * "E-SafeNet" 供应商标记在部分文件可能被加密数据部分覆写，
 * 因此用魔数比用 E-SafeNet 更可靠。
 */
BOOL is_file_encrypted(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    BYTE buf[24];
    DWORD rd;
    BOOL encrypted = FALSE;

    if (ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd == sizeof(buf)) {
        if (memcmp(buf + 1, "\x14\x23\x65", 3) == 0) {
            encrypted = TRUE;
        }
    }

    CloseHandle(h);
    return encrypted;
}

/* ── Worker 子进程模式 ───────────────────────────────────────── */

#define COPY_BUF_SIZE (4 * 1024 * 1024)

void worker_mode(void) {
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    char line[MAX_PATH_LEN * 8];
    while (fgets(line, sizeof(line), stdin)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = 0;
        if (!len) continue;

        char *tab = strchr(line, '\t');
        if (!tab) { fputs("ERR:no tab\n", stdout); fflush(stdout); continue; }
        *tab = 0;
        const char *src_u8 = line;
        const char *dst_u8 = tab + 1;

        wchar_t src_w[MAX_PATH_LEN], dst_w[MAX_PATH_LEN];
        if (!MultiByteToWideChar(CP_UTF8, 0, src_u8, -1, src_w, MAX_PATH_LEN) ||
            !MultiByteToWideChar(CP_UTF8, 0, dst_u8, -1, dst_w, MAX_PATH_LEN)) {
            fputs("ERR:utf8 decode failed\n", stdout); fflush(stdout); continue;
        }

        HANDLE hSrc = CreateFileW(src_w, GENERIC_READ,
                                  FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hSrc == INVALID_HANDLE_VALUE) {
            fprintf(stdout, "ERR:open src failed (%lu)\n", GetLastError());
            fflush(stdout); continue;
        }

        LARGE_INTEGER fsz; fsz.QuadPart = 0;
        GetFileSizeEx(hSrc, &fsz);

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
            if (rd == 0) break;
            fbuf_len += rd;
        }
        CloseHandle(hSrc);
        if (!read_ok) continue;

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
        if (!write_ok) { DeleteFileW(dst_w); free(fbuf); continue; }

        free(fbuf);
        fputs("OK\n", stdout);
        fflush(stdout);
    }
}

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

/* ── 核心解密逻辑 ─────────────────────────────────────────── */

typedef struct {
    HANDLE hProcess;
    HANDLE hStdin_W;
    HANDLE hStdout_R;
} WorkerProc;

static BOOL start_worker(const wchar_t *fake_exe, WorkerProc *wp) {
    HANDLE r_in=NULL, w_in=NULL, r_out=NULL, w_out=NULL;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&r_in,  &w_in,  &sa, 0)) return FALSE;
    if (!CreatePipe(&r_out, &w_out, &sa, 0)) { CloseHandle(r_in); CloseHandle(w_in); return FALSE; }
    SetHandleInformation(w_in,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(r_out, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = r_in;
    si.hStdOutput  = w_out;
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

static BOOL send_task(WorkerProc *wp,
                      const wchar_t *src, const wchar_t *dst,
                      char *err_out, int err_len) {
    int su = MAX_PATH_LEN * 4, du = MAX_PATH_LEN * 4;
    char *src_u8 = (char*)malloc(su);
    char *dst_u8 = (char*)malloc(du);
    if (!src_u8 || !dst_u8) {
        free(src_u8); free(dst_u8);
        strncpy(err_out, "out of memory", err_len-1); return FALSE;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, src, -1, src_u8, su, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, dst, -1, dst_u8, du, NULL, NULL)) {
        free(src_u8); free(dst_u8);
        strncpy(err_out, "utf8 encode failed", err_len-1); return FALSE;
    }

    int lu = su + du + 4;
    char *line = (char*)malloc(lu);
    if (!line) {
        free(src_u8); free(dst_u8);
        strncpy(err_out, "out of memory", err_len-1); return FALSE;
    }
    int n = snprintf(line, lu, "%s\t%s\n", src_u8, dst_u8);
    free(src_u8); free(dst_u8);
    if (n <= 0) { free(line); strncpy(err_out, "format line failed", err_len-1); return FALSE; }
    DWORD written;
    BOOL pipe_ok = WriteFile(wp->hStdin_W, line, (DWORD)n, &written, NULL);
    free(line);
    if (!pipe_ok) { strncpy(err_out, "write to worker pipe failed", err_len-1); return FALSE; }

    /* 逐字节读响应直到 '\n'，带超时保护（30秒），同时检测 worker 进程存活 */
    char resp[512] = {0};
    int ri = 0;
    ULONGLONG deadline = GetTickCount64() + 30000;
    while (ri < (int)sizeof(resp)-1) {
        /* 先检查管道里是否有数据可读，避免 ReadFile 永久阻塞 */
        DWORD avail = 0;
        if (!PeekNamedPipe(wp->hStdout_R, NULL, 0, NULL, &avail, NULL)) {
            strncpy(err_out, "worker pipe broken", err_len-1); return FALSE;
        }
        if (avail == 0) {
            /* 无数据：检查超时 */
            if (GetTickCount64() >= deadline) {
                strncpy(err_out, "worker timeout (30s)", err_len-1); return FALSE;
            }
            /* 检查 worker 进程是否已死 */
            if (wp->hProcess &&
                WaitForSingleObject(wp->hProcess, 0) != WAIT_TIMEOUT) {
                strncpy(err_out, "worker process died", err_len-1); return FALSE;
            }
            Sleep(10);
            continue;
        }
        char ch; DWORD rd;
        if (!ReadFile(wp->hStdout_R, &ch, 1, &rd, NULL) || rd == 0) {
            strncpy(err_out, "worker pipe closed unexpectedly", err_len-1); return FALSE;
        }
        if (ch == '\n') break;
        resp[ri++] = ch;
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

/* ── 解密线程 ────────────────────────────────────────────── */

typedef struct {
    wchar_t **paths;
    int       path_cnt;
    wchar_t   proc_override[MAX_PATH_LEN];
    wchar_t   fallback_proc[MAX_PATH_LEN];
    const ExtMapEntry *ext_map;
    int                ext_map_cnt;
    HWND      notify_hwnd;
    HANDLE    hStopEvent;   /* 置信号时通知线程中断 */
} DecryptArgs;

#define NOTIFY_LOG(hwnd, s)   do { wchar_t *_s = _wcsdup(s); if (!_s || !PostMessageW((hwnd), WM_WORKER_LOG, 0, (LPARAM)_s)) free(_s); } while(0)
/* NOTIFY_DONE：队列满时重试，确保完成通知一定送达 */
#define NOTIFY_DONE(hwnd, ok) do { \
    while (!PostMessageW((hwnd), WM_WORKER_DONE, (WPARAM)(ok), 0)) Sleep(10); \
} while(0)

/* 只在进度值变化时才发消息，避免大量重复值塞满消息队列 */
static void notify_prog(HWND hwnd, int v) {
    static int last = -1;
    if (v != last) { PostMessageW(hwnd, WM_WORKER_PROG, (WPARAM)v, 0); last = v; }
}
#define NOTIFY_PROG(hwnd, v) notify_prog((hwnd), (int)(v))

#define MAX_WORKERS 16
typedef struct { wchar_t name[MAX_PATH_LEN]; WorkerProc wp; } WorkerEntry;

#define MAX_FALLBACK_EXTS 64

static DWORD WINAPI decrypt_thread(LPVOID param) {
    DecryptArgs *args = (DecryptArgs*)param;
    HWND hwnd = args->notify_hwnd;

    FileList fl;
    filelist_init(&fl);
    collect_files(args->paths, args->path_cnt, &fl);

    if (fl.count == 0) {
        NOTIFY_LOG(hwnd, L"[警告] 未找到任何文件");
        NOTIFY_DONE(hwnd, FALSE);
        goto cleanup_args;
    }

    int total = fl.count;
    {
        wchar_t buf[128];
        _snwprintf(buf, 127, L"共找到 %d 个文件，开始解密...", total);
        buf[127] = 0;
        NOTIFY_LOG(hwnd, buf);
    }

    wchar_t self_exe[MAX_PATH_LEN];
    get_self_path(self_exe, MAX_PATH_LEN);

    wchar_t tmp_dir[MAX_PATH_LEN];
    GetTempPathW(MAX_PATH_LEN, tmp_dir);
    {
        wchar_t sub[64];
        _snwprintf(sub, 63, L"yst_unlock_%lu\\", GetCurrentProcessId());
        sub[63] = 0;
        wcscat(tmp_dir, sub);
    }
    if (!CreateDirectoryW(tmp_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        /* 临时目录创建失败，仍可继续（后续每个文件会失败并报错） */
    }

    WorkerEntry workers[MAX_WORKERS];
    int worker_cnt = 0;
    int success_count = 0, fail_count = 0, skip_count = 0;

    wchar_t fallback_exts[MAX_FALLBACK_EXTS][32];
    int fallback_ext_cnt = 0;
    wchar_t processed_exts[MAX_FALLBACK_EXTS][32];
    int processed_ext_cnt = 0;

    for (int idx = 0; idx < fl.count; idx++) {
        const wchar_t *src = fl.items[idx];

        /* 检查停止信号 */
        if (args->hStopEvent &&
            WaitForSingleObject(args->hStopEvent, 0) == WAIT_OBJECT_0) {
            NOTIFY_LOG(hwnd, L"[提示] 用户中断解密");
            break;
        }

        /* 跳过未加密的文件
         * is_file_encrypted() 在父进程（非白名单）中用 CreateFileW
         * 读取文件头，检查偏移 12 处是否存在 "E-SafeNet" 供应商标记。
         * 在安装了亿赛通的机器上实测验证：驱动对非白名单进程读文件
         * 不会透明解密，读到的仍是加密头，E-SafeNet 检测可靠。 */
        if (!is_file_encrypted(src)) {
            skip_count++;
            NOTIFY_PROG(hwnd, (idx+1)*100/fl.count);
            continue;
        }

        /* 收集加密文件扩展名 */
        {
            const wchar_t *dot = wcsrchr(src, L'.');
            if (dot && processed_ext_cnt < MAX_FALLBACK_EXTS) {
                wchar_t ext_lower[32];
                wcsncpy(ext_lower, dot, 31); ext_lower[31] = 0;
                wcs_lower(ext_lower);
                BOOL found = FALSE;
                for (int ei = 0; ei < processed_ext_cnt; ei++)
                    if (wcscmp(processed_exts[ei], ext_lower) == 0) { found = TRUE; break; }
                if (!found) {
                    wcsncpy(processed_exts[processed_ext_cnt], ext_lower, 31);
                    processed_exts[processed_ext_cnt][31] = 0;
                    processed_ext_cnt++;
                }
            }
        }

        /* 确定进程名：映射表 > 注册表 > 兜底 */
        wchar_t proc_name[MAX_PATH_LEN];
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
                        proc_name[MAX_PATH_LEN-1] = 0;
                        found_in_map = TRUE;
                        break;
                    }
                }
            }
        }
        if (!found_in_map &&
            !resolve_process_name(src, args->proc_override, proc_name, MAX_PATH_LEN)) {
            const wchar_t *dot = wcsrchr(src, L'.');
            if (dot && fallback_ext_cnt < MAX_FALLBACK_EXTS) {
                wchar_t ext_lower[32];
                wcsncpy(ext_lower, dot, 31); ext_lower[31] = 0;
                wcs_lower(ext_lower);
                BOOL found = FALSE;
                for (int ei = 0; ei < fallback_ext_cnt; ei++)
                    if (wcscmp(fallback_exts[ei], ext_lower) == 0) { found = TRUE; break; }
                if (!found) {
                    wcsncpy(fallback_exts[fallback_ext_cnt], ext_lower, 31);
                    fallback_exts[fallback_ext_cnt][31] = 0;
                    fallback_ext_cnt++;
                }
            }
            wcsncpy(proc_name, args->fallback_proc, MAX_PATH_LEN-1);
            proc_name[MAX_PATH_LEN-1] = 0;
        }

        /* 查找或创建 worker */
        WorkerEntry *we = NULL;
        for (int w = 0; w < worker_cnt; w++)
            if (wcscmp(workers[w].name, proc_name) == 0) { we = &workers[w]; break; }
        if (!we) {
            if (worker_cnt < MAX_WORKERS) {
                we = &workers[worker_cnt++];
                wcsncpy(we->name, proc_name, MAX_PATH_LEN-1);
                we->name[MAX_PATH_LEN-1] = 0;
                memset(&we->wp, 0, sizeof(we->wp));
            }
        }
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

        /* 确定临时文件路径（始终原位解密：写 .yst_tmp → 替换原文件） */
        wchar_t dst[MAX_PATH_LEN];
        _snwprintf(dst, MAX_PATH_LEN-1, L"%s.yst_tmp", src);
        dst[MAX_PATH_LEN-1] = 0;

        /* 发送任务 */
        char err[512] = {0};
        BOOL ok = we ? send_task(&we->wp, src, dst, err, 512) : FALSE;

        if (ok) {
            /* 保留原文件时间戳 */
            HANDLE hTimeSrc = CreateFileW(src, FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS, NULL);
            FILETIME ft_create, ft_access, ft_write;
            BOOL got_time = (hTimeSrc != INVALID_HANDLE_VALUE) &&
                            GetFileTime(hTimeSrc, &ft_create, &ft_access, &ft_write);
            if (hTimeSrc != INVALID_HANDLE_VALUE) CloseHandle(hTimeSrc);

            /* 只读文件无法直接替换，先清除只读属性 */
            SetFileAttributesW(src, FILE_ATTRIBUTE_NORMAL);
            if (!MoveFileExW(dst, src, MOVEFILE_REPLACE_EXISTING)) {
                DWORD e = GetLastError();
                snprintf(err, 512, "替换原文件失败 (%lu)", e);
                ok = FALSE;
                DeleteFileW(dst);
            } else {
                if (got_time)
                    preserve_file_time(src, &ft_create, &ft_access, &ft_write);
                /* MoveFileExW 后驱动可能重新加密，再次检查确认 */
                if (is_file_encrypted(src))
                    ok = FALSE;
            }
        } else {
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
            _snwprintf(buf, 1023, L"[%d/%d] 失败 [%s]: %s",
                       idx+1, total, proc_name, fname);
            buf[1023] = 0;
        }
        NOTIFY_LOG(hwnd, buf);
        NOTIFY_PROG(hwnd, (idx+1)*100/total);
    }

    /* 关闭所有 worker，清理临时目录 */
    for (int w = 0; w < worker_cnt; w++) stop_worker(&workers[w].wp);
    rmdir_recursive(tmp_dir);
    {
        wchar_t buf[256];
        _snwprintf(buf, 255, L"\n完成：成功 %d 个，失败 %d 个，跳过 %d 个（未加密）",
                   success_count, fail_count, skip_count);
        buf[255] = 0;
        NOTIFY_LOG(hwnd, buf);
    }
    if (fallback_ext_cnt > 0) {
        wchar_t hint[128];
        _snwprintf(hint, 127, L"[提示] 以下扩展名未找到注册表默认进程，已用 %s 兜底解密：",
                   args->fallback_proc);
        hint[127] = 0;
        NOTIFY_LOG(hwnd, hint);
        wchar_t extline[MAX_FALLBACK_EXTS * 34];
        extline[0] = L' '; extline[1] = L' '; extline[2] = 0;
        for (int ei = 0; ei < fallback_ext_cnt; ei++) {
            if (ei > 0) wcsncat(extline, L"  ", sizeof(extline)/sizeof(wchar_t) - wcslen(extline) - 1);
            wcsncat(extline, fallback_exts[ei], sizeof(extline)/sizeof(wchar_t) - wcslen(extline) - 1);
        }
        NOTIFY_LOG(hwnd, extline);
    }
    if (processed_ext_cnt > 0) {
        wchar_t hint[128];
        _snwprintf(hint, 127, L"[提示] 以下扩展名的文件检测到加密状态，已处理：");
        hint[127] = 0;
        NOTIFY_LOG(hwnd, hint);
        wchar_t extline[MAX_FALLBACK_EXTS * 34];
        extline[0] = L' '; extline[1] = L' '; extline[2] = 0;
        for (int ei = 0; ei < processed_ext_cnt; ei++) {
            if (ei > 0) wcsncat(extline, L"  ", sizeof(extline)/sizeof(wchar_t) - wcslen(extline) - 1);
            wcsncat(extline, processed_exts[ei], sizeof(extline)/sizeof(wchar_t) - wcslen(extline) - 1);
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

HANDLE start_decrypt_thread(HWND notify_hwnd,
                             wchar_t **paths, int path_cnt,
                             const wchar_t *proc_override,
                             const wchar_t *fallback_proc,
                             HANDLE *out_stop_event) {
    DecryptArgs *args = (DecryptArgs*)calloc(1, sizeof(DecryptArgs));
    if (!args) return NULL;
    args->notify_hwnd = notify_hwnd;
    args->path_cnt    = path_cnt;
    args->paths       = (wchar_t**)malloc(path_cnt * sizeof(wchar_t*));
    if (!args->paths) { free(args); return NULL; }
    for (int i = 0; i < path_cnt; i++) {
        args->paths[i] = _wcsdup(paths[i]);
        if (!args->paths[i]) {
            for (int j = 0; j < i; j++) free(args->paths[j]);
            free(args->paths); free(args);
            return NULL;
        }
    }
    if (proc_override)
        wcsncpy(args->proc_override, proc_override, MAX_PATH_LEN-1);
    if (fallback_proc && fallback_proc[0])
        wcsncpy(args->fallback_proc, fallback_proc, MAX_PATH_LEN-1);
    else
        wcsncpy(args->fallback_proc, g_fallback_proc, MAX_PATH_LEN-1);
    args->ext_map     = g_ext_map;
    args->ext_map_cnt = g_ext_map_cnt;
    if (out_stop_event) {
        args->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        *out_stop_event = args->hStopEvent;
    }
    HANDLE ht = CreateThread(NULL, 0, decrypt_thread, args, 0, NULL);
    if (!ht) {
        if (out_stop_event && args->hStopEvent) {
            CloseHandle(args->hStopEvent);
            *out_stop_event = NULL;
        }
        for (int i = 0; i < path_cnt; i++) free(args->paths[i]);
        free(args->paths);
        free(args);
        return NULL;
    }
    return ht;
}
