"""
亿赛通文件解密工具
原理：将自身以目标进程名（白名单进程）复制到临时目录，
      再以该"假进程"身份调用子进程读取加密文件并写出明文副本。

支持三种调用方式：
  1. 直接双击：打开 GUI
  2. --silent <file_or_dir> [<file_or_dir> ...]：静默解密（右键菜单用）
  3. --worker：内部长连接子进程模式（stdin 读任务，stdout 回写结果，不直接调用）
"""

import os
import sys
import shutil
import tempfile
import subprocess
import threading
import winreg
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from tkinterdnd2 import TkinterDnD, DND_FILES

# 检测是否以打包后的 exe 运行（Nuitka 设置 __compiled__，PyInstaller 设置 sys.frozen）
_IS_PACKED = getattr(sys, "frozen", False) or globals().get("__compiled__", False)


# ── 注册表查询：扩展名 -> 默认打开程序进程名 ────────────────────────────────

def get_default_process_for_ext(ext: str) -> str | None:
    """
    查 Windows 注册表，返回指定扩展名的默认打开程序进程名（如 WINWORD.EXE）。
    优先读用户级关联（HKCU UserChoice），回退到 HKCR。
    """
    if not ext.startswith("."):
        ext = "." + ext

    prog_id = None

    # 用户级关联
    try:
        key = rf"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\{ext}\UserChoice"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
            prog_id, _ = winreg.QueryValueEx(k, "ProgId")
    except OSError:
        pass

    # 系统级关联
    if not prog_id:
        try:
            with winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, ext) as k:
                prog_id, _ = winreg.QueryValueEx(k, "")
        except OSError:
            return None

    if not prog_id:
        return None

    # 从 ProgId 的 open 命令里提取 exe 文件名
    for verb in ("open", "Open", "edit"):
        try:
            cmd_path = rf"{prog_id}\shell\{verb}\command"
            with winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, cmd_path) as k:
                cmd, _ = winreg.QueryValueEx(k, "")
            # cmd 形如：`"C:\...\WINWORD.EXE" /n "%1"` 或 `WINWORD.EXE "%1"`
            token = cmd.strip().lstrip('"').split('"')[0].strip()
            if not token:
                token = cmd.strip().split()[0]
            name = os.path.basename(token)
            if name.lower().endswith(".exe"):
                return name
        except OSError:
            continue

    return None


def resolve_process_name(filepath: str, override: str | None) -> str:
    """返回用于伪装的进程名，优先 override，次查注册表，最后回退 wps.exe。"""
    if override:
        return override.strip()
    ext = os.path.splitext(filepath)[1].lower()
    return get_default_process_for_ext(ext) or "wps.exe"


# ── 右键菜单注册表管理 ───────────────────────────────────────────────────────

MENU_LABEL = "YST unlock"
REG_ROOTS = [
    # 文件（任意类型）
    r"Software\Classes\*\shell",
    # 文件夹
    r"Software\Classes\Directory\shell",
    # 文件夹背景
    r"Software\Classes\Directory\Background\shell",
]


def _self_exe_path() -> str:
    """返回当前可执行文件的真实绝对路径（打包或开发模式均适用）。"""
    return os.path.abspath(sys.argv[0])


def install_context_menu() -> str:
    """向 HKCU 写入右键菜单，不需要管理员权限。"""
    exe = _self_exe_path()
    errors = []
    for root_path in REG_ROOTS:
        shell_key_path = rf"{root_path}\{MENU_LABEL}"
        cmd_key_path = rf"{shell_key_path}\command"
        try:
            # Background\shell 用 %V（当前文件夹路径），其余用 %1（选中项路径）
            placeholder = "%V" if "Background" in root_path else "%1"
            if _IS_PACKED:
                # 打包后是独立 exe，直接引用
                cmd = f'"{exe}" --silent "{placeholder}"'
            else:
                # 开发模式：通过 python 解释器运行脚本
                script = os.path.abspath(__file__)
                cmd = f'"{sys.executable}" "{script}" --silent "{placeholder}"'

            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, shell_key_path) as k:
                winreg.SetValueEx(k, "", 0, winreg.REG_SZ, MENU_LABEL)
                winreg.SetValueEx(k, "Icon", 0, winreg.REG_SZ, exe)
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, cmd_key_path) as k:
                winreg.SetValueEx(k, "", 0, winreg.REG_SZ, cmd)
        except Exception as e:
            errors.append(str(e))

    return "右键菜单安装成功" if not errors else f"部分失败：{'; '.join(errors)}"


def uninstall_context_menu() -> str:
    """从 HKCU 删除右键菜单项。"""
    errors = []
    for root_path in REG_ROOTS:
        shell_key_path = rf"{root_path}\{MENU_LABEL}"
        try:
            # 先删子键 command，再删父键
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, rf"{shell_key_path}\command")
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, shell_key_path)
        except FileNotFoundError:
            pass  # 已不存在，忽略
        except Exception as e:
            errors.append(str(e))

    return "右键菜单已卸载" if not errors else f"部分失败：{'; '.join(errors)}"


# ── Worker 子进程：以伪装进程名身份复制文件 ─────────────────────────────────

def worker_copy_file():
    """
    长连接 worker 模式：从 stdin 循环读取任务，每行格式为 `src\tdst`。
    每处理一条回写 `OK\n` 或 `ERR:msg\n` 到 stdout，直到 EOF 退出。
    以当前进程名（即伪装的白名单进程名）身份运行，YST 看到的是合法进程。
    """
    # 切换 stdin/stdout 为二进制行模式，避免编码问题
    stdin  = open(sys.stdin.fileno(),  "rb", buffering=0)
    stdout = open(sys.stdout.fileno(), "wb", buffering=0)
    for raw in stdin:
        line = raw.rstrip(b"\n").rstrip(b"\r")
        if not line:
            continue
        try:
            src, dst = line.split(b"\t", 1)
            src = src.decode("utf-8")
            dst = dst.decode("utf-8")
            with open(src, "rb") as f_in:
                data = f_in.read()
            with open(dst, "wb") as f_out:
                f_out.write(data)
            stdout.write(b"OK\n")
        except Exception as e:
            msg = str(e).replace("\n", " ")
            stdout.write(f"ERR:{msg}\n".encode("utf-8"))


# ── 文件收集 ─────────────────────────────────────────────────────────────────

# 跳过的目录名（精确匹配，大小写不敏感）
_SKIP_DIRS = {
    ".git", ".svn", ".hg", ".idea", ".vscode",
    "__pycache__", "node_modules", ".cache",
    "$recycle.bin", "system volume information",
    "windows", "program files", "program files (x86)",
}

# 跳过的文件扩展名
_SKIP_EXTS = {
    ".lnk", ".tmp", ".temp", ".yst_tmp",
    ".db", ".DS_Store", ".ini", ".log",
}


def _should_skip_dir(name: str) -> bool:
    """判断目录是否应跳过（隐藏目录、系统目录、工具目录等）。"""
    if name.startswith(".") or name.startswith("_"):
        return True
    return name.lower() in _SKIP_DIRS


def _should_skip_file(path: str) -> bool:
    """判断文件是否应跳过。"""
    name = os.path.basename(path)
    # 隐藏文件或临时文件
    if name.startswith(".") or name.startswith("~$"):
        return True
    ext = os.path.splitext(name)[1].lower()
    return ext in _SKIP_EXTS


def collect_files(paths: list[str]) -> list[str]:
    """收集所有待处理文件，跳过隐藏/系统/临时目录和无关文件。"""
    result = []
    for p in paths:
        if os.path.isfile(p):
            if not _should_skip_file(p):
                result.append(p)
        elif os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                # 就地修改 dirs 以阻止 os.walk 进入被跳过的目录
                dirs[:] = [
                    d for d in dirs
                    if not _should_skip_dir(d)
                ]
                for fn in files:
                    fp = os.path.join(root, fn)
                    if not _should_skip_file(fp):
                        result.append(fp)
    return result


# ── 核心解密逻辑（GUI 和静默模式共用）───────────────────────────────────────

def _start_worker_process(fake_exe: str) -> subprocess.Popen:
    """启动一个长连接 worker 子进程，返回 Popen 对象。"""
    return subprocess.Popen(
        [fake_exe, "--worker"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )


def _send_task(proc: subprocess.Popen, src: str, dst: str) -> tuple[bool, str]:
    """
    向 worker 进程发送一条任务并读取结果。
    格式：stdin 写 `src\tdst\n`，stdout 读一行 `OK` 或 `ERR:msg`。
    """
    try:
        line = f"{src}\t{dst}\n".encode("utf-8")
        proc.stdin.write(line)
        proc.stdin.flush()
        resp = proc.stdout.readline().decode("utf-8").strip()
        if resp == "OK":
            return True, ""
        elif resp.startswith("ERR:"):
            return False, resp[4:]
        else:
            return False, f"未知响应: {resp!r}"
    except Exception as e:
        return False, str(e)


def decrypt_files(
    target_paths: list[str],
    proc_override: str | None,
    output_dir: str | None,
    log_callback,           # fn(str)，静默模式传 None
    progress_callback,      # fn(float 0-100)，静默模式传 None
    done_callback,          # fn(bool)
):
    def log(msg):
        if log_callback:
            log_callback(msg)

    files = collect_files(target_paths)
    if not files:
        log("[警告] 未找到任何文件")
        done_callback(False)
        return

    total = len(files)
    log(f"共找到 {total} 个文件，开始解密...")

    tmp_dir = tempfile.mkdtemp(prefix="yst_unlock_")
    # Nuitka onefile 运行时 sys.executable 指向临时解压目录，sys.argv[0] 才是真实 exe
    self_exe = os.path.abspath(sys.argv[0])
    success_count = 0
    fail_count = 0

    # 每个进程名对应一个长连接 worker 进程
    worker_procs: dict[str, subprocess.Popen] = {}

    # 确定输出路径的公共根（仅 output_dir 模式需要）
    if output_dir:
        common_root = (
            os.path.commonpath(target_paths)
            if len(target_paths) > 1
            else (target_paths[0] if os.path.isdir(target_paths[0]) else os.path.dirname(target_paths[0]))
        )

    try:
        for idx, src in enumerate(files, 1):
            proc_name = resolve_process_name(src, proc_override)

            # 按需启动对应进程名的 worker（首次遇到，或上次崩溃后重启）
            if proc_name not in worker_procs or worker_procs[proc_name].poll() is not None:
                fake_exe = os.path.join(tmp_dir, proc_name)
                try:
                    if not os.path.exists(fake_exe):
                        shutil.copy2(self_exe, fake_exe)
                    worker_procs[proc_name] = _start_worker_process(fake_exe)
                except Exception as e:
                    log(f"[{idx}/{total}] 失败 [{proc_name}]: {os.path.basename(src)} — 启动 worker 失败: {e}")
                    fail_count += 1
                    if progress_callback:
                        progress_callback(idx / total * 100)
                    continue

            if output_dir:
                rel = os.path.relpath(src, common_root)
                dst = os.path.join(output_dir, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
            else:
                dst = src + ".yst_tmp"

            ok, err = _send_task(worker_procs[proc_name], src, dst)

            if ok and not output_dir:
                try:
                    os.replace(dst, src)
                except Exception as e:
                    ok, err = False, f"替换原文件失败: {e}"
                    if os.path.exists(dst):
                        os.remove(dst)
            elif not ok and not output_dir and os.path.exists(dst):
                os.remove(dst)

            if ok:
                success_count += 1
                log(f"[{idx}/{total}] 成功 [{proc_name}]: {os.path.basename(src)}")
            else:
                fail_count += 1
                log(f"[{idx}/{total}] 失败 [{proc_name}]: {os.path.basename(src)} — {err}")

            if progress_callback:
                progress_callback(idx / total * 100)

    except Exception as e:
        log(f"[错误] 解密中断: {e}")
    finally:
        for proc in worker_procs.values():
            try:
                proc.stdin.close()
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
        shutil.rmtree(tmp_dir, ignore_errors=True)
        log(f"\n完成：成功 {success_count} 个，失败 {fail_count} 个")
        # 无论正常结束还是异常，都触发 done_callback
        done_callback(success_count > 0)


# ── 进度窗口（右键菜单调用时弹出）──────────────────────────────────────────

class ProgressWindow(tk.Tk):
    """
    轻量进度窗口：显示进度条 + 日志，解密完成后自动关闭。
    使用标准 tk.Tk 而非 TkinterDnD，避免引入不必要依赖。
    """
    _AUTO_CLOSE_MS = 3000   # 完成后 3 秒自动关闭

    def __init__(self, paths: list[str]):
        super().__init__()
        self.title("亿赛通解密")
        self.resizable(False, False)
        self.geometry("480x240")
        # 居中
        self.update_idletasks()
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        x, y = (sw - 480) // 2, (sh - 240) // 2
        self.geometry(f"480x240+{x}+{y}")
        self._paths = paths
        self._destroyed = False         # 防止回调操作已销毁的窗口
        self._auto_close_id = None      # after() 返回的 id，用于取消
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _on_close(self):
        """用户点击关闭按钮或按 Alt+F4 时调用。"""
        self._destroyed = True
        if self._auto_close_id is not None:
            self.after_cancel(self._auto_close_id)
        self.destroy()

    def _build_ui(self):
        pad = {"padx": 10, "pady": 4}

        self._status = tk.StringVar(value="准备中...")
        ttk.Label(self, textvariable=self._status).pack(fill="x", **pad)

        self._progress = ttk.Progressbar(self, mode="determinate", length=440)
        self._progress.pack(**pad)

        self._log_text = tk.Text(self, height=6, state="disabled", wrap="none",
                                 font=("Consolas", 9))
        sy = ttk.Scrollbar(self, orient="vertical", command=self._log_text.yview)
        self._log_text.configure(yscrollcommand=sy.set)
        sy.pack(side="right", fill="y", padx=(0, 4))
        self._log_text.pack(fill="both", expand=True, padx=(10, 0), pady=4)

        self._btn_close = ttk.Button(self, text="关闭", command=self._on_close, state="disabled")
        self._btn_close.pack(pady=(0, 8))

    def _log(self, msg: str):
        if self._destroyed:
            return
        self._log_text.configure(state="normal")
        self._log_text.insert("end", msg + "\n")
        self._log_text.see("end")
        self._log_text.configure(state="disabled")

    def _set_progress(self, val: float):
        if self._destroyed:
            return
        self._progress["value"] = val
        self._status.set(f"解密中... {val:.0f}%")

    def _on_done(self, ok: bool):
        if self._destroyed:
            return
        self._status.set("解密完成" if ok else "解密完成（有失败项）")
        self._progress["value"] = 100
        self._btn_close.configure(state="normal")
        # 3 秒后自动关闭，记录 id 以便手动关闭时取消
        self._auto_close_id = self.after(self._AUTO_CLOSE_MS, self._on_close)

    def start(self):
        threading.Thread(
            target=decrypt_files,
            args=(
                self._paths,
                None,           # proc_override=None：按扩展名自动查
                None,           # output_dir=None：覆盖原文件
                lambda msg: self.after(0, self._log, msg),
                lambda val: self.after(0, self._set_progress, val),
                lambda ok: self.after(0, self._on_done, ok),
            ),
            daemon=True,
        ).start()


def run_with_progress(paths: list[str]):
    """右键菜单调用入口：弹出进度窗口执行解密（自动查进程名 + 覆盖原文件）。"""
    win = ProgressWindow(paths)
    win.after(100, win.start)   # 窗口渲染后再开始，避免白屏
    win.mainloop()


# ── GUI ──────────────────────────────────────────────────────────────────────

class App(TkinterDnD.Tk):
    def __init__(self, initial_paths: list[str] | None = None):
        super().__init__()
        self.title("YST unlock")
        self.resizable(True, True)
        self.minsize(660, 520)
        self._selected_paths: list[str] = []
        self._build_ui()
        if initial_paths:
            for p in initial_paths:
                self._add_path(p)

    def _build_ui(self):
        pad = {"padx": 8, "pady": 4}

        # ── 文件选择区 ────────────────────────────────────
        frame_top = ttk.LabelFrame(self, text="待解密文件 / 文件夹（可拖入）", padding=6)
        frame_top.pack(fill="both", expand=True, **pad)

        btn_row = ttk.Frame(frame_top)
        btn_row.pack(fill="x")
        ttk.Button(btn_row, text="添加文件", command=self._add_files).pack(side="left", padx=4)
        ttk.Button(btn_row, text="添加文件夹", command=self._add_folder).pack(side="left", padx=4)
        ttk.Button(btn_row, text="清空列表", command=self._clear_list).pack(side="right", padx=4)

        list_frame = ttk.Frame(frame_top)
        list_frame.pack(fill="both", expand=True, pady=4)

        self.listbox = tk.Listbox(list_frame, selectmode="extended", height=8,
                                  activestyle="none")
        scroll_y = ttk.Scrollbar(list_frame, orient="vertical", command=self.listbox.yview)
        scroll_x = ttk.Scrollbar(list_frame, orient="horizontal", command=self.listbox.xview)
        self.listbox.configure(yscrollcommand=scroll_y.set, xscrollcommand=scroll_x.set)
        scroll_y.pack(side="right", fill="y")
        scroll_x.pack(side="bottom", fill="x")
        self.listbox.pack(side="left", fill="both", expand=True)

        # 右键菜单（列表项删除）
        ctx = tk.Menu(self, tearoff=0)
        ctx.add_command(label="删除所选", command=self._remove_selected)
        self.listbox.bind("<Button-3>", lambda e: ctx.post(e.x_root, e.y_root))

        # 拖拽支持
        self.listbox.drop_target_register(DND_FILES)
        self.listbox.dnd_bind("<<Drop>>", self._on_drop)

        # ── 配置区 ────────────────────────────────────────
        frame_cfg = ttk.LabelFrame(self, text="解密配置", padding=6)
        frame_cfg.pack(fill="x", **pad)

        # 自动/手动进程名
        self.auto_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            frame_cfg, text="推荐设置",
            variable=self.auto_var, command=self._toggle_auto
        ).grid(row=0, column=0, columnspan=4, sticky="w", padx=4, pady=2)

        ttk.Label(frame_cfg, text="指定进程名:").grid(row=1, column=0, sticky="w", padx=4)
        self.proc_var = tk.StringVar()
        self.proc_entry = ttk.Entry(frame_cfg, textvariable=self.proc_var, width=22, state="disabled")
        self.proc_entry.grid(row=1, column=1, sticky="w", padx=4)
        ttk.Label(frame_cfg, text="（如 EXCEL.EXE）", foreground="gray").grid(row=1, column=2, sticky="w")

        # 输出目录
        ttk.Label(frame_cfg, text="输出目录:").grid(row=2, column=0, sticky="w", padx=4, pady=4)
        self.out_var = tk.StringVar()
        ttk.Entry(frame_cfg, textvariable=self.out_var, width=34).grid(row=2, column=1, columnspan=2, sticky="ew", padx=4)
        ttk.Button(frame_cfg, text="浏览...", command=self._pick_output).grid(row=2, column=3, padx=4)
        ttk.Label(frame_cfg, text="留空则覆盖原文件", foreground="gray").grid(row=2, column=4, sticky="w")
        frame_cfg.columnconfigure(1, weight=1)

        # ── 日志 + 进度 ───────────────────────────────────
        frame_bot = ttk.LabelFrame(self, text="日志", padding=6)
        frame_bot.pack(fill="both", expand=True, **pad)

        self.progress = ttk.Progressbar(frame_bot, mode="determinate")
        self.progress.pack(fill="x", pady=(0, 4))

        self.log_text = tk.Text(frame_bot, height=7, state="disabled", wrap="none")
        ls_y = ttk.Scrollbar(frame_bot, orient="vertical", command=self.log_text.yview)
        ls_x = ttk.Scrollbar(frame_bot, orient="horizontal", command=self.log_text.xview)
        self.log_text.configure(yscrollcommand=ls_y.set, xscrollcommand=ls_x.set)
        ls_y.pack(side="right", fill="y")
        ls_x.pack(side="bottom", fill="x")
        self.log_text.pack(side="left", fill="both", expand=True)

        # ── 按钮行 ────────────────────────────────────────
        btn_bottom = ttk.Frame(self)
        btn_bottom.pack(fill="x", padx=8, pady=(0, 8))

        # 左侧：右键菜单管理
        ttk.Button(btn_bottom, text="安装右键菜单", command=self._install_ctx).pack(side="left", padx=4)
        ttk.Button(btn_bottom, text="卸载右键菜单", command=self._uninstall_ctx).pack(side="left", padx=4)

        # 右侧：解密
        self.btn_decrypt = ttk.Button(btn_bottom, text="开始解密", command=self._start_decrypt)
        self.btn_decrypt.pack(side="right", padx=4)

    # ── 拖拽 ──────────────────────────────────────────────

    def _on_drop(self, event):
        """处理从资源管理器拖入的文件/文件夹。"""
        # tkinterdnd2 返回的格式：`{path with spaces} path2 ...`，用 tk.splitlist 解析
        raw = event.data
        try:
            paths = self.tk.splitlist(raw)
        except Exception:
            paths = raw.split()
        for p in paths:
            p = p.strip()
            if p:
                self._add_path(p)

    # ── 路径管理 ──────────────────────────────────────────

    def _add_path(self, p: str):
        p = os.path.normpath(p)
        if p not in self._selected_paths:
            self._selected_paths.append(p)
            self.listbox.insert("end", p)

    def _add_files(self):
        for p in filedialog.askopenfilenames(title="选择文件"):
            self._add_path(p)

    def _add_folder(self):
        p = filedialog.askdirectory(title="选择文件夹")
        if p:
            self._add_path(p)

    def _clear_list(self):
        self._selected_paths.clear()
        self.listbox.delete(0, "end")

    def _remove_selected(self):
        for idx in reversed(self.listbox.curselection()):
            self.listbox.delete(idx)
            self._selected_paths.pop(idx)

    def _pick_output(self):
        p = filedialog.askdirectory(title="选择输出目录")
        if p:
            self.out_var.set(p)

    # ── 配置联动 ──────────────────────────────────────────

    def _toggle_auto(self):
        if self.auto_var.get():
            self.proc_entry.configure(state="disabled")
            self.proc_var.set("")
        else:
            self.proc_entry.configure(state="normal")

    # ── 右键菜单 ──────────────────────────────────────────

    def _install_ctx(self):
        msg = install_context_menu()
        messagebox.showinfo("右键菜单", msg)

    def _uninstall_ctx(self):
        msg = uninstall_context_menu()
        messagebox.showinfo("右键菜单", msg)

    # ── 日志 / 进度 ───────────────────────────────────────

    def _log(self, msg: str):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", msg + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _set_progress(self, val: float):
        self.progress["value"] = val

    # ── 解密 ──────────────────────────────────────────────

    def _start_decrypt(self):
        if not self._selected_paths:
            messagebox.showwarning("提示", "请先添加要解密的文件或文件夹")
            return

        proc_override = None if self.auto_var.get() else self.proc_var.get().strip()
        if not self.auto_var.get() and not proc_override:
            messagebox.showwarning("提示", "已关闭自动模式，请填写进程名")
            return

        out_dir = self.out_var.get().strip() or None
        if out_dir:
            try:
                os.makedirs(out_dir, exist_ok=True)
            except Exception as e:
                messagebox.showerror("错误", f"无法创建输出目录：{e}")
                return

        self.btn_decrypt.configure(state="disabled")
        self.progress["value"] = 0
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

        mode = "自动" if self.auto_var.get() else proc_override
        self._log(f"模式：{mode}")

        threading.Thread(
            target=decrypt_files,
            args=(
                list(self._selected_paths),
                proc_override,
                out_dir,
                lambda msg: self.after(0, self._log, msg),
                lambda val: self.after(0, self._set_progress, val),
                lambda ok: self.after(0, self.btn_decrypt.configure, {"state": "normal"}),
            ),
            daemon=True,
        ).start()


# ── 入口 ─────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]

    # worker 子进程模式（长连接，从 stdin 读任务）
    if args and args[0] == "--worker":
        worker_copy_file()
        return

    # 右键菜单调用（弹出进度窗口）
    if args and args[0] == "--silent":
        run_with_progress(args[1:])
        return

    # GUI 模式，可带初始路径参数
    initial = args if args else None
    App(initial_paths=initial).mainloop()


if __name__ == "__main__":
    main()
