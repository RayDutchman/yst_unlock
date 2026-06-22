#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

/* ── 辅助：打印 GetLastError ──────────────────────────── */
static void print_err(const char *tag) {
    printf("  [%s] Error: %lu\n", tag, GetLastError());
}

/* ── 文件大小 ─────────────────────────────────────────── */
static void check_size(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) { print_err("Size"); return; }
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz))
        printf("  [Size] Logical size: %lld bytes\n", sz.QuadPart);
    else
        print_err("Size");
    CloseHandle(h);

    /* 磁盘占用大小（压缩/稀疏文件可能与逻辑大小不同） */
    DWORD hi = 0;
    DWORD lo = GetCompressedFileSizeW(path, &hi);
    if (lo != INVALID_FILE_SIZE)
        printf("  [Size] Compressed/disk size: %lld bytes\n",
               ((LONGLONG)hi << 32) | lo);
}

/* ── 文件头 dump（普通打开）──────────────────────────── */
static void check_header(const wchar_t *path, const char *label,
                         DWORD extra_flags) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN | extra_flags, NULL);
    if (h == INVALID_HANDLE_VALUE) { print_err(label); return; }

    BYTE buf[128];
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &rd, NULL);
    CloseHandle(h);

    if (!ok || rd < 4) {
        printf("  [%s] Read failed (rd=%lu, err=%lu)\n", label, rd, GetLastError());
        return;
    }

    /* 完整 hex dump（最多 128 字节，每行 16） */
    printf("  [%s] First %lu bytes:\n", label, rd);
    for (DWORD r = 0; r < rd; r += 16) {
        printf("    %04lX: ", r);
        for (DWORD c = r; c < r+16 && c < rd; c++)
            printf("%02X ", buf[c]);
        /* 补齐空格 */
        for (DWORD c = rd; c < r+16; c++) printf("   ");
        printf(" | ");
        for (DWORD c = r; c < r+16 && c < rd; c++)
            printf("%c", (buf[c] >= 0x20 && buf[c] < 0x7F) ? buf[c] : '.');
        printf("\n");
    }

    /* E-SafeNet 扫描：在整个 buf 里找所有出现位置 */
    printf("  [%s] E-SafeNet scan:\n", label);
    {
        int found = 0;
        for (DWORD i = 0; i + 9 <= rd; i++) {
            if (memcmp(buf + i, "E-SafeNet", 9) == 0) {
                printf("    -> Found at offset %lu\n", i);
                found++;
            }
        }
        if (!found) printf("    -> (not found in first %lu bytes)\n", rd);
    }

    /* 亿赛通魔数扫描：14 23 65 在偏移 1-3 是更可靠的标志 */
    printf("  [%s] Magic check:\n", label);
    if (rd >= 4 && memcmp(buf + 1, "\x14\x23\x65", 3) == 0)
        printf("    -> Magic 14 23 65 FOUND at offset 1 (type code=0x%02X)\n", buf[0]);
    else
        printf("    -> Magic 14 23 65 NOT FOUND\n");
}

/* ── 文件属性 ─────────────────────────────────────────── */
static void check_attributes(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) { print_err("Attr"); return; }
    printf("  [Attr] 0x%08lX", attr);
    if (attr & FILE_ATTRIBUTE_ENCRYPTED)     printf(" ENCRYPTED");
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) printf(" REPARSE_POINT");
    if (attr & FILE_ATTRIBUTE_OFFLINE)       printf(" OFFLINE");
    if (attr & FILE_ATTRIBUTE_COMPRESSED)    printf(" COMPRESSED");
    if (attr & FILE_ATTRIBUTE_HIDDEN)        printf(" HIDDEN");
    if (attr & FILE_ATTRIBUTE_SYSTEM)        printf(" SYSTEM");
    printf("\n");
}

/* ── 重解析点 ─────────────────────────────────────────── */
static void check_reparse(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) { print_err("Reparse"); return; }
    BYTE buf[4096];
    DWORD ret = 0;
    if (DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0,
                        buf, sizeof(buf), &ret, NULL)) {
        ULONG tag = *(ULONG*)buf;
        printf("  [Reparse] Tag=0x%08lX\n", tag);
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_NOT_A_REPARSE_POINT)
            printf("  [Reparse] (not a reparse point)\n");
        else
            printf("  [Reparse] IOCTL failed, err=%lu\n", err);
    }
    CloseHandle(h);
}

/* ── 备用数据流（ADS）────────────────────────────────── */
static void check_ads(const wchar_t *path) {
    WIN32_FIND_STREAM_DATA fsd;
    HANDLE h = FindFirstStreamW(path, FindStreamInfoStandard, &fsd, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF || err == ERROR_FILE_NOT_FOUND)
            printf("  [ADS] (none)\n");
        else
            printf("  [ADS] Error %lu\n", err);
        return;
    }
    printf("  [ADS] Streams:\n");
    do {
        wprintf(L"    %ls  (size=%lld)\n",
                fsd.cStreamName, fsd.StreamSize.QuadPart);
    } while (FindNextStreamW(h, &fsd));
    FindClose(h);
}

/* ── 用 BackupRead 绕过驱动 filter 读头部 ────────────── */
static void check_header_backup(const wchar_t *path) {
    /* BackupRead 会调用 IRP_MJ_READ 但带 backup intent，
     * 某些驱动对此不做 hook，可以用于判断驱动是否拦截了普通读 */
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) { print_err("BackupRead"); return; }

    BYTE buf[128];
    DWORD rd = 0;
    LPVOID ctx = NULL;
    BOOL ok = BackupRead(h, buf, sizeof(buf), &rd, FALSE, TRUE, &ctx);
    if (ctx) BackupRead(h, NULL, 0, &rd, TRUE, FALSE, &ctx); /* 释放 ctx */
    CloseHandle(h);

    if (!ok || rd < 4) {
        printf("  [BackupRead] failed (rd=%lu)\n", rd);
        return;
    }

    /* 只打印前 24 字节和 E-SafeNet 扫描结果，避免输出太多 */
    printf("  [BackupRead] Bytes 0-23: ");
    for (DWORD i = 0; i < 24 && i < rd; i++) printf("%02X ", buf[i]);
    printf("\n");
    int found = 0;
    for (DWORD i = 0; i + 9 <= rd; i++)
        if (memcmp(buf+i, "E-SafeNet", 9) == 0) { found = 1; printf("  [BackupRead] E-SafeNet at offset %lu\n", i); }
    if (!found) printf("  [BackupRead] E-SafeNet: NOT FOUND\n");
}

/* ── 入口 ─────────────────────────────────────────────── */
int main(void) {
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) {
        printf("Usage: check_enc.exe <file1> [file2 ...]\n");
        printf("  Dumps file header, attributes, reparse point, ADS,\n");
        printf("  and BackupRead result for each file.\n");
        LocalFree(argv);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        wprintf(L"\n=== %ls ===\n", argv[i]);
        check_size(argv[i]);
        check_attributes(argv[i]);
        check_reparse(argv[i]);
        check_ads(argv[i]);
        check_header(argv[i], "Header(normal)", 0);
        check_header_backup(argv[i]);
    }
    printf("\nDone.\n");
    LocalFree(argv);
    return 0;
}
