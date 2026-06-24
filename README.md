## 免责声明

本工具仅供个人学习研究使用，不得用于商业传播或违法用途，否则一切后果自负。

---

## YiSaiTongUnLock — 亿赛通透明加密批量解密工具

### 原理

亿赛通驱动基于**进程白名单**实现透明加解密：

- 非白名单进程读文件 → 读到加密原始字节（含 `E-SafeNet` 特征头）
- 白名单进程读文件 → 驱动自动透明解密，应用层得到明文

本工具将自身复制到临时目录并重命名为白名单进程名（如 `POWERPNT.EXE`），以该名启动 Worker 子进程。Worker 以白名单进程身份读取加密文件（驱动透明解密），写出明文，父进程再通过 `MoveFileExW` 原位替换原文件。

### 限制

- 只能解密**当前电脑上有对应白名单进程**的文件格式
- 部分文件类型（如 `.STEP.CATPart`）在 `MoveFileExW` 替换后会被驱动重新加密，无法解密
- `.txt` 等无对应白名单进程的格式无法解密

### 使用方法

双击 `yst_unlock.exe` 打开主界面，拖入文件或文件夹，点击"开始解密"。

也可通过右键菜单直接对文件/文件夹调用（需先在程序内安装右键菜单）。

### 配置文件（yst_unlock.ini）

放在 `yst_unlock.exe` 同目录：

```ini
[Config]
FallbackProc=POWERPNT.EXE   ; 找不到对应进程时的兜底进程名

[ExtMap]
.pdf=POWERPNT.EXE            ; 手动指定扩展名对应的白名单进程
.docx=WINWORD.EXE
```

未配置的扩展名会自动从系统注册表查找默认关联进程。

### 构建

在 WSL / Linux 环境，需安装 `mingw-w64`：

```bash
make
```

或在 Windows PowerShell 中：

```powershell
.\build.ps1
```

### 文件说明

| 文件 | 说明 |
|------|------|
| `main.c` | 程序入口，参数解析 |
| `decrypt.c / decrypt.h` | 核心解密逻辑，Worker 管理，IPC |
| `gui.c / gui.h` | Win32 GUI 界面 |
| `check_enc.c` | 诊断工具：检测文件是否被亿赛通加密 |
| `app.rc / app.res` | 资源文件（图标、清单） |
| `Makefile` | WSL 构建脚本 |
| `build.ps1` | Windows PowerShell 构建脚本 |
| `yst_unlock.ini` | 配置文件示例 |
| `DESIGN.md` | 设计文档 |
