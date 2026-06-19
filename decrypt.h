/*
 * decrypt.h — 解密业务层公开接口
 *
 * 包含：INI持久化、注册表查询、右键菜单、文件收集、
 *        Worker IPC、解密线程、时间戳保留
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ── 共享常量（UI层也需要） ──────────────────────────────── */
#define APP_TITLE       L"YST unlock"
#define MENU_LABEL      L"YST unlock"
#define MAX_PATHS       4096
#define MAX_PATH_LEN    4096

/* 线程→窗口通知消息 */
#define WM_WORKER_LOG   (WM_USER + 1)
#define WM_WORKER_PROG  (WM_USER + 2)
#define WM_WORKER_DONE  (WM_USER + 3)

/* ── 扩展名映射表 ──────────────────────────────────────── */
#define MAX_EXT_MAP 64
typedef struct { wchar_t ext[32]; wchar_t proc[MAX_PATH_LEN]; } ExtMapEntry;

/* 全局：映射表、兜底进程名、INI路径（decrypt.c 定义，其他模块引用） */
extern ExtMapEntry g_ext_map[MAX_EXT_MAP];
extern int         g_ext_map_cnt;
extern wchar_t     g_fallback_proc[MAX_PATH_LEN];
extern wchar_t     g_ini_path[MAX_PATH_LEN];

/* ── 工具函数 ──────────────────────────────────────────── */
void get_self_path(wchar_t *buf, int len);
void wcs_lower(wchar_t *s);
void preserve_file_time(const wchar_t *path,
                        const FILETIME *create,
                        const FILETIME *access,
                        const FILETIME *write);

/* ── INI 持久化 ────────────────────────────────────────── */
void init_ini_path(void);
void load_ini(void);
void save_ini(void);

/* ── 右键菜单注册表管理 ────────────────────────────────── */
void install_context_menu(wchar_t *msg_out, int msg_len);
void uninstall_context_menu(wchar_t *msg_out, int msg_len);

/* ── 解密线程入口（异步，返回线程句柄；调用方 CloseHandle）
 *   返回 NULL 表示启动失败（内存不足/CreateThread失败）。
 *   线程结束时向 notify_hwnd 发：
 *     WM_WORKER_LOG  lp=_wcsdup(str)（接收方负责 free）
 *     WM_WORKER_PROG wp=0..100
 *     WM_WORKER_DONE wp=TRUE/FALSE
 */
HANDLE start_decrypt_thread(HWND notify_hwnd,
                             wchar_t **paths, int path_cnt,
                             const wchar_t *proc_override,   /* NULL = 自动 */
                             const wchar_t *fallback_proc,   /* NULL = 用全局 */
                             const wchar_t *output_dir);     /* NULL = 覆盖 */

/* ── Worker 子进程入口（--worker 模式） ─────────────────── */
void worker_mode(void);
