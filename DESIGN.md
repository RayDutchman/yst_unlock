# YST Unlock — 亿赛通透明加密文件解密工具

## 免责声明

本工具仅供个人学习与研究使用，不得用于商业传播或违法用途，使用者须自行承担一切法律责任。

---

## 背景原理

亿赛通（E-SafeNet）是一款基于 Windows 内核驱动的**透明加密系统**。其核心机制是：

- 在受保护目录中创建的文件，会被驱动自动加密写入磁盘
- 驱动维护一份**进程白名单**，只有白名单内的进程（如 `WINWORD.EXE`、`POWERPNT.EXE` 等）在读取文件时，驱动才会在内核层透明解密，应用层感知不到加密的存在
- 非白名单进程读取同一文件，读到的是原始加密字节

**加密文件头结构（前 24 字节）：**

```
偏移  内容
0     文件类型码（0x62=PNG, 0x63=PDF, 其他格式各异）
1-3   14 23 65（固定魔数）
4-11  其他元信息
12-20 "E-SafeNet"（9字节供应商标识，用于检测）
21-23 00 00 00
```

**解密思路：** 将工具本体复制一份到临时目录，重命名为白名单进程名（如 `POWERPNT.EXE`），再以该名称启动子进程（worker）。子进程被驱动视为合法进程，读文件时驱动透明解密，子进程读到明文后写出即完成解密。

---

## 功能特性

- **GUI 主界面**：拖拽文件/文件夹、选择输出目录、自定义进程名
- **右键菜单集成**：在资源管理器中右键文件或文件夹，一键解密
- **多文件单实例合并**：右键选中多个文件时，只弹一个进度窗口
- **扩展名→进程名映射表**：不同类型文件自动匹配对应的白名单进程，可在 GUI 中配置
- **注册表自动查询**：根据文件扩展名查询系统默认关联程序，自动推断白名单进程名
- **兜底进程名**：无法自动匹配时使用可配置的 fallback（默认 `POWERPNT.EXE`）
- **时间戳保留**：就地替换模式下，解密后文件的创建/修改时间与原文件相同
- **只读文件支持**：替换前自动清除只读属性
- **DPI 感知**：支持高分屏，字体和布局随系统 DPI 自动缩放

---

## 文件结构

```
YiSaiTongUnLock/
├── main.c          入口层：WinMain、命令行参数分发、单实例逻辑
├── decrypt.c       业务层：worker IPC、解密线程、INI持久化、注册表查询
├── decrypt.h       业务层公开接口
├── gui.c           GUI层：主窗口、进度窗口、扩展名映射对话框
├── gui.h           GUI层公开接口
├── app.rc          资源文件（图标、清单引用）
├── app.manifest    DPI感知声明、Visual Styles 声明
├── Makefile        构建脚本（MinGW-w64）
├── yst_unlock.ini  运行时配置（自动生成，与 exe 同目录）
└── check_enc.c     诊断工具源码（独立编译）
```

---

## 运行模式

程序通过命令行参数区分三种模式：

| 模式 | 触发方式 | 说明 |
|------|----------|------|
| GUI 模式 | 无参数启动 / 拖拽文件 | 主窗口，可交互配置并手动触发解密 |
| `--silent` 模式 | 右键菜单调用，带文件路径参数 | 直接弹出进度窗口，无主窗口 |
| `--worker` 模式 | 父进程内部启动（用户不直接调用） | 以白名单进程名运行，从 stdin 读任务，向 stdout 返回结果 |

---

## 核心解密流程

```
父进程（yst_unlock.exe）
  │
  ├─ 收集目标文件列表（递归展开文件夹，跳过 .lnk/.tmp/.ini 等）
  │
  ├─ 主循环每轮开头检查停止事件（hStopEvent），置信号则跳出
  │
  ├─ 对每个文件，确定白名单进程名：
  │   1. 用户手动指定（proc_override）
  │   2. 扩展名映射表（INI 中的 [ExtMap] 节）
  │   3. 注册表默认程序查询
  │   4. 兜底进程名（默认 POWERPNT.EXE）
  │
  ├─ 将自身复制到临时目录，重命名为白名单进程名（如 POWERPNT.EXE）
  │
  ├─ 以该名称启动 worker 子进程（CreateProcess，隐藏窗口）
  │   通过匿名管道进行 IPC：
  │     父→子  stdin:  "<src_path>\t<dst_path>\n"（UTF-8）
  │     子→父  stdout: "OK\n" 或 "ERR:<reason>\n"
  │
  ├─ worker 子进程（--worker 模式）：
  │   亿赛通驱动认为它是白名单进程，ReadFile 时透明解密
  │   将解密后的明文写入 dst_path（临时文件 .yst_tmp）
  │
  └─ 父进程收到 OK 后：
      - 就地替换模式：MoveFileExW(tmp → src)，恢复时间戳
      - 输出目录模式：文件已写入指定目录
```

---

### 优雅停止机制

`start_decrypt_thread()` 新增可选 `out_stop_event` 参数。当传入非空指针时，函数会创建一个**手动重置事件**，线程通过该事件的信号状态判断是否需要提前终止。

**停止流程：**

```
用户关闭窗口
  │
  └─ SetEvent(hStopEvent)       ← 置信号
  │
  └─ 解密线程检测到信号后：
       ├─ 跳出文件处理主循环
       ├─ stop_worker() → 关闭 stdin → worker 自然退出
       ├─ rmdir_recursive(tmp_dir) → 删除伪装 exe
       └─ NOTIFY_DONE 通知 UI
  │
  └─ WaitForSingleObject(thread, 30s)  ← 等线程自行退出
```

相比 `TerminateThread`（强制杀线程，worker 子进程变孤儿），优雅停止确保 worker 子进程被正常回收，临时目录被清理。

主窗口模式的"开始解密"不传入 `out_stop_event`（不提供停止按钮）。

### 扩展名映射对话框 DPI 支持

`ExtMapDlgProc` 新增 `extdlg_reposition_children()` 函数，在 `WM_DPICHANGED` 和 `WM_CREATE` 时重新定位所有子控件并缩放 ListView 列宽，确保跨显示器拖拽时布局正确。

---

## 进程名选择策略

亿赛通白名单通常包含常见 Office 系列进程，但具体名单因企业配置而异。实测：

- `POWERPNT.EXE` — 可解密 PDF 等多种格式（默认兜底）
- `WINWORD.EXE` — 可解密 Word 文档
- `mspaint.exe` — **不在白名单**，无法解密（即使用此名也会失败）

如果某种文件类型解密失败（worker 输出 ERR 或解密结果仍是加密字节），尝试更换进程名。

---

## 配置文件（yst_unlock.ini）

与 `yst_unlock.exe` 同目录，自动生成和读取，格式示例：

```ini
[Config]
FallbackProc=POWERPNT.EXE

[ExtMap]
.pdf=POWERPNT.EXE
.docx=WINWORD.EXE
.xlsx=EXCEL.EXE
.pptx=POWERPNT.EXE
```

- `FallbackProc`：无法匹配时使用的兜底进程名
- `[ExtMap]`：扩展名到进程名的映射，覆盖注册表自动查询结果

---

## 右键菜单安装/卸载

在主界面点击"安装右键菜单"后，程序会在以下三个注册表路径写入 shell 扩展项：

```
HKCU\Software\Classes\*\shell\YST unlock
HKCU\Software\Classes\Directory\shell\YST unlock
HKCU\Software\Classes\Directory\Background\shell\YST unlock
```

右键选中文件/文件夹后出现"YST unlock"菜单项，点击以 `--silent` 模式启动。

---

## 构建方法

环境要求：Linux/WSL + `mingw-w64`（`x86_64-w64-mingw32-gcc`）

```bash
# 编译主程序
make

# 编译诊断工具
x86_64-w64-mingw32-gcc -Os -s -DUNICODE -D_UNICODE -std=c11 \
  -mconsole -municode -o check_enc.exe check_enc.c
```

编译产物：`yst_unlock.exe`（约 90KB）、`check_enc.exe`（约 57KB）

---

## 诊断工具 check_enc.exe

用于分析文件是否被亿赛通加密，以及排查检测逻辑问题。

```cmd
check_enc.exe <file1> [file2] ...
```

输出内容：

| 项目 | 说明 |
|------|------|
| `[Size]` | 逻辑大小与磁盘占用大小 |
| `[Attr]` | 文件属性标志（`ENCRYPTED`、`REPARSE_POINT` 等） |
| `[Reparse]` | 重解析点 tag（亿赛通通常不使用） |
| `[ADS]` | 备用数据流列表及大小 |
| `[Header(normal)]` | 普通打开方式读取的前 128 字节 hex dump，扫描 `E-SafeNet` 出现位置 |
| `[BackupRead]` | 用 `BackupRead` API 读取的前 24 字节（用于对比驱动是否拦截普通读） |

**加密文件特征（无亿赛通驱动环境下）：**
- 文件头第 0 字节为文件类型码（PNG=`0x62`，PDF=`0x63`）
- 偏移 12 处存在 `"E-SafeNet"` 字符串
- 文件属性、重解析点、ADS 与普通文件无差异

---

## 已知问题与调查中事项

### 加密检测逻辑待验证

`is_file_encrypted()` 函数通过在父进程中读取文件头的 `E-SafeNet` 标记来判断文件是否加密。

当前代码（最新版本）已**移除**解密流程中的加密前置检测门控，所有文件统一交给 worker 处理。原因：

在安装了亿赛通的机器上，父进程（非白名单）读文件头的行为与驱动的交互机制尚未完全明确，检测结果在实测中存在不准情况。移除门控后，未加密文件被 worker 读取并原样写出，不会造成文件损坏。

`is_file_encrypted()` 函数保留在代码中，供后续调试和 `check_enc.exe` 参考。

### check_enc.exe 实测对比待完成

需要在安装了亿赛通的机器上，对同一加密文件运行 `check_enc.exe`，确认：
1. 父进程（非白名单）`[Header(normal)]` 中 `E-SafeNet` 是否可见
2. 文件属性/重解析点/ADS 在有驱动环境下是否与无驱动环境不同

---

## 使用限制

- 只能解密在**当前电脑**上有权限打开的文件（即电脑安装了亿赛通且当前账号有该文件的访问权限）
- 无法解密其他电脑加密、当前电脑无权访问的文件
- 解密效果取决于所选白名单进程名是否正确
