/*
 * gui.h — GUI 层公开接口
 *
 * 包含：控件 ID 常量、主窗口全局句柄、
 *        进度窗口、主窗口、扩展名对话框、消息泵
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "decrypt.h"

/* ── 控件 ID ─────────────────────────────────────────────── */
#define IDC_LISTBOX              101
#define IDC_BTN_ADDFILE          102
#define IDC_BTN_ADDDIR           103
#define IDC_BTN_CLEAR            104
#define IDC_BTN_DECRYPT          105
#define IDC_BTN_INSTALL          106
#define IDC_BTN_UNINSTALL        107
#define IDC_PROGRESS             108
#define IDC_LOG                  109
/* IDC_CHECK_AUTO 110 已废弃，预留不重用 */
#define IDC_EDIT_PROC            111
/* IDC_EDIT_OUTDIR 112 / IDC_BTN_BROWSE 113 已废弃（输出目录功能移除） */
#define IDC_EDIT_FALLBACK        114
#define IDC_BTN_EXT_MAP          115
#define IDC_LIST_EXT_MAP         116
#define IDC_STATIC_EXTMAP_LBL    117
#define IDC_STATIC_FALLBACK_LBL  118
#define IDC_STATIC_FALLBACK_HINT 119

/* ── 主窗口全局控件句柄（gui.c 定义，WinMain 等可访问） ── */
extern HWND g_hwnd;
extern HWND g_hwndList;
extern HWND g_hwndLog;
extern HWND g_hwndProgress;
extern HWND g_hwndProcEdit;
extern HWND g_hwndBtnDecrypt;

/* 路径列表（gui.c 管理，decrypt 线程只读） */
extern wchar_t **g_paths;
extern int       g_path_cnt;

/* ── GUI 入口函数 ────────────────────────────────────────── */

/* --silent 模式：弹进度窗口运行解密 */
void run_progress_window(wchar_t **paths, int n);

/* GUI 主窗口模式 */
void run_main_gui(wchar_t **init_paths, int n);

/* 扩展名映射对话框（模态，阻塞直到用户关闭） */
void show_ext_map_dialog(HWND parent);
