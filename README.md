# msitool

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C)
![MSVC](https://img.shields.io/badge/MSVC-2017%2B-purple)
![Windows](https://img.shields.io/badge/platform-Windows-blue)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

> 用 **MSVC + C++17 + Windows Installer (Msi) API** 编写的工具，用来**查看、解包、打包** MSI 安装包；带一个纯 Win32 自绘的 Fluent 风格图形界面，同时完整保留命令行。

`msitool` 是一个零外部依赖的单文件程序：运行时只使用 Windows 自带的 `msi.dll` 与 `cabinet.dll`，不需要 WiX、Orca、7-Zip、Python、.NET 或任何 UI 框架，不需要管理员权限，也**不会真正安装任何东西**。它直接读取 MSI 的 OLE 复合存储与表，用 FDI/FCI 处理 CAB，适合逆向分析、离线审计、CI 中的包检查，以及把目录快速做成一个可安装的 MSI。双击 exe 打开图形界面，在终端里带参数运行则仍是原来的 CLI。

- **只读安全**：数据库一律以 `MSIDBOPEN_READONLY` 打开（`query` 也无法修改包）。
- **Unicode**：全程 W 版 API + `std::wstring`，输出 UTF-8，支持中文/空格路径与文件名。
- **自包含**：内嵌 CAB 直接从 `_Streams` 读出；未压缩 administrative layout 也能还原。
- **可验证**：生成的包会经 `MsiVerifyPackageW` 自检，并与源文件做哈希往返校验。

## 目录

- [特性](#特性)
- [快速开始](#快速开始)
- [图形界面（Fluent UI）](#图形界面fluent-ui)
- [安装](#安装)
- [命令参考](#命令参考)
- [工作原理](#工作原理)
- [项目结构](#项目结构)
- [构建详解](#构建详解)
- [测试与验证](#测试与验证)
- [实测过的样本](#实测过的样本)
- [TODO](#todo)
- [已知限制](#已知限制)
- [故障排除 FAQ](#故障排除-faq)
- [安全](#安全)
- [贡献](#贡献)
- [许可证](#许可证)
- [致谢与参考](#致谢与参考)

---

## 特性

| 能力 | 说明 |
| --- | --- |
| 包预检 | 打开前校验 OLE 复合文件签名、`MsiOpenDatabaseW`、`_Tables` 结构；`verify` 额外调用 `MsiVerifyPackageW` |
| 查看 | SummaryInformation、Property、所有表与行数、列定义/类型/主键、任意 MSI SQL 查询 |
| 导出 | 全部表为 `.idt`、全部 `_Streams` 原始流、`Binary` 条目、`summary.txt`、CAB + `manifest.txt` |
| 解包 | 还原安装目录树；内嵌 CAB（`_Streams`）、外部 CAB、未压缩 layout、跨 CAB 连续文件（FDI 分卷集）、`.msp` 补丁负载都支持；`--transform` 可查看应用 transform 后的视图；GUI 另提供资源管理器式浏览（目录树 + 文件列表、筛选、右键菜单、双击单文件解包、目录递归解包、自动输出目录） |
| 打包 | 把目录做成自包含 MSI：FCI 生成一个或多个 MSZIP CAB + Msi API 建库/写流/写摘要，可选分卷、空目录、快捷方式、注册表、服务、环境变量、多 Feature、UI 与数字签名 |
| 输出 | UTF-8 文本表格或 `--tsv`；控制字符转义为 `\xNN`；警告与详细日志走 stderr |
| 图形界面 | 纯 Win32 API（user32 / gdi32 / shell32 / ole32）自绘的 Fluent 界面：亮/暗主题、高 DPI、导航、卡片、数据网格、SQL 查询、解包/导出/打包表单、实时日志与进度；**不使用任何第三方库或 UI 框架** |

---

## 快速开始

### 环境要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows（需系统自带 `msi.dll` / `cabinet.dll`）；在 Windows 10/11 上完成验证 |
| 编译器 | MSVC（Visual Studio 2017+，实测 VS 2022 / MSVC 19.44） |
| SDK | Windows SDK 10，提供 `msi.h`、`msiquery.h`、`fdi.h`、`fci.h`、`Msi.Lib`、`Cabinet.Lib` |
| 构建工具 | `build.bat`（调用 `cl.exe`）或 CMake 3.20+ |
| 运行时依赖 | 无额外依赖，不需要管理员权限 |

> 源码是 UTF-8 无 BOM，编译必须带 `/utf-8`（构建脚本与 CMake 已配置）。

### 构建

```bat
cd E:\work\winmsi
build.bat            :: x64 -> build\msitool_x64.exe
build.bat x86        :: x86 -> build\msitool_x86.exe
build.bat clean      :: 清理构建产物
```

脚本会自动定位 `vcvars64.bat` / `vcvars32.bat`（Developer Command Prompt → `vswhere.exe` → 常见安装目录）。非标准安装可显式指定：

```bat
set MSIT_VCVARS=D:\vs\Community\VC\Auxiliary\Build\vcvars64.bat
build.bat
```

### 第一条命令

```bat
build\msitool_x64.exe --help
build\msitool_x64.exe verify setup.msi
build\msitool_x64.exe info setup.msi
build\msitool_x64.exe extract setup.msi C:\out\files --verbose
```

### 启动图形界面

```bat
build\msitool_x64.exe                       :: 不带参数 = GUI
build\msitool_x64.exe gui                   :: 显式启动 GUI
build\msitool_x64.exe gui setup.msi         :: 启动 GUI 并直接打开一个包
```

也可以直接把 `.msi` / `.msp` 拖进窗口。GUI 与 CLI 是同一个 exe：带命令参数时在终端里以 CLI 运行（自动附加父控制台），不带参数时是无控制台的窗口程序。

---

## 图形界面（Fluent UI）

### 设计

整个 UI 层在 `src/ui_*.cpp` 中，**只调用 Windows API**：

- 窗口、消息循环、拖拽、高 DPI（`SetProcessDpiAwarenessContext` / `GetDpiForWindow`）、系统文件对话框（`IFileDialog`）、剪贴板、注册表全部用 user32 / shell32 / ole32 / advapi32；
- 所有像素由 gdi32 绘制到内存 DC 双缓冲：圆角卡片、渐变 Hero、导航选中态、数据网格、切换开关、进度条、日志、Toast、以及 30 多个用 `MoveToEx`/`LineTo`/`RoundRect`/`Ellipse` 画的矢量图标；
- 每个控件都是一个自绘 `HWND`（共用一套窗口过程 + C++ 对象路由），只有文本输入使用系统原生 `EDIT` 子窗口以获得输入法、光标与选字能力，边框、占位文字、焦点强调线仍由 GDI 绘制；
- 布局全部按当前客户区尺寸计算：列宽、行距、控件高度、日志高度随窗口缩放 / 最大化实时自适应；打包表单拆成 TabView 分页，小窗口下也不会字段堆叠；解包页隐去全局「当前包文件」栏与页面大标题，整页就是文件浏览器，选项与日志默认收起；
- 无边框标题栏通过 `WM_NCCALCSIZE` / `WM_NCHITTEST` 自绘，并动态加载 `dwmapi.dll` 设置 Win11 圆角与深色边框；
- 亮/暗/跟随系统三套主题，主题设置与最近打开的包保存在 `HKCU\Software\msitool`；所有尺寸按 DPI 缩放；
- 耗时的 `extract` / `export` / `pack` 在工作线程执行，通过 `PostMessage` 把日志与完成状态送回 UI 线程；`util::outLine` / `errLine` 新增可选日志钩子，让现有 CLI 代码的输出直接变成 GUI 日志。

### 页面

| 页面 | 功能 |
| --- | --- |
| 概览 | 包摘要、快捷操作、渐变 Hero |
| 包信息 | 概览头部（产品名 / 版本 / 大小 / 表数 / CAB / 流）+「摘要 / 属性 / 媒体」分区，全宽表格、关键字筛选、选中行详情与 TSV 复制；属性区展示 Property 表全部键值 |
| 数据表 | 表筛选 + 任意表的数据网格（虚拟滚动、选中、Ctrl+C 复制 TSV） |
| SQL 查询 | MSI SQL 编辑器、示例查询、结果网格与耗时 |
| 解包 | **整页是文件资源管理器**：命令 / 地址栏（后退、前进、上一级、刷新、可点击面包屑、搜索框、解包全部、解包选中、选项、日志）+ 左侧导航树 + 右侧详细信息列表（文件夹与文件混排、图标、类型列、点击表头排序、双击进入目录 / 双击文件解包、右键菜单）；常驻「解包到」位置条可直接编辑路径、点「浏览…」选目录、从「最近」下拉复用历史目录、点「打开」查看，默认自动填 `<包名>_files`；六个开关、MST transform 与基准 MSI 收进可折叠「选项」，运行日志收进可折叠「日志」抽屉 |
| 导出 | CAB / 完整归档两种模式、运行日志 |
| 打包 | 「基本 / 高级 / 签名」三个标签页，字段按窗口尺寸自适应两列排布；右上角「开始打包」常驻，完成后自动打开新 MSI |
| 验证 | 签名 / `MsiOpenDatabase` / `MsiVerifyPackage` / `_Tables` / 媒体检查报告 |
| 设置 | 主题、DPI 信息、关于与快捷键 |

### 快捷键

| 按键 | 作用 |
| --- | --- |
| `Ctrl+O` | 打开包文件 |
| `F5` | 重新载入当前包 |
| `Ctrl+C` | 复制数据网格 / 日志内容 |
| `Ctrl+Enter` | 运行 SQL 查询 |
| `Tab` / `Shift+Tab` | 在控件之间切换焦点 |
| `Esc` | 关闭下拉菜单 / 清除提示 |

---

## 安装

本项目目前**不发布预编译二进制**（见 [TODO](#todo)）。从源码构建后，把 `build\msitool_x64.exe` 复制到任意目录并加入 `PATH` 即可：

```bat
setx PATH "%PATH%;E:\work\winmsi\build"
```

`build\`、`build-cmake\`、`_test\` 等产物已被 `.gitignore` 忽略，仓库里只有源码与文档。

---

## 命令参考

### 命令一览

```
msitool <command> <package.msi> [arguments...] [options...]
msitool gui [package.msi]        :: 图形界面
msitool                          :: 不带参数 = 图形界面
```

> 带命令参数时是原样的 CLI；不带参数（或 `gui` / `--gui` / `/gui`）时启动 [图形界面](#图形界面fluent-ui)。

| 命令 | 参数 | 作用 |
| --- | --- | --- |
| `verify` | `<包>` | 验证文件是否是真正的 MSI/MSP；输出完整报告，VALID=0 / INVALID=1 |
| `info` | `<包>` | 摘要信息、关键属性、Media/CAB 清单 |
| `tables` | `<包>` | 所有表、是否持久表、行数 |
| `schema` | `<包> <表名>` | 列名、MSI 类型、是否可空、主键 |
| `query` | `<包> <SQL>` | 执行任意**只读** MSI SQL 查询 |
| `list` | `<包> <名称>` | 常用表或任意表；`--tsv` 便于脚本处理 |
| `cabs` | `<包> <输出目录>` | 导出内嵌/外部 CAB + `manifest.txt` |
| `export` | `<包> <输出目录>` | 导出全部表 `.idt`、全部流、Binary、`summary.txt` |
| `extract` | `<包> <输出目录>` | 解包：还原目录树并解压 CAB 中的文件；`.msp` 导出补丁负载 |
| `pack` | `<源目录> <输出.msi>` | 打包：目录 → 自包含 MSI（一个或多个内嵌 MSZIP CAB） |

`list` 的常用名称：`files`、`components`、`features`、`dirs` / `directories`、`media`、`properties`、`streams`、`cabs`、`shortcuts`、`registry`、`services`、`customactions`、`upgrade`、`launchconditions`、`featurecomponents`、`binaries`、`assemblies`、`tables`，也可直接写任意真实表名。

### 全局选项

| 选项 | 作用 |
| --- | --- |
| `-h`, `--help`, `/?` | 显示帮助 |
| `-v`, `--verbose` | 详细日志（stderr，前缀 `[v]`） |
| `--tsv` | 表格以 TSV 输出（单元格内的 tab/换行会转义） |

### 解包选项（`extract`）

| 选项 | 作用 |
| --- | --- |
| `--flat` | 不还原目录层次，全部平铺；重名自动加序号 |
| `--dry-run` | 只枚举 CAB 内容并打印计划，不写文件 |
| `--skip-existing` | 跳过已存在的目标文件 |
| `--source-layout` | 按未压缩源镜像的目录树输出，而不是安装后的目录树 |
| `--keep-temp` | 保留暂存的 CAB 临时目录，便于排查 |
| `--transform <file.mst>` | 把 MST 作为**临时视图**应用到只读数据库；`info` / `tables` / `schema` / `query` / `list` / `extract` / `verify` 看到的都是 transform 后的表（可重复指定） |
| `--base-msi <file.msi>` | 解包 `.msp` 时，把补丁里的 transform 应用到该目标 MSI，并按真实目录名布局输出 |

示例：

```bat
msitool extract setup.msi C:\out\files --verbose
msitool extract setup.msi C:\out\files --flat --skip-existing
msitool extract setup.msi C:\out\files --dry-run
msitool extract setup.msi C:\out\files --transform custom.mst
msitool extract update.msp C:\out\patch --base-msi setup.msi
```

真实输出（Windows SDK Debuggers MSI，18 个 CAB）：

```
[cab 18/18] #e8bc712abeffd7c9711ee3f55d4aa99b.cab (1383515 bytes, 5 file(s), 7 name(s))

Extracted 473 file(s) from 18 cabinet(s), 0 copied from layout, 0 skipped, 0 warning(s).  File table: 473 row(s).
Output: E:\work\winmsi\out
```

### 解包进阶

#### 跨 CAB 连续文件（cabinet set）

有些打包工具会让一个 MSZIP folder / 文件**横跨多个 CAB**（CAB 头里的 `hasnext` / `hasprev`、`setID`、`iCabinet` 串成一个分卷集，FDI 会回调 `fdintNEXT_CABINET` 请求下一个 CAB）。`extract` 会：

1. 把 `Media` 里的所有 CAB 先暂存到同一个短临时目录，并保留 `Media.Cabinet` 里的文件名；
2. 用 `FDIIsCabinet` 读每个 CAB 头，按 `hasnext/hasprev` + `setID` + 连续 `iCabinet` 把它们分组为分卷集；
3. 每个分卷集只跑一次 `FDICopy`，在 `fdintNEXT_CABINET` 回调里把下一个 CAB 的路径交给 FDI（必要时生成别名文件），跨 CAB 的文件因此被写成**一个完整文件**；
4. `Media.LastSequence` 相同的多条记录（跨 CAB 文件常这样写）会合并到同一个分卷集，文件不会被重复还原。

独立 CAB（包括 `pack --cab-size` 生成的多 CAB）仍是各自一组，行为不变。

#### transform 后的视图

`--transform <file.mst>` 用 `MsiDatabaseApplyTransformW` 把 MST 应用为**只读临时视图**（不修改包、不落盘），之后所有查看 / 解包命令读到的都是 transform 后的 `File` / `Property` / `Directory` 等表。可重复指定多个 MST，按顺序应用；`verify` 也会在报告里列出已应用的 transform。

```bat
msitool list setup.msi properties --transform custom.mst
msitool extract setup.msi C:\out\files --transform custom.mst
```

#### `.msp` 补丁解包

`extract` 对 `.msp`（用 `MSIDBOPEN_PATCHFILE` 打开）不再只做查看，而是：

- 遍历 `_Streams`，按签名分类：`MSCF` → CAB 负载、`D0 CF 11 E0` 且流名以 `#` 开头 → transform、其余 → 原始流；
- CAB 负载用 FDI 解到 `<输出目录>\payload\`（文件名为 CAB 内部名 / File 表键），transform 写到 `transforms\*.mst`，其余流写到 `streams\`；
- 写 `manifest.txt`：`MsiPatchMetadata`、SummaryInformation、每条流的大小与去向、解出的负载文件清单；
- 可选 `--base-msi <file.msi>`：把补丁里的 transform（以及 `--transform` 指定的额外 MST）应用到目标 MSI 的临时视图，并把补丁 CAB 作为额外 `_Streams` 提供给解包流程，从而在 `<输出目录>\layout\` 下按**真实安装目录名**还原补丁后的文件布局。

```bat
msitool extract KB2565063.msp C:\out\patch
msitool extract KB2565063.msp C:\out\patch --base-msi C:\src\setup.msi
```

QFE / hotfix 型补丁（例如 KB2565063）通常只有 `QFE.cab`、没有 transform 流：此时 `--base-msi` 只能解出目标 MSI 本身的布局，补丁文件仍以 CAB 内部名放在 `payload\`。

### 打包选项（`pack`）

```bat
msitool pack <源目录> <输出.msi> [选项]
```

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `--name <文本>` | 源目录名 | ProductName，同时作为安装目录下的文件夹名 |
| `--version <x.y.z>` | `1.0.0` | ProductVersion |
| `--manufacturer <文本>` | `msitool` | Manufacturer |
| `--product-code <GUID>` | 随机 GUID | 固定 ProductCode（省略花括号会自动补全） |
| `--upgrade-code <GUID>` | 随机 GUID | 固定 UpgradeCode |
| `--platform <x64\|Intel>` | `Intel` | `x64` → `ProgramFiles64Folder` + 64 位组件；`Intel` → `ProgramFilesFolder` |
| `--language <LCID>` | `1033` | ProductLanguage 与 SummaryInformation Template |
| `--cab-size <n>[M\|G]` | `0` | 按未压缩字节把文件切到多个内嵌 CAB；`0` = 单 CAB。支持 `64K` / `16M` / `1G` 等后缀 |
| `--no-empty-dirs` | — | 不写 `CreateFolder` 表，空目录不会在安装时创建 |
| `--large-files <mode>` | `layout` | > 2 GiB 文件：`layout` 写成 MSI 同目录的未压缩伴随文件；`error` 直接拒绝 |
| `--ui <none\|basic\|minimal>` | `none` | 安装界面：无 UI / Windows Installer 基础进度条 / 自带的欢迎·进度·完成向导 |
| `--config <file>` | — | INI 清单：多 Feature、快捷方式、注册表、服务、环境变量、签名、默认值 |
| `--sign-cert <pfx>` | — | 提交后用 `signtool.exe` 写入 `\x05DigitalSignature` 流 |
| `--sign-password <text>` | — | PFX 密码 |
| `--sign-timestamp <url>` | — | RFC 3161 时间戳服务 |
| `--sign-tool <path>` | — | 显式指定 `signtool.exe` 路径 |
| `--force` | — | 覆盖已存在的输出 MSI |
| `--dry-run` | — | 只打印计划，不写 CAB / 数据库 |

示例：

```bat
msitool pack C:\payload C:\out\app.msi --name "My App" --version 1.2.3 --platform x64
```

```
Packing 7 file(s) in 4 directory(ies) -> E:\work\winmsi\app.msi
Product   : My App 1.2.3
Codes     : ProductCode={...} UpgradeCode={...}
Platform  : x64;1033
Cabinets  : 1 embedded CAB(s), 1 feature(s), 0 empty dir(s), 0 >2 GiB layout file(s)
Cabinet   : 1114953 bytes in 1 CAB(s) (MSZIP)
MSI       : 1138688 bytes
MsiVerifyPackage: OK (Windows Installer accepts this package)
Done      : E:\work\winmsi\app.msi
```

基础包含 9 张表：`Directory`、`Component`、`File`、`Media`、`Feature`、`FeatureComponents`、`Property`、`InstallExecuteSequence`、`AdminExecuteSequence`，以及完整 SummaryInformation；按需再加 `CreateFolder`、`Shortcut`、`Icon`、`Registry`、`ServiceInstall`、`ServiceControl`、`Environment`、`InstallUISequence` 与 UI 的 `Dialog` / `Control` / `ControlEvent` / `TextStyle`。因为带 `AdminExecuteSequence`，可以用不写注册表的方式让 Windows Installer 自己验证：

```bat
msiexec /a app.msi /qn TARGETDIR=C:\out\admin
```

### 打包进阶

#### 分卷、大文件与空目录

- `--cab-size 16`：按**未压缩**输入字节把文件切成多个内嵌 CAB（`cab1.cab`、`cab2.cab`…）。每个 CAB 一条 `Media` 行（`DiskId` / `LastSequence` / `#cabN.cab`），全部写进 `_Streams`；单个文件超过阈值时独占一个 CAB。省略时仍是单 CAB。`extract` 与 `msiexec /a` 都能按 `Media.LastSequence` 还原。
- 默认写 `CreateFolder` 表，配合已在序列里的 `CreateFolders` 标准动作，源目录里的**空目录**（含嵌套空目录）在安装时也会被创建；`--no-empty-dirs` 可关闭。
- `File.FileSize` 是 32 位整数，FCI 的 seek 回调也只能表示 32 位偏移，因此 > 2 GiB 的文件无法进 CAB。默认 `--large-files layout`：把它作为**未压缩 layout 文件**写到 MSI 同目录的 `<ProductName>\...` 伴随目录，写一条 `Cabinet` 为空的 `Media` 行和 `File.Attributes=8192`（Noncompressed），`FileSize` 记 0 并给出警告。`extract` 会从该伴随目录复制还原。这类包必须把伴随目录和 MSI 一起分发。`--large-files error` 恢复旧的「直接拒绝」行为。

#### UI

`--ui none`（默认，不建 `InstallUISequence`）/ `basic`（只加 `CostInitialize`、`FileCost`、`CostFinalize`、`ExecuteAction`，用 Windows Installer 自带进度条）/ `minimal`（再生成 `Dialog`、`Control`、`ControlEvent`、`TextStyle` 和欢迎 / 进度 / 完成 / 取消 / 错误五个对话框，以及 `DefaultUIFont` 属性）。无论哪种，包都能用 `/qn`、`/qb`、`/a` 安装。

#### 数字签名

`--sign-cert <pfx>` 在数据库提交后调用 `signtool.exe sign /fd SHA256` 写入 `\x05DigitalSignature` 流，可用 `--sign-password`、`--sign-timestamp`、`--sign-tool` 补充参数；签名后再次用 `MsiVerifyPackageW` 自检。签名是**可选**的：仅在打包时需要 Windows SDK 的 `signtool.exe`（自动从 `PATH` 或 `%ProgramFiles(x86)%\Windows Kits\10\bin\*\x64` 查找），运行时依旧只依赖系统 `msi.dll` / `cabinet.dll`。

#### manifest：`--config`

复杂资源用一个 INI 风格的 UTF-8 / UTF-16 清单描述，完整示例见 [examples/pack.ini](examples/pack.ini)。段名 `[类型:名称]`，键名不区分大小写；`;` / `#` 开头为注释；相对路径按源目录解析。**命令行的同名选项优先于 `[pack]` 段**。

| 段 | 常用键 | 生成的表 / 作用 |
| --- | --- | --- |
| `[pack]` | `cab-size`（MiB）、`ui`、`large-files`、`keep-empty-dirs`、`sign-cert` / `sign-password` / `sign-timestamp` / `sign-tool` | 填充命令行未指定的默认值 |
| `[feature:名称]` | `title`、`description`、`dirs`（`;` 分隔的相对目录前缀）、`level`、`display`、`attributes` | `Feature` + `FeatureComponents`；未匹配的目录归 `MainFeature` |
| `[shortcut:名称]` | `name`、`dir`（`ProgramMenuFolder` / `DesktopFolder` 等标准目录，或源内相对目录）、`target`（源内文件相对路径，自动转成 `[#FileKey]`）、`args`、`description`、`wkdir`、`icon`（.ico）、`component` | `Shortcut` + `Icon` |
| `[registry:名称]` | `root`（`HKLM` / `HKCU` / `HKCR` / `HKU` / `auto`）、`key`、`name`、`value`、`type`（`string` / `integer` / `binary` / `expand` / `multi` / `key` / `keydelete` / `keycreatedelete`）、`component` | `Registry` |
| `[service:名称]` | `name`、`displayname`、`image`（服务 exe 的源内相对路径，用于选组件）、`type`（`own` / `share` / `interactive`）、`start`（`auto` / `demand` / `disabled` / `system`）、`error`、`account`、`password`、`args`、`dependencies`、`description` | `ServiceInstall` + `ServiceControl` |
| `[environment:名称]` | `name`、`value`、`action`（`set` / `create` / `append` / `prepend` / `remove`）、`separator`、`system`、`component` | `Environment` |

`component` 可写组件键（`C1`）、目录键（`INSTALLFOLDER` / `D2`）、源内相对目录或文件相对路径；省略时资源挂到目标文件所属组件（无目标时挂到根组件）。标准动作 `CreateShortcuts`、`WriteRegistryValues`、`InstallServices`、`StartServices`、`WriteEnvironmentStrings`（以及卸载侧的 `RemoveShortcuts`、`RemoveRegistryValues`、`StopServices`、`DeleteServices`）已加入 `InstallExecuteSequence`。

```bat
msitool pack C:\payload C:\out\app.msi --config examples\pack.ini --cab-size 16 --ui minimal
```

### 退出码与输出约定

| 退出码 | 含义 |
| --- | --- |
| `0` | 成功（`verify` 判定 VALID） |
| `1` | 一般错误（`util::Error`）、包预检失败、或 `verify` 判定 INVALID |
| `2` | 命令行无法解析 |

- stdout 统一 **UTF-8**（工具已调用 `SetConsoleOutputCP(CP_UTF8)`），重定向到文件也是 UTF-8。
- 警告前缀 `[warn]`、详细日志前缀 `[v]` 输出到 **stderr**，不会污染 `--tsv` 数据。
- 表格长单元格会截断并显示 `…`；`verify` 失败时额外打印完整 `Reason:`。

### `export` 的输出结构

```
<outdir>\tables\<Table>.idt    每张持久表的文本归档（MsiDatabaseExportW）
<outdir>\streams\<name>        _Streams 里的所有原始流（内嵌 CAB 也在这里）
<outdir>\binary\<name>         Binary 表条目（图标、自定义操作 DLL 等）
<outdir>\summary.txt           SummaryInformation + Property 表（UTF-8）
```

> `MsiDatabaseExportW` 导出含二进制列的表（如 `Binary`）时，会在同目录额外生成同名子目录存放流，这是 MSI API 的正常行为。

---

## 工作原理

**包预检（所有命令）**

1. `probe()`：文件存在、非目录、大小 ≥ 512 字节、前 8 字节为 OLE 复合文件签名 `D0 CF 11 E0 A1 B1 1A E1`。
2. `MsiOpenDatabaseW`：`.msi` 用 `MSIDBOPEN_READONLY`；`.msp` 用 `MSIDBOPEN_PATCHFILE` 伪指针（msi.dll 实际要求值 32，SDK 宏在 Unicode 下算出 16，代码两种都试）。
3. 打开后确认 `_Tables` 虚拟表可查询，排除「有 CFB 头但不是 MSI 存储」的文件。
4. `verify` 额外调用 `MsiVerifyPackageW`（仅 `.msi`），并把结果作为诊断项输出。

**查看侧（Msi API，只读）**

`MsiDatabaseOpenViewW` + `MsiViewExecute` + `MsiViewFetch` 取行，`MsiViewGetColumnInfo(MSICOLINFO_NAMES/TYPES)` 拿列名/类型，`MsiRecordGetStringW` / `MsiRecordGetInteger` / `MsiRecordDataSize` 渲染单元格；`MsiGetSummaryInformationW` + `MsiSummaryInfoGetPropertyW` 读摘要；`MsiDatabaseExportW` 导出 `.idt`；`MsiRecordReadStream` 读二进制流。

MSI SQL 是 SQL 的**子集**（例如没有 `COUNT(*)` 这类聚合函数），所以 `tables` 的行数通过 `MsiViewFetch` 逐行计数得到。

**解包侧**

1. 读 `Media` 表：`Cabinet` 以 `#` 开头表示 `_Streams` 里的内嵌 CAB；否则是外部 CAB（按 MSI 同目录查找）；为空则是未压缩 layout。
2. 按 `Media.LastSequence` 升序划分 `File.Sequence` 区间，确定每个文件属于哪个 CAB。
3. 由 `Directory` / `Component.Directory_` / `File.FileName` 还原相对目录树；`TARGETDIR` 映射到输出根。`DefaultDir` 采用 MSI 的 **`target:source`** 双路径语法：冒号左边是安装目录、右边是未压缩源镜像里的目录，每侧还可写成 `short|long`，`.` 表示与父目录相同。
4. 所有 CAB 先暂存到短临时目录（规避 FDI 的 256 字节路径限制），用 `FDIIsCabinet` 读头信息把连成一套的分卷 CAB 分组；每组跑一次 `FDICopy`（`cabinet.dll` 的 FDI）。`fdintCOPY_FILE` 回调按 CAB 内文件名查表，直接用 `CreateFileW` 打开最终路径；未匹配的条目落到 `_unmapped\`，不会丢文件。`fdintNEXT_CABINET` 回调把分卷集的下一个 CAB 交回 FDI，跨 CAB 的文件保持**同一个打开的目标句柄**，因此不会被截断或重复写入。
5. 未被 CAB 覆盖的文件（未压缩 layout / 外置文件）从 MSI 同目录的源树复制，仍找不到才给出警告。
6. `--transform` 用 `MsiDatabaseApplyTransformW` 在只读句柄上叠加临时视图；`.msp` 走单独分支：分类 `_Streams` 里的 CAB / transform / 原始流，FDI 解出 `payload\`，可选 `--base-msi` 把补丁 transform 应用到目标 MSI 后按真实目录名布局。

**打包侧**

1. 递归枚举源目录：每个目录一行 `Directory` + 一个 `Component`（含空目录），每个文件一行 `File`；`File` 主键（`F1`、`F2`…）同时是 CAB 内文件名。> 2 GiB 的文件移到序列末尾，标记为 uncompressed layout。
2. 按 `--cab-size` 把文件分组（未压缩字节数），每组跑一次独立的 **FCI**（`FCICreate` / `FCIAddFile` / `FCIFlushCabinet`）生成 MSZIP CAB；源路径以 UTF-8 交给 FCI 回调再转回 UTF-16。
3. `MsiOpenDatabaseW(MSIDBOPEN_CREATE)` 建表（基础 9 张 + 按需的资源表 / UI 表；MSI SQL 的 `PRIMARY KEY <列名>` 必须附在**最后一个列定义之后**），参数化 `INSERT ... VALUES (?, ?)` 写行：`Media` 每个 CAB 一行，空目录写 `CreateFolder`，清单里的资源写 `Shortcut` / `Registry` / `ServiceInstall` / `Environment` / `Feature` 等。
4. `MsiRecordSetStreamW` + `MSIMODIFY_INSERT` 把每个 CAB 写入 `_Streams`（`#cabN.cab`），图标同理写入 `Icon`；> 2 GiB 的文件复制到 MSI 同目录的伴随 layout 树。
5. `MsiSummaryInfoSetPropertyW` → `MsiSummaryInfoPersist` → `MsiDatabaseCommit`；可选 `signtool.exe` 签名；最后 `MsiVerifyPackageW` 自检。

所有句柄用 RAII 封装，文件 I/O 使用 Win32 W 版 API，不依赖 CRT 的 ANSI 路径。

---

## 项目结构

```
.
├── .github/
│   ├── ISSUE_TEMPLATE/          bug / feature 模板
│   └── PULL_REQUEST_TEMPLATE.md PR 模板
├── src/
│   ├── util.h / util.cpp        Unicode/UTF-8 输出、表格打印、路径与错误处理
│   ├── msidb.h / msidb.cpp      Msi API RAII 封装：包预检、查询、列信息、流、摘要、导出
│   ├── cab.h / cab.cpp          FDI 解压，回调式直接落盘
│   ├── extract.h / extract.cpp  表解析、目录树、Media/Sequence 映射、CAB 导出与解包
│   ├── pack.h / pack.cpp        FCI 多 CAB 压缩 + manifest + Msi 建库/建表/写流/签名/摘要
│   ├── ui.h                     UI 层接口：主题、GDI 绘制器、控件声明
│   ├── ui_draw.cpp              主题 / DPI / 双缓冲绘制 / 矢量图标
│   ├── ui_widgets.cpp           自绘控件：按钮、输入框、下拉、网格、日志、进度、导航
│   ├── ui_app.cpp               主窗口、九个页面、业务逻辑与后台任务
│   └── main.cpp                 入口：无参数进 GUI，带参数进 CLI（全部命令）
├── examples/
│   └── pack.ini                 打包清单示例（features/shortcuts/registry/services/env）
├── build.bat                    MSVC 构建脚本（x64 / x86 / clean）
├── CMakeLists.txt               CMake 构建（MSVC）
├── CHANGELOG.md                 变更记录
├── CODE_OF_CONDUCT.md           行为准则
├── CONTRIBUTING.md              贡献指南
├── SECURITY.md                  安全策略
├── LICENSE                      MIT
└── README.md
```

---

## 构建详解

### build.bat

```bat
build.bat            :: x64
build.bat x86        :: x86
build.bat clean      :: 清理
```

核心命令等价于：

```bat
cl /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE ^
   /Isrc /Fobuild\obj\x64\ /Febuild\msitool_x64.exe ^
   src\util.cpp src\msidb.cpp src\cab.cpp src\extract.cpp src\pack.cpp src\main.cpp ^
   src\ui_draw.cpp src\ui_widgets.cpp src\ui_app.cpp ^
   Msi.Lib Cabinet.Lib Shell32.Lib Ole32.Lib User32.Lib Gdi32.Lib Advapi32.Lib ^
   Comdlg32.Lib Uuid.Lib /link /MACHINE:X64 /SUBSYSTEM:WINDOWS
```

> `/SUBSYSTEM:WINDOWS` 让双击 exe 时没有控制台黑框；CLI 模式会 `AttachConsole(ATTACH_PARENT_PROCESS)`（失败则 `AllocConsole`）并重绑标准流，重定向到文件时也不会破坏管道。

| 库 | 用途 |
| --- | --- |
| `Msi.Lib` | `MsiOpenDatabaseW` / `MsiDatabaseOpenViewW` / `MsiRecordReadStream` / … |
| `Cabinet.Lib` | FDI（`FDICopy`）解压 + FCI（`FCIAddFile`）压缩 |
| `Shell32.Lib` / `Ole32.Lib` | `CommandLineToArgvW` / `IFileDialog` / `OleInitialize` / GUID |
| `User32.Lib` / `Gdi32.Lib` | 自绘窗口、消息循环、DPI 与全部 GDI 绘制 |
| `Advapi32.Lib` / `Comdlg32.Lib` / `Uuid.Lib` | 注册表设置 / 文件过滤器 / COM 接口 GUID |

### CMake

标准 VS 安装：

```bat
cmake -S . -B build-cmake -G "Visual Studio 17 2022" -A x64
cmake --build build-cmake --config Release
```

若 VS 是绿色/非标准安装（CMake 找不到实例），在 **Native Tools Command Prompt** 里用 NMake：

```bat
cmake -S . -B build-cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
```

构建目标是 `/W4` **零警告**；新增代码请保持这一点。

---

## 测试与验证

项目目前没有自动化单元测试框架（见 [TODO](#todo)），验证以**真实 MSI 样本 + 往返哈希**为准。提交 PR 前建议至少跑一遍：

```bat
:: 1) 查看侧
build\msitool_x64.exe verify path\to\sample.msi
build\msitool_x64.exe tables path\to\sample.msi
build\msitool_x64.exe list path\to\sample.msi streams

:: 2) 解包侧
build\msitool_x64.exe extract path\to\sample.msi C:\out\files

:: 3) 打包 + 往返（对比 sha256 / 文件大小）
build\msitool_x64.exe pack C:\payload C:\out\app.msi --force
build\msitool_x64.exe verify C:\out\app.msi
build\msitool_x64.exe extract C:\out\app.msi C:\out\roundtrip

:: 4) 进阶能力（分卷 / 清单 / UI / 签名）
build\msitool_x64.exe pack C:\payload C:\out\split.msi --cab-size 64K --force
build\msitool_x64.exe list C:\out\split.msi media --tsv
build\msitool_x64.exe pack C:\payload C:\out\full.msi --config examples\pack.ini --ui minimal --force
build\msitool_x64.exe tables C:\out\full.msi
build\msitool_x64.exe pack C:\payload C:\out\signed.msi --sign-cert C:\cert.pfx --force
build\msitool_x64.exe list C:\out\signed.msi streams

:: 5) 解包进阶（transform 视图 / .msp / 分卷集）
build\msitool_x64.exe list setup.msi properties --transform custom.mst
build\msitool_x64.exe extract setup.msi C:\out\files --transform custom.mst
build\msitool_x64.exe extract update.msp C:\out\patch
build\msitool_x64.exe extract update.msp C:\out\patch --base-msi setup.msi
```

还可以让 Windows Installer 自己验证生成的包（administrative install 不写注册表）：

```bat
msiexec /a C:\out\app.msi /qn TARGETDIR=C:\out\admin
```

负向用例也应覆盖：非 MSI 文件、目录、伪造 CFB 头、缺失外部 CAB、中文/空格路径、`--dry-run`、`--force`。详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

---

## 实测过的样本

| 包 | 形态 | 结果 |
| --- | --- | --- |
| GitHub CLI 2.89.0 MSI | 1 个内嵌 CAB / 1 文件 | 还原 `Program Files\GitHub CLI\gh.exe`，40,592,888 字节与 `File.FileSize` 一致 |
| VC++ Runtime Minimum x64 | 外部 `cab1.cab` / 12 文件 | 12 个 DLL 全部还原到 `System64\` |
| X64 Debuggers And Tools (Windows SDK) | 18 个内嵌 CAB / 473 文件 | 473 文件、约 84 MiB，0 警告 |
| Windows Installer 缓存 MSI | 1 个内嵌 CAB / 146 文件 | 146 文件、约 21 MiB |
| VS Setup TestMsi | 无 Media 表 | 0 文件，正常退出 |
| NetLimiter 5.3.28 x64 | 未压缩 layout，`target:source` 语法 / 178 文件 | 178 文件全部从源目录树还原，0 警告 |
| KB2565063 `.msp` | 补丁包 | `verify` 判定 VALID（patch 模式），可读取 `MsiPatchMetadata` 等表 |
| `pack` 往返（7 文件：中文名 / 空格 / 空文件 / 嵌套 / 1 MiB 随机数据） | 自包含 MSI | `MsiVerifyPackage` OK；`extract` 往返 sha256 完全一致；`ACTION=ADMIN` 让 Windows Installer 自身也解出全部文件 |
| `pack --cab-size 64K`（5 文件） | 3 个内嵌 CAB | 3 条 `Media` 行 + 3 个 `_Streams`；`extract` 全量还原；`msiexec /a` 也解出全部文件 |
| `pack --config` 全量清单 | 21 张表 | `Feature` 3 / `Shortcut` 2 + `Icon` 1 / `Registry` 2 / `ServiceInstall`+`ServiceControl` / `Environment` / 5 个 `Dialog`；`MsiVerifyPackage` OK，`msiexec /a` 成功 |
| `pack --ui minimal` | 自包含向导 | 欢迎 / 进度 / 完成 / 取消 / 错误对话框；`msiexec /i /qb` 安装成功 |
| `pack --sign-cert`（自签名 PFX） | 已签名 MSI | `\x05DigitalSignature` + `\x05MsiDigitalSignatureEx` 流写入，`MsiVerifyPackage` 仍 OK |
| `pack`（2.3 GiB 稀疏文件） | 未压缩伴随 layout | `FileSize=0`、`Attributes=8192`、一条空 `Cabinet` 的 `Media` 行；`extract` 从伴随目录完整还原 2,300,000,000 字节 |
| 纯空目录（0 文件 / 3 目录） | 仅 `CreateFolder` 表 | `MsiVerifyPackage` OK；`msiexec /i` 会真正创建空目录树 |
| `makecab` 分卷集（400 KB 文件跨 `span1/2/3.cab`） | 3 条相同 `LastSequence` 的 `Media` 行 | `fdintNEXT_CABINET` 串起 3 个 CAB，`extract` 还原出 400,000 字节，sha256 与源文件完全一致 |
| Autodesk `acadps.mst` + 自生成 MSI | `--transform` 临时视图 | `Property` 表由 8 行变 14 行（新增 6 个 `ODIS_*` 属性），`extract` / `verify` 均正常 |
| KB2565063 `.msp`（VS2010 QFE） | `QFE.cab` + `MsiPatchMetadata` | 解出 18 个补丁负载文件 + `manifest.txt` + 原始流；`--base-msi` 另解出目标 MSI 布局 |
| 文本 / EXE / 伪造 CFB 头 / 目录 / 不存在路径 | 非 MSI | 预检拒绝，`verify` 输出 INVALID + 完整原因，退出码 1 |

---

## TODO

约定：`- [x]` 是已完成项（同时用删除线划掉），`- [ ]` 是待办。搞定一项就把 `[ ]` 改成 `[x]`、给文字加上 `~~`，并同步更新 [CHANGELOG.md](CHANGELOG.md)。

- [x] ~~包预检与 `verify`：OLE 签名、`_Tables` 结构、`MsiVerifyPackageW`~~
- [x] ~~查看命令：`info` / `tables` / `schema` / `query` / `list` / `cabs` / `export`~~
- [x] ~~解包：内嵌 CAB、外部 CAB、未压缩 layout、`target:source` 目录语法~~
- [x] ~~打包：FCI + Msi 建库，产出通过 `MsiVerifyPackageW` 的自包含 MSI~~
- [x] ~~MSVC x64/x86 构建脚本与 CMake 构建~~
- [x] ~~开源项目文档：README / LICENSE / CONTRIBUTING / CHANGELOG / SECURITY / 行为准则 / issue 与 PR 模板~~

- [x] ~~`pack`：多 CAB 分卷、> 2 GiB 文件、`CreateFolders` 保留空目录~~
- [x] ~~`pack`：快捷方式、注册表、服务、环境变量、UI、数字签名、多个 Feature~~
- [x] ~~`extract`：跨 CAB 连续文件（FDI `fdintNEXT_CABINET`）、应用 transform 后的视图、`.msp` 解包~~
- [ ] 工程：自动化单元测试 + GitHub Actions CI（MSVC x64/x86）

欢迎在 issue 中讨论或认领（见 [CONTRIBUTING.md](CONTRIBUTING.md)）。

---

## 已知限制

**解包 / 查看**

- **不执行安装**：不解析条件、不执行 CustomAction、不写注册表；`--transform` 只叠加只读的临时视图，不落盘、不修改原包。
- 跨 CAB 连续文件依赖完整的**分卷集**：某个 CAB 缺失时 `extract` 会给出 `CAB set requested cabinet ... which is not staged` / `FDICopy ... aborted` 警告并跳过该分卷集；从分卷集中间的 CAB 开始解包则报 `partial file`，被截断的文件无法还原。
- 外部 CAB 必须在 MSI 同目录（或 `Media.Cabinet` 指定的相对路径）；内嵌 CAB 无此要求。
- `extract` 需要一块与 CAB 总大小相当的临时磁盘空间，结束后自动清理（`--keep-temp` 可保留）。
- 目录还原是**相对布局**：`TARGETDIR` 作为输出根，标准目录属性展开为同名目录，不映射到真实的 `C:\Program Files`。
- 未压缩 administrative image 的源目录结构（source 侧）常与安装布局不同，需保留源结构时用 `--source-layout`。
- 不校验数字签名，不处理加密/受口令保护的包。`.msp` 可以导出补丁 CAB 负载 / transform / 原始流；QFE 型补丁没有 transform 流时，只能用 CAB 内部名（`F_CENTRAL_*` 之类）命名 `payload\` 里的文件，需要真实目录名时必须提供 `--base-msi`（且补丁需带 transform）。
- MSI SQL 是子集：`query` 只支持受限的 `SELECT` 语法，聚合函数等常见 SQL 特性不可用。

**打包**

- 基础形态是单 Feature、单 CAB 的最小包；快捷方式 / 注册表 / 服务 / 环境变量 / 多 Feature / UI / 签名都通过 `--config` 或对应选项按需生成，不提供自定义动作与自定义 UI 皮肤（`--ui minimal` 是内置的固定向导）。
- 分卷按**未压缩**字节估算，压缩后的 CAB 可能小于或略大于阈值；不生成 CAB 之间的连续文件（一个文件不会跨 CAB 切分）。
- > 2 GiB 的文件不能进 CAB（`File.FileSize` 是 32 位整数、FCI 的 seek 也是 32 位）：默认以未压缩伴随文件的方式保留，`FileSize` 记 0，安装 / 还原时必须同时分发伴随目录。
- 数字签名是可选的，需要 Windows SDK 的 `signtool.exe` 与一张代码签名证书；不提供证书管理。
- 文件名 / 目录名中的 `\ / : * ? " < > |` 与控制字符会替换为 `_`，结尾的点与空格会被去掉。
- 需要一块与所有 CAB 总大小相当的临时磁盘空间；输出已存在时默认拒绝，需 `--force`。

**图形界面**

- GUI 是 CLI 的前端：所有读写仍走同一套 `msi::Db` / `extract` / `pack` 实现，没有额外的数据库或包格式解析。
- 界面自绘，不依赖视觉样式清单（manifest）：系统原生 `EDIT` 的占位文字由 UI 层自己绘制；Win11 圆角 / 深色边框通过动态加载 `dwmapi.dll` 设置，旧版 Windows 上自动降级为普通直角窗口。
- 后台任务（解包 / 导出 / 打包）运行期间禁止切换页面和关闭窗口；GUI 不提供任务中途取消（CLI 本身也没有中断点）。
- `Msi` 句柄不跨线程共享：启动任务前关闭主界面持有的数据库，工作线程自己重新打开只读句柄。

---

## 故障排除 FAQ

| 现象 | 原因 / 处理 |
| --- | --- |
| `tables` 里某些表的 `Rows` 是 `?` | 虚拟表无法枚举行数；属正常现象，用 `list <表名>` 查看内容 |
| `extract` 显示 `0 cabinet(s)`，但文件都复制出来了 | 这是未压缩 administrative layout（`Media.Cabinet` 为空），文件来自 MSI 同目录的源树 |
| 大量 `No source data for ...` | 外部 CAB 或源文件缺失；用 `info` 看 Media 状态是否为 `MISSING`，把 CAB / 源目录放到 MSI 同目录 |
| 目标路径出现 `._x64` 这类怪目录 | 旧版本的 `DefaultDir` 解析问题，已在 `target:source` 修复中解决；请重新构建 |
| `query` 报 `SQL 查询语法不正确或不被支持` (1615) | MSI SQL 是子集，不支持聚合函数等常见语法；改用 `list` 或简单 `SELECT` |
| `export` 报 `指定的路径无效` (161) | 旧版本在正斜杠路径下的问题，已通过入口路径规范化修复 |
| 中文显示乱码 | 输出是 UTF-8；终端可执行 `chcp 65001`，或用支持 UTF-8 的编辑器打开重定向文件 |
| `verify` 里 `MsiVerifyPackage` 显示 `reported ...` | 这是附加诊断，不影响 VALID 结论；某些缓存/剥离的 MSI 不是可安装包 |
| `pack` 提示输出已存在 | 加 `--force` 覆盖，或换一个输出路径 |
| `pack` 警告 `file(s) exceed 2 GiB` 并多出一个目录 | 大文件以未压缩伴随文件保存；分发 MSI 时必须一并带上 `<ProductName>\` 目录，否则安装 / `extract` 找不到源文件 |
| `--sign-cert` 报 `Cannot find signtool.exe` | 安装 Windows SDK，或用 `--sign-tool <signtool.exe 完整路径>` 指定 |
| 安装后看不到源里的空目录 | 检查是否用了 `--no-empty-dirs`；正常包会写 `CreateFolder` 表并由 `CreateFolders` 动作创建 |
| `--config` 报 `Unknown section` | 段名只能是 `pack` / `feature:*` / `shortcut:*` / `registry:*` / `service:*` / `environment:*` |
| `extract` 报 `FDICopy ... wrong cabinet in set` | 分卷集里某个 CAB 缺失或顺序不对；确认 MSI 同目录外部 CAB 齐全，或用 `cabs` 导出后检查 CAB 头的 `setID` / `iCabinet` 是否连续 |
| `extract` 报 `partial file` | 从分卷集中间的 CAB 开始解包；必须提供完整 CAB 集（从 `hasprev=0` 的第一个 CAB 开始） |
| `--transform` 报 `MsiDatabaseApplyTransformW` 失败 | 检查 MST 路径、是否属于该产品的 transform，以及 transform 与目标 MSI 的 ProductCode / 语言是否匹配 |
| `extract *.msp` 的 `payload\` 文件名是 `F_CENTRAL_*` | QFE 补丁 CAB 里只有 File 表键；加 `--base-msi` 且补丁带 transform 时才能还原真实目录名 |

---

## 安全

`msitool` 只读打开 MSI、不执行安装、不运行 CAB 内的代码、不写注册表。但它会用系统库解析**第三方提供的** OLE 复合文档与 CAB 数据，恶意构造的输入仍可能导致解析库异常或写出意外路径。建议：

- 处理不受信任的样本时在隔离环境/普通权限账户下进行；
- `extract` 的目标目录选专用输出目录，工具已对路径组件做净化（去除 `..`、非法字符）；
- `query` 为只读，`pack` 生成物不要伪装成他人软件分发。

漏洞报告流程与支持的版本见 [SECURITY.md](SECURITY.md)，请**不要**直接在公开 issue 中提交安全问题。

---

## 贡献

欢迎 issue、文档改进、样本回归与 PR。参与讨论请遵守 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。提交前请先阅读 [CONTRIBUTING.md](CONTRIBUTING.md)，其中包含：

- 开发环境与构建方式
- 代码风格（C++17、RAII、W 版 API、UTF-8 输出、`/W4` 零警告）
- 测试清单与提交信息约定
- 报告 bug 时需要提供的信息

---

## 许可证

[MIT](LICENSE) © 2026 msitool contributors

本项目使用 Windows SDK 提供的头文件与导入库（`Msi.Lib`、`Cabinet.Lib` 等），运行时依赖操作系统自带的 `msi.dll` / `cabinet.dll`；这些组件归 Microsoft 所有，不受本项目许可证约束。

---

## 致谢与参考

- Microsoft **Windows Installer** SDK：`msi.h` / `msiquery.h`，以及 Windows Installer 文档（<https://learn.microsoft.com/windows/win32/msi/>）
- Microsoft **Cabinet SDK**：`fdi.h`（FDI 解压）与 `fci.h`（FCI 压缩）
- MSVC / Windows SDK 工具链
- 用于回归测试的真实安装包：GitHub CLI、VC++ Runtime、Windows SDK Debuggers、NetLimiter、Windows Installer 缓存包
