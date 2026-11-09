# msitool

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C)
![MSVC](https://img.shields.io/badge/MSVC-2017%2B-purple)
![Windows](https://img.shields.io/badge/platform-Windows-blue)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

> 用 **MSVC + C++17 + Windows Installer (Msi) API** 编写的工具，用来**查看、解包、打包** MSI 安装包；带一个纯 Win32 自绘的 Fluent 风格图形界面，同时完整保留命令行。

`msitool` 是一个零外部依赖的单文件程序：运行时只使用 Windows 自带的 `msi.dll`、`cabinet.dll` 与 `msxml6.dll`，不需要 Orca、7-Zip、Python、.NET 或任何 UI 框架，不需要管理员权限，也**不会真正安装任何东西**。它直接读取 MSI 的 OLE 复合存储与表，用 FDI/FCI 处理 CAB，适合逆向分析、离线审计、CI 中的包检查，以及把目录快速做成一个可安装的 MSI。双击 exe 打开图形界面，在终端里带参数运行则仍是原来的 CLI。

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
  - [安装效果导出（`effects`）](#安装效果导出effects)
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
| 安装效果 | `effects` 把注册表 / 服务 / 环境变量 / 快捷方式 / 自定义动作翻译成 `.reg`、`.cmd`、`.ps1` 脚本和一份可回填 `pack --config` 的 `pack.xml`；命令行与 GUI 均可调用 |
| 解包 | 还原安装目录树；内嵌 CAB（`_Streams`）、外部 CAB、未压缩 layout、跨 CAB 连续文件（FDI 分卷集）、`.msp` 补丁负载都支持；`--transform` 可查看应用 transform 后的视图；GUI 另提供资源管理器式浏览（目录树 + 文件列表、筛选、右键菜单、双击单文件解包、目录递归解包、自动输出目录） |
| 打包 | 把目录做成自包含 MSI：FCI 生成一个或多个 MSZIP CAB + Msi API 建库/写流/写摘要，可选安装位置（`--install-dir`）、分卷、空目录、快捷方式（桌面 / 公共桌面 / 开始菜单子目录 / 图标索引 / 广告式）、注册表、服务、环境变量、多 Feature、**XML 清单**、**内置中英多页向导**、**UI 模板导入**、自定义 UI 与数字签名 |
| Transform | `transform` 用 `MsiDatabaseGenerateTransformW` + `MsiCreateTransformSummaryInfoW` 把两个 MSI 的差异写成真正的 `.mst`（可指定 error / validation 标志，`--dry-run` 只做逐表 diff）；`apply` 复制原包后在事务句柄上 `MsiDatabaseApplyTransformW` + `MsiDatabaseCommit` 落地新包；`mst` 读取摘要流与 `_TransformView` 变更明细；`--transform` 继续支持把 MST 作为只读视图查看 / 解包 |
| 输出 | UTF-8 文本表格或 `--tsv`；控制字符转义为 `\xNN`；警告与详细日志走 stderr |
| 图形界面 | 纯 Win32 API（user32 / gdi32 / shell32 / ole32）自绘的 Fluent 界面：亮/暗主题、高 DPI（Per-Monitor V2，实测 75%–300%，跨显示器热切换会重排布局与列宽）、导航、卡片、数据网格、SQL 查询、解包/导出/打包表单、实时日志与进度；**不使用任何第三方库或 UI 框架** |

---

## 快速开始

### 环境要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows（需系统自带 `msi.dll` / `cabinet.dll` / `msxml6.dll`）；在 Windows 10/11 上完成验证 |
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

也可以直接把 `.msi` / `.msp` 拖进窗口，或在「打包」页把文件 / 文件夹拖到具体字段上（见 [拖放](#拖放)）。GUI 与 CLI 是同一个 exe：带命令参数时在终端里以 CLI 运行（自动附加父控制台），不带参数时是无控制台的窗口程序。

---

## 图形界面（Fluent UI）

### 设计

整个 UI 层在 `src/ui_*.cpp` 中，**只调用 Windows API**：

- 窗口、消息循环、拖拽、高 DPI（`SetProcessDpiAwarenessContext` / `GetDpiForWindow`）、系统文件对话框（`IFileDialog`）、剪贴板、注册表全部用 user32 / shell32 / ole32 / advapi32；
- 所有像素由 gdi32 绘制到内存 DC 双缓冲：圆角卡片、渐变 Hero、导航选中态、数据网格、切换开关、进度条、日志、Toast、以及 30 多个用 `MoveToEx`/`LineTo`/`RoundRect`/`Ellipse` 画的矢量图标；
- 每个控件都是一个自绘 `HWND`（共用一套窗口过程 + C++ 对象路由），只有文本输入使用系统原生 `EDIT` 子窗口以获得输入法、光标与选字能力，边框、占位文字、焦点强调线仍由 GDI 绘制；
- Toast 通知是独立置顶的无焦点浮层（`WS_POPUP`），不会被内容区控件或布局重绘遮挡，随窗口尺寸自动定位；
- 布局全部按当前客户区尺寸计算：列宽、行距、控件高度、日志高度随窗口缩放 / 最大化实时自适应；打包表单拆成 TabView 分页，小窗口下也不会字段堆叠；解包页隐去全局「当前包文件」栏与页面大标题，整页就是文件浏览器，选项与日志默认收起；打包页同样不显示「当前包文件」（打包只产出包，不读取包）；
- 无边框标题栏通过 `WM_NCCALCSIZE` / `WM_NCHITTEST` 自绘，并动态加载 `dwmapi.dll` 设置 Win11 圆角与深色边框；
- 文件拖放用 ole32 的 `RegisterDragDrop` + `IDropTarget`（不再只用 `WM_DROPFILES`），因此在拖动的过程中就能拿到光标位置：当前页面 / 标签页的可用字段会亮起，光标所在的字段决定落点，窗口空白处则按拖放内容自动分配；`WM_DROPFILES` 仍保留为旧式拖放的兜底路径；
- 亮/暗/跟随系统三套主题，主题设置与最近打开的包保存在 `HKCU\Software\msitool`；所有尺寸按 DPI 缩放；
- 耗时的 `extract` / `export` / `pack` 在工作线程执行，通过 `PostMessage` 把日志与完成状态送回 UI 线程；`util::outLine` / `errLine` 新增可选日志钩子，让现有 CLI 代码的输出直接变成 GUI 日志。

### 页面

| 页面 | 功能 |
| --- | --- |
| 概览 | 包摘要、快捷操作、渐变 Hero |
| 包信息 | 概览头部（产品名 / 版本 / 大小 / 表数 / CAB / 流）+「摘要 / 属性 / 媒体」分区，全宽表格、关键字筛选、选中行详情与 TSV 复制；属性区展示 Property 表全部键值 |
| 数据表 | 表筛选 + 任意表的数据网格（虚拟滚动、选中、Ctrl+C 复制 TSV） |
| SQL 查询 | MSI SQL 编辑器、示例查询、结果网格与耗时 |
| 解包 | **整页是文件资源管理器**：命令 / 地址栏（后退、前进、上一级、刷新、可点击面包屑、搜索框、解包全部、解包选中、选项、日志）+ 左侧导航树 + 右侧详细信息列表（文件夹与文件混排、图标、类型列、点击表头排序、双击进入目录 / 双击文件解包、右键菜单）；常驻「解包到」位置条可直接编辑路径、点「浏览…」选目录、从「最近」下拉复用历史目录、点「打开」查看，默认自动填 `<包名>_files`；六个开关、MST transform 与基准 MSI 收进可折叠「选项」，运行日志收进可折叠「日志」抽屉（展开后按窗口高度自适应，约占页面 1/3，最大化时更高） |
| 导出 | CAB / 完整归档两种模式、运行日志 |
| 安装效果 | 三种导出内容（两者 / 仅脚本 / 仅 XML 清单）、输出目录（留空自动填 `<包名>_effects`，可拖放）、「导出安装效果」按钮与运行日志；结果文件列表直接回显在日志里 |
| 打包 | 「基本 / 高级 / 签名 / 快捷方式 / 自定义动作」五个标签页，字段按窗口尺寸自适应两列排布（基本页含源目录、输出 MSI、产品名称、产品版本、安装目录；高级页含 CAB 分卷、XML 清单、UI 模板、UI 语言与安装范围；快捷方式页含主程序、桌面 / 开始菜单勾选与可增删行的内联快捷方式表格——名称 / 位置 / 目标 / 参数 / 图标 / 索引 / 说明；自定义动作页为可视化编辑器——动作列表 + 类型感知属性表单 + 标志位 + 执行序列时间线 + 实时检查 + XML 导入 / 导出，详见 [自定义动作](#自定义动作customactions)）；右上角「开始打包」常驻，完成后自动打开新 MSI；文件 / 文件夹可直接拖入（见 [拖放](#拖放)） |
| Transform | 原包 / 更新包 / `.mst` / 输出包四个路径 + `--errors` / `--validate` 标志输入，三个动作：生成 MST（逐表 diff 统计）、应用 MST（复制 + 提交成新包）、查看变更（摘要流 + `_TransformView` 明细）；结果与进度走运行日志 |
| 验证 | 签名 / `MsiOpenDatabase` / `MsiVerifyPackage` / `_Tables` / 媒体检查报告 |
| 设置 | 主题、DPI 信息、可自定义快捷键（点击按键框重新录制、Backspace 清除、一键恢复默认）与关于（版本 / 协议 / 可点击项目主页 / 构建信息） |

### 拖放

拖文件 / 文件夹进窗口时，**落点决定字段**：光标经过的输入框会亮起，右下角提示「松开：设为…」。落在字段以外的地方时按内容走，并且**解包与打包严格分开**：文件夹永远属于打包（自动切到「打包」页并设为源目录），`.msi` / `.msp` 永远属于查看 / 解包；「打包」页不会打开安装包，也不会显示当前包。

| 落点 | 拖文件夹 | 拖文件 |
| --- | --- | --- |
| 打包 · 源目录 | 直接作为源目录 | 取所在目录（打包以目录为单位，会包含其中全部内容） |
| 打包 · 输出 MSI | 在该目录下按产品名生成 `<产品名>.msi` | `.msi` 直接作为输出路径 |
| 打包 · 配置文件 | 查找目录里的 `pack.xml` / `msitool.xml` / `manifest.xml` | 作为 XML 清单文件；编辑器未编辑过时自动载入其中的自定义动作 |
| 打包 · UI 模板 | — | `.msi` 作为 UI 模板 |
| 打包 · 签名证书 | — | `.pfx` / `.p12` |
| 解包 · 解包到 / 导出 · 输出目录 / 安装效果 · 输出目录 | 作为输出目录（同时记入「最近」） | 取所在目录 |
| 打包页空白处 | 源目录 | `.xml` → 清单，`.pfx` → 证书，其余 → 源目录（取所在目录）；`.msi` / `.msp` 不打开，只提示去查看 / 解包页 |
| 其他页面空白处 | 作为打包源目录并切到「打包」页 | `.xml` → 清单，`.pfx` → 证书，`.msi` / `.msp` → 打开，其余 → 源目录（取所在目录） |

解包页与导出页的输出目录投放区是该行的「解包到」/「输出目录」字段，拖到页面其它空白处同样按上面的规则进入打包流程。多项目同时拖放时取它们的**最深共同目录**作为源目录；若两项不在同一磁盘则拒绝并提示。拖放只写入字段并记一行日志，不会自动开始打包，也不会移动 / 删除源文件（`IDropTarget` 只报告 `DROPEFFECT_COPY`）。

### 快捷键

上表前四项可在「设置 → 快捷键」中点击右侧按键框重新录制（要求 Ctrl / Alt 组合或功能键，Backspace 清除、Esc 取消），冲突、系统保留组合（`Alt+F4`、`Ctrl+C` 等）会被拒绝；「恢复默认」一键还原。配置保存在 `HKCU\Software\msitool\Key*`，启动时重建加速键表。

| 按键 | 作用 | 可自定义 |
| --- | --- | --- |
| `Ctrl+O` | 打开包文件 | 是 |
| `F5` | 重新载入当前包 | 是 |
| `Ctrl+,` | 打开设置 | 是 |
| `Ctrl+Shift+T` | 切换明暗主题 | 是 |
| `Ctrl+C` | 复制数据网格 / 日志内容 | 否 |
| `Ctrl+Enter` | 运行 SQL 查询 | 否 |
| `Tab` / `Shift+Tab` | 在控件之间切换焦点 | 否 |
| `Esc` | 关闭下拉菜单 / 清除提示 | 否 |

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
| `effects` | `<包> <输出目录>` | 导出安装效果：注册表 / 服务 / 环境变量 / 快捷方式脚本 + `pack.xml` 清单（别名 `fx`） |
| `extract` | `<包> <输出目录>` | 解包：还原目录树并解压 CAB 中的文件；`.msp` 导出补丁负载 |
| `pack` | `<源目录> <输出.msi>` | 打包：目录 → 自包含 MSI（一个或多个内嵌 MSZIP CAB） |
| `transform` | `<原包.msi> <新包.msi> <输出.mst>` | 生成 transform：比较两个 MSI，按差异写出 `.mst`（`--dry-run` 只比较不写） |
| `apply` | `<原包.msi> <输出.msi> <file.mst...>` | 应用 transform：复制原包 → 依次应用 MST → 提交为新包 |
| `mst` | `<file.mst>` | 查看 transform：摘要流（错误条件 / 校验项）、目标产品、内部流；`--base-msi` 时列出 `_TransformView` 变更明细 |

`list` 的常用名称：`files`、`components`、`features`、`dirs` / `directories`、`media`、`properties`、`streams`、`cabs`、`shortcuts`、`registry`、`services`、`customactions`、`upgrade`、`launchconditions`、`featurecomponents`、`binaries`、`assemblies`、`tables`，也可直接写任意真实表名。

### 全局选项

| 选项 | 作用 |
| --- | --- |
| `-h`, `--help`, `/?` | 显示帮助 |
| `-v`, `--verbose` | 详细日志（stderr，前缀 `[v]`） |
| `--tsv` | 表格以 TSV 输出（单元格内的 tab/换行会转义） |
| `--format <all\|scripts\|manifest>` | `effects`：写脚本包 + XML 清单 / 仅脚本包 / 仅 XML 清单（默认 `all`） |

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

### 生成 / 应用 transform（`transform` / `apply` / `mst`）

transform（`.mst`）是 Windows Installer 的“补丁式差异数据”：它只包含两张数据库之间的**差异**（改动的行、新增/删除的表、替换的 `_Streams` 负载），应用时必须针对**原包（base）**。`msitool` 三个命令覆盖了完整生命周期：

| 命令 | 做什么 | 用到的 API |
| --- | --- | --- |
| `transform` | 比较两个 MSI，写出 `.mst`；先逐表 diff 并打印改动统计 | `MsiDatabaseGenerateTransformW`、`MsiCreateTransformSummaryInfoW` |
| `apply` | 复制原包 → 在事务句柄上依次应用 MST → 提交为新包 | `MsiDatabaseApplyTransformW`、`MsiDatabaseCommit` |
| `mst` | 读取 `.mst` 的摘要流与内部流；带 `--base-msi` 时列出 `_TransformView` 变更明细 | 属性集（OLE）读取、`MSITRANSFORM_ERROR_VIEWTRANSFORM` |

```bat
:: 1) 只比较，不落盘（输出 .mst 可省略）
msitool transform setup.msi setup2.msi --dry-run

:: 2) 生成 transform，写入 error / validation 标志
msitool transform setup.msi setup2.msi custom.mst --errors addexistingrow ^
                 --validate product,language --force

:: 3) 应用到一个新包（原包不动）
msitool apply setup.msi setup-custom.msi custom.mst

:: 4) 查看 transform 本身；再加 --base-msi 看安装程序视角的变更明细
msitool mst custom.mst
msitool mst custom.mst --base-msi setup.msi
```

`transform` 的输出（真实样例，两个 Windows Desktop Runtime 包）：

```
Transform : T:\out\big.mst
Size      : 104.0 KiB
Errors    : none (suppressed)
Validation: none
Summary   : written (MsiCreateTransformSummaryInfo)
Changes   : 15 table(s), 877 added row(s), 601 removed, 6 modified

Table                  | Added | Removed | Modified | Schema
-----------------------+-------+---------+----------+--------
Component              | 283   | 190     | 1        |
Directory              | 15    | 6       | 0        |
File                   | 282   | 184     | 0        |
Media                  | 0     | 0       | 1        |
Property               | 0     | 0       | 4        |
_Streams               | 4     | 4       | 0        |
```

`mst --base-msi` 的输出直接来自安装程序：

```
Table             | Row           | Column      | New value             | Current value
------------------+---------------+-------------+-----------------------+--------------------
Directory         | INSTALLFOLDER | DefaultDir  | F basic               | F minimal
Property          | ProductName   | Value       | F basic               | F minimal
Property          | ProductCode   | Value       | {A957EE14-9111-...}   | {F44AAA7E-C4A5-...}
Dialog            |               | DROP        |                       |
```

实现要点：

- **diff 的行身份**用表的主键；无主键的表退化为整行内容，因此改动表现为「删除 + 新增」。
- **二进制列**（`_Streams.Data`、`Binary.Data` 等）用 FNV-1a 64 位指纹比较，所以替换了 CAB 但行键不变也能看出来；`_Streams` / `_Storages` 不在 `_Tables` 里，diff 会显式补上（transform 会携带这些负载）。
- `transform` 发现两个包完全相同（安装程序返回 `ERROR_NO_DATA`）时**不写空文件**，只报告“无差异”。
- `apply` 先复制原包，再用 `MSIDBOPEN_TRANSACT` 打开副本：任何一步失败都不会留下半成品（副本会被删除）；目标与原包同一个文件时必须显式 `--force`（原地应用）。
- 原包带 Authenticode 签名时，`apply` 会提示：签名流会被原样复制并因此失效，需要重新签名。
- `--errors` / `--validate` 接受名称（`addexistingrow`、`product`、`language` …）或数值（`0x11`），含义就是 `MSITRANSFORM_ERROR_*` / `MSITRANSFORM_VALIDATE_*`；不带校验标志时按安装程序的默认行为（不校验）执行。
- 查看 transform 摘要流时会把安装程序打包的「错误条件 / 校验项」解出来（低 16 位 = error conditions，高 16 位 = validation），无需猜测。

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
| `--per-user` / `--all-users` | `--all-users` | `--per-user`：不写 `ALLUSERS`，默认装到 `LocalAppDataFolder\<ProductName>`，快捷方式落在当前用户的桌面 / 开始菜单；`--all-users`：写 `ALLUSERS=1`，默认 `Program Files`（per-machine） |
| `--install-dir <路径>` | — | 安装位置：留空 = `Program Files\<ProductName>`；`App` / `Tools\App` 相对 Program Files；`LocalAppDataFolder\App` 等标准目录前缀；`D:\Apps\App` / `\\server\share\App` 绝对路径（`INSTALLFOLDER` 属性钉位）。详见 [安装位置](#安装位置) |
| `--language <LCID>` | `1033` | ProductLanguage 与 SummaryInformation Template |
| `--cab-size <n>[M\|G]` | `0` | 按未压缩字节把文件切到多个内嵌 CAB；`0` = 单 CAB。支持 `64K` / `16M` / `1G` 等后缀 |
| `--no-empty-dirs` | — | 不写 `CreateFolder` 表，空目录不会在安装时创建 |
| `--large-files <mode>` | `layout` | > 2 GiB 文件：`layout` 写成 MSI 同目录的未压缩伴随文件；`error` 直接拒绝 |
| `--ui <none\|basic\|minimal\|wizard\|custom>` | `none` | 安装界面：无 UI / Windows Installer 基础进度条 / 旧的紧凑向导 / **内置多页向导**（欢迎·许可·目录·确认·进度·完成 + 维护模式）/ XML 自定义（可 `base="wizard"` 复用布局） |
| `--ui-template <file.msi>` | — | 用现有 MSI 的 UI 表作为基座（`Dialog` / `Control` / `ControlEvent` / `CheckBox` / `RadioButton` / `Binary` / `InstallUISequence` 等） |
| `--ui-locale <en\|zh-CN>` | 跟随 `--language` | 内置向导文案语言（1033 → en，2052 / 1028 → zh-CN） |
| `--desktop-shortcut` | — | 为主程序创建桌面快捷方式 |
| `--start-menu-shortcut` | — | 为主程序创建开始菜单快捷方式 |
| `--main-exe <相对路径>` | 自动 | 快捷方式指向的主程序；省略时取源根的唯一 `.exe`，否则取最大的 `.exe` |
| `--dump-ui <目录>` | — | 把内置向导的 XML / EULA / banner 导出到目录后退出 |
| `--config <file.xml>` | — | XML 清单：多 Feature、快捷方式、注册表、服务、环境变量、UI、签名、默认值（**不再支持 INI**） |
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
Install   : [ProgramFiles64Folder]\My App
Codes     : ProductCode={...} UpgradeCode={...}
Platform  : x64;1033
Cabinets  : 1 embedded CAB(s), 1 feature(s), 0 empty dir(s), 0 >2 GiB layout file(s)
Cabinet   : 1114953 bytes in 1 CAB(s) (MSZIP)
MSI       : 1138688 bytes
MsiVerifyPackage: OK (Windows Installer accepts this package)
Done      : E:\work\winmsi\app.msi
```

基础包含 9 张表：`Directory`、`Component`、`File`、`Media`、`Feature`、`FeatureComponents`、`Property`、`InstallExecuteSequence`、`AdminExecuteSequence`，以及完整 SummaryInformation 和写入列元数据的 `_Validation` 表；按需再加 `CreateFolder`、`Shortcut`、`Icon`、`MsiShortcutProperty`、`RemoveFile`、`Registry`、`ServiceInstall`、`ServiceControl`、`Environment`，以及 UI 的 `InstallUISequence` / `AdminUISequence` / `Dialog` / `Control` / `ControlEvent` / `ControlCondition` / `CheckBox` / `RadioButton` / `ComboBox` / `ListBox` / `ListView` / `TextStyle` / `Binary` / `Error` / `Billboard`。因为带 `AdminExecuteSequence`，可以用不写注册表的方式让 Windows Installer 自己验证：

```bat
msiexec /a app.msi /qn TARGETDIR=C:\out\admin
```

### 打包进阶

#### 分卷、大文件与空目录

- `--cab-size 16`：按**未压缩**输入字节把文件切成多个内嵌 CAB（`cab1.cab`、`cab2.cab`…）。每个 CAB 一条 `Media` 行（`DiskId` / `LastSequence` / `#cabN.cab`），全部写进 `_Streams`；单个文件超过阈值时独占一个 CAB。省略时仍是单 CAB。`extract` 与 `msiexec /a` 都能按 `Media.LastSequence` 还原。
- 默认写 `CreateFolder` 表，配合已在序列里的 `CreateFolders` 标准动作，源目录里的**空目录**（含嵌套空目录）在安装时也会被创建；`--no-empty-dirs` 可关闭。
- `File.FileSize` 是 32 位整数，FCI 的 seek 回调也只能表示 32 位偏移，因此 > 2 GiB 的文件无法进 CAB。默认 `--large-files layout`：把它作为**未压缩 layout 文件**写到 MSI 同目录的 `<ProductName>\...` 伴随目录，写一条 `Cabinet` 为空的 `Media` 行和 `File.Attributes=8192`（Noncompressed），`FileSize` 记 0 并给出警告。`extract` 会从该伴随目录复制还原。这类包必须把伴随目录和 MSI 一起分发。`--large-files error` 恢复旧的「直接拒绝」行为。

#### 安装位置

默认把源目录映射到 `Program Files\<ProductName>`（per-machine）：`Directory` 表是 `TARGETDIR` → `ProgramFilesFolder` → `INSTALLFOLDER`（`DefaultDir` = ProductName）→ 源目录层级。`--per-user` 时锚点换成 `LocalAppDataFolder`，并且不写 `ALLUSERS`（`DesktopFolder` / `ProgramMenuFolder` 随之解析到当前用户）。`--install-dir` 换掉锚点与名字，`--files` 选择的相对路径仍保持在安装根之下：

| 写法 | 结果 |
| --- | --- |
| `--install-dir "My App"` | `[ProgramFilesFolder]\My App`（或 `ProgramFiles64Folder`，跟随 `--platform`） |
| `--install-dir "Vendor\My App"` | `[ProgramFilesFolder]\Vendor\My App`，中间层各写一行 `Directory`（`ID1`…） |
| `--install-dir "LocalAppDataFolder\My App"` | `[LocalAppDataFolder]\My App`，适合免管理员/按用户安装 |
| `--install-dir "CommonAppDataFolder\Vendor\App"` | `[CommonAppDataFolder]\Vendor\App` |
| `--install-dir "LocalAppDataFolder"` | 直接装进该标准目录（`INSTALLFOLDER` 的 `DefaultDir` 为 `.`） |
| `--install-dir "D:\Apps\My App"` | 绝对路径：`Directory` 表保持相对结构，另写一条 `Property` 行 `INSTALLFOLDER = D:\Apps\My App` 把它钉到绝对位置 |
| `--install-dir "\\server\share\App"` | UNC 路径，同上（目标机器上的安装账号要能访问该共享） |

标准目录前缀（大小写不敏感）：`ProgramFilesFolder`、`ProgramFiles64Folder`、`CommonFilesFolder`、`CommonFiles64Folder`、`ProgramMenuFolder`、`StartMenuFolder`、`StartupFolder`、`DesktopFolder`、`AppDataFolder`、`LocalAppDataFolder`、`CommonAppDataFolder`、`PersonalFolder`、`SendToFolder`、`TemplatesFolder`、`FavoritesFolder`、`FontsFolder`、`MyPicturesFolder`、`AdminToolsFolder`、`TempFolder`、`WindowsFolder`、`WindowsVolume`、`SystemFolder`、`System64Folder`。

绝对路径为什么不能写成 `Directory.DefaultDir`：那一列里 `:` 是 `[targetname]:[sourcename]` 分隔符，`D:\Apps\App` 会被读成 target = `D`、source = `\Apps\App`（实测：`msiexec /a` 报 2714「Invalid value for default folder name」，`msiexec /i` 则老老实实建了个名叫 `D` 的文件夹）。按 MSI 文档「Directory 列可以是一个被设为目标完整路径的属性的名字」，正确做法是保留相对目录树，再用一条 `Property` 行把 `INSTALLFOLDER` 属性设成绝对路径 —— 打包器现在就是这么做的，`Directory` 表的 `DefaultDir` 仍取绝对路径的最后一段（如 `MyApp`），所以 `extract` 与 `msiexec /a` 照旧还原出相对树。

`--install-dir` 只改**打包时的默认值**，`INSTALLFOLDER` 仍是安装根，所以终端用户依旧可以整体搬家（命令行属性优先于 `Property` 表里的钉位值）：

```bat
msiexec /i app.msi /qb INSTALLFOLDER="D:\Apps\My App"
```

清单的 `<pack installDir="...">` 同样接受安装位置，命令行同名选项优先。

#### UI

`--ui` 决定安装界面，五种模式可以组合 `--ui-template` / `--ui-locale`：

| 模式 | 行为 |
| --- | --- |
| `none`（默认） | 不建 `InstallUISequence`，`/qn` 静默安装 |
| `basic` | 只加 `CostInitialize` / `FileCost` / `CostFinalize` / `ExecuteAction`，用 Windows Installer 自带进度条 |
| `minimal` | 旧的紧凑向导：欢迎 / 进度 / 完成 / 取消 / 错误五页（保持向后兼容） |
| `wizard` | **内置多页向导**：欢迎 → 许可协议 → 目标文件夹（含 BrowseDlg 目录浏览）→ 确认 → 进度 → 完成；已安装时进入维护模式（修复 / 删除）；共 12 个对话框 |
| `custom` | 对话框全部来自 XML；写 `base="wizard"` 可复用内置布局，只覆盖需要的行 |

- **中英两套**：`--language 1033` → en，`2052` / `1028` → zh-CN，`--ui-locale en|zh-CN` 可显式覆盖。文案与布局分离（`wizard.layout.xml` + `wizard.<locale>.xml`），字体随 locale 切换（Tahoma 8 / Microsoft YaHei 9）。
- **BrowseDlg**：`DirectoryCombo` + `DirectoryList` + `PathEdit`，全部使用 `_BrowseProperty` + `Indirect`，`SetTargetPath` 写回 `INSTALLFOLDER`；没有自定义动作。
- **许可页**：RTF 由 `ScrollableText` 显示，`CheckBox` 写 `LicenseAccepted`，未勾选时用 `ControlCondition` 禁用「下一步」。
- **维护模式**：`InstallUISequence` 用 `Installed` / `NOT Installed` 条件在欢迎页与维护欢迎页之间切换；修复走 `Reinstall All` + `ReinstallMode ecmus`，删除走 `Remove All`。
- **UI 模板**：`--ui-template <msi>` 读取现有包的 `Dialog` / `Control` / `ControlEvent` / `ControlCondition` / `CheckBox` / `RadioButton` / `Binary` / `TextStyle` / `Error` / `Billboard` 与 `InstallUISequence`，只保留标准动作；模板里的自定义动作会按类型导入（Binary/文本类）或按规则丢弃并告警，详见 [自定义动作](#自定义动作customactions)。
- `--dump-ui <目录>` 导出内置 `wizard.layout.xml`、两个 locale、两份示例 EULA 与 banner / background 位图，便于二次修改。

无论哪种模式，包都能用 `/qn`、`/qb`、`/a` 安装；`msiexec /a` 的 UI 走 `AdminUISequence`。

#### 数字签名

`--sign-cert <pfx>` 在数据库提交后调用 `signtool.exe sign /fd SHA256` 写入 `\x05DigitalSignature` 流，可用 `--sign-password`、`--sign-timestamp`、`--sign-tool` 补充参数；签名后再次用 `MsiVerifyPackageW` 自检。签名是**可选**的：仅在打包时需要 Windows SDK 的 `signtool.exe`（自动从 `PATH` 或 `%ProgramFiles(x86)%\Windows Kits\10\bin\*\x64` 查找），运行时依旧只依赖系统 `msi.dll` / `cabinet.dll` / `msxml6.dll`。

#### XML 清单：`--config`

复杂资源用一个 XML 清单描述，完整示例见 [examples/pack.xml](examples/pack.xml)，自定义 UI 示例见 [examples/ui-custom.xml](examples/ui-custom.xml)。根元素必须带命名空间 `https://msitool.dev/schema/pack/v1`；载荷路径（feature 的 `dirs`、shortcut 的 `target` / `icon`）按源目录解析，资源文件（`binary file`、`sign cert`、`ui template`）按 XML 所在目录解析。**命令行的同名选项优先于 `<pack>` 属性**。INI 清单已移除，传 `.ini` 会直接报错。 编辑器可加载 [schemas/msitool-v1.xsd](schemas/msitool-v1.xsd) 做补全与校验。

| 元素 | 常用属性 | 生成的表 / 作用 |
| --- | --- | --- |
| `<pack>` | `name`、`version`、`manufacturer`、`productCode`、`upgradeCode`、`platform`、`language`、`installDir`、`ui`、`cabSize`、`largeFiles`、`keepEmptyDirs` | 填充命令行未指定的默认值 |
| `<features><feature>` | `id`、`title`、`description`、`dirs`（`;` 分隔）、`level`、`display`、`attributes` | `Feature` + `FeatureComponents`；未匹配的目录归 `MainFeature` |
| `<shortcuts><shortcut>` | `id`、`name`、`directory`（标准目录 / 别名 / 子目录 / 源内目录）、`target`、`arguments`、`description`、`icon`、`iconIndex`、`workingDirectory`、`component`、`advertise` | `Shortcut` + `Icon` + 可选 `MsiShortcutProperty` |
| `<registry><registryValue>` | `id`、`root`（`HKLM` / `HKCU` / `HKCR` / `HKU` / `auto`）、`key`、`name`、`value`、`type`（`string` / `integer` / `binary` / `expand` / `multi` / `key` / `keydelete` / `keycreatedelete`）、`component` | `Registry` |
| `<services><service>` | `id`、`name`、`displayName`、`image`、`type`（`own` / `share` / `interactive`）、`start`（`auto` / `demand` / `disabled` / `system`）、`error`、`account`、`password`、`arguments`、`dependencies`、`description` | `ServiceInstall` + `ServiceControl` |
| `<environment><variable>` | `id`、`name`、`value`、`action`（`set` / `create` / `append` / `prepend` / `remove`）、`separator`、`system` | `Environment` |
| `<sign>` | `cert`、`password`、`timestamp`、`tool` | 与 `--sign-*` 等价 |
| `<customactions><customaction>` | `id`、`type`（`dll` / `exe` / `dllfile` / `exefile` / `exedir` / `setproperty` / `setdirectory` / `runcommand` 或数字）、`binary` / `file` / `directory` / `property` / `entry` / `target` / `args` / `value`、`deferred` / `commit` / `rollback` / `async` / `continue` / `impersonate` / `taware` / `x64` / `hidetarget` | `CustomAction` + 可选 `Binary` 流 |
| `<executeSequence>` / `<adminExecuteSequence>` | 子 `<action name value condition>` | `InstallExecuteSequence` / `AdminExecuteSequence` 追加行 |
| `<ui>` | `base`（`wizard` / `minimal`）、`locale`、`font`、`template` | UI 基座与增量段 |
| `<ui><textstyle>` | `id`、`face`、`size`、`color`、`style` | `TextStyle` |
| `<ui><dialog>` | `id`、`title`、`width`、`height`、`attributes` / `modeless`、`first`、`default`、`cancel`、`sequence`、`condition` | `Dialog` + `InstallUISequence` |
| `<ui><control>` | `id`、`type`、`x/y/width/height`、`attributes` / `visible` / `enabled` / `sunken` / `indirect`、`property`、`text` / `textFile`、`binary`、`next`、`help` | `Control`；`RadioButtonGroup` / `CheckBox` / `ListBox` 等按需生成子表 |
| `<ui><publish>` / `<setproperty>` | `event`、`value`、`condition`、`order` / `name`、`value` | `ControlEvent` |
| `<ui><controlcondition>` | `action`（`enable` / `disable` / `show` / `hide` / `default`）、`condition` | `ControlCondition` |
| `<ui><checkbox>` | `property`、`value` | `CheckBox` |
| `<ui><binary>` | `id`、`file` | `Binary`（位图 / 图标 / RTF） |
| `<ui><error>` | `id`、`text` | `Error` |
| `<ui><billboard>` / `<billboardaction>` | `id`、`feature`、`action`、`ordering`、子 `<control>` | `Billboard` + `BBControl` |
| `<ui><uisequence>` / `<adminuisequence>` | 子 `<action name value condition>` | `InstallUISequence` / `AdminUISequence` |

`component` 可写组件键（`C1`）、目录键（`INSTALLFOLDER` / `D2`）、源内相对目录或文件相对路径；省略时资源挂到目标文件所属组件（无目标时挂到根组件）。标准动作 `CreateShortcuts`、`WriteRegistryValues`、`InstallServices`、`StartServices`、`WriteEnvironmentStrings`（以及卸载侧的 `RemoveShortcuts`、`RemoveRegistryValues`、`StopServices`、`DeleteServices`）已加入 `InstallExecuteSequence`。

快捷方式的 `directory` 支持标准目录名与别名 `desktop` / `startmenu` / `startup` / `sendto`，也支持 `ProgramMenuFolder\厂商\产品` 这样的多级子目录：打包器会为每一级生成 `Directory` + 组件（HKLM 注册表 keypath，`RemoveFile` 负责卸载清理），因此子菜单目录会被真实创建。`advertise="true"` 时 `Target` 写 feature 名（由目标文件所在 feature 决定），并要求显式 `component`；`target` 为空且非广告式会直接报错。`icon` 接受 `.ico` / `.exe` / `.dll`，`iconIndex` 选择图标序号。`<shortcut>` 内可加 `<msishortcutproperty key="..." value="..."/>` 写 `MsiShortcutProperty`（任务栏 / 开始菜单固定）。

```xml
<msitool xmlns="https://msitool.dev/schema/pack/v1">
  <pack name="My App" version="1.2.3" ui="wizard" language="2052"/>
  <features>
    <feature id="Core" title="核心文件" dirs="bin"/>
  </features>
  <shortcuts>
    <shortcut id="Desktop" name="My App.lnk" directory="desktop"
              target="bin\app.exe" icon="bin\app.exe" iconIndex="0"/>
  </shortcuts>
</msitool>
```

#### 自定义动作（customactions）

`<customactions>` 生成 `CustomAction` 表；代码可以内嵌为 `Binary` 流（`binary`），也可以引用已安装文件（`file`）或标准目录（`directory`）。序列由 `<executeSequence>` / `<adminExecuteSequence>` 追加，UI 按钮可直接 `<publish event="DoAction" value="动作名"/>`。完整示例见 [examples/custom-actions.xml](examples/custom-actions.xml)：

```xml
<customactions>
  <customaction id="SetProp" type="setproperty" property="MYPROP" value="formatted-value"/>
  <customaction id="RunCmd" type="exedir" directory="SystemFolder" deferred="yes"
                target="cmd.exe /c echo [MYPROP]&gt;[INSTALLFOLDER]bin\ca.txt"/>
</customactions>
<executeSequence>
  <action name="SetProp" value="1100"/>
  <action name="RunCmd" value="4100" condition="NOT Installed"/>
</executeSequence>
```

- `deferred="yes"` 把动作排入安装脚本（此时文件已落盘），`impersonate="no"` 加 `NoImpersonate`，`commit` / `rollback` 只与 deferred 组合使用；`async` / `continue` 控制返回码处理。
- `type` 也接受数字（原始 `CustomAction.Type`）以及 `dll` / `exe`（内嵌 Binary）、`dllfile` / `exefile`（已安装文件）、`exedir`（标准目录 + exe）、`setproperty` / `setdirectory` / `runcommand`。
- 导入 UI 模板时，引用 `Binary` 的自定义动作会连同流一起导入；引用模板自身文件/目录的动作会丢弃并告警。

#### GUI 可视化编辑器（打包 → 自定义动作）

打包页第 5 个标签页把 `CustomAction` / `InstallExecuteSequence` 变成可视化编辑，无需手写 XML：

- **工具栏**：顶部一行——左侧是列表操作（新增 / 复制 / 删除 / 上移 / 下移 / 清空，无选中或到边界时自动禁用），右侧是清单操作（导入 / 复制片段 / 导出清单）；卡片副标题即实时检查结果（✓ / ⚠ / ✗ + 数量 + 当前行问题）。
- **动作列表**：左侧表格吃满整列高度，显示状态图标（✓ 通过 / ⚠ 提醒 / ✗ 错误）、类型、来源与序号。点击一行后，右侧属性表**只显示该类型需要的字段**（`dll` → Binary 文件 / 入口点 / 命令行，`setproperty` → 属性名 / 值，`exedir` → 目录键 / 命令行……），其余字段自动隐藏。
- **类型与序列**：类型下拉含 8 种常用写法（也保留导入清单里的原始数字类型）；「插入阶段」下拉给出避开标准动作序号的建议值（1100 / 1550 / 3900 / 4100 / 5300 / 6450），也可直接输入序号与条件（如 `NOT Installed`）。9 个标志（`deferred` / `commit` / `rollback` / `async` / `continue` / 不模拟用户 / 终端服务感知 / x64 / 隐藏 Target）用复选框切换。
- **整页布局**：该标签页不显示「运行日志」卡片（日志仍在后台记录，切到其它标签页即可查看），编辑区独占整个内容区：左列动作列表 + 右列编辑区。右列用「属性表单 / 执行序列」两个按钮切换——两栏轮流吃满编辑区高度，而不是上下叠在一起；这样在矮窗口（例如 1360x736）或高 DPI 下，属性行、复选框和文本控件不会再被压到 20 px 甚至互相重叠。窗口变大时字段行高（最高 46 px）、列表行数与序列行数同步增大。
- **序列时间线**：右列第二个视图把标准动作与自定义动作按 `InstallExecuteSequence` 序号混排，自动滚动并高亮当前动作；点击自定义（⚡）行即可选中它，回到「属性表单」即可编辑。
- **实时检查**：ID 重复 / 必填来源与目标 / 序号范围与冲突 / 延迟动作未排入序列 / Binary 文件不存在等，错误会阻止打包并跳回本页，提醒只显示黄字；当前选中行的问题直接写在卡片副标题里。
- **XML 往返**：「导入」从 XML 清单读取 `<customactions>` + `<executeSequence>`（未填配置文件时弹出文件选择）；「复制片段」把当前列表复制成可粘贴进清单的 XML；「导出清单」写出可直接作为 `--config` 使用的完整清单。拖放 XML 清单到「配置文件」时，若编辑器尚未使用也会自动载入其中的动作。
- **生效规则**：一旦本页被编辑，打包就以本页列表为准（清单中的 `<customactions>` 及对应序列行会被替换），因此删除 / 改名都会真实生效；未编辑过时清单照常生效。

```bat
msitool pack C:\payload C:\out\app.msi --config examples\pack.xml --cab-size 16 --ui wizard
msitool pack C:\payload C:\out\app.msi --config examples\custom-actions.xml --force
msitool pack C:\payload C:\out\app.msi --ui template --ui-template C:\templates\ui.msi
msitool pack C:\payload C:\out\app.msi --ui custom --config examples\ui-custom.xml
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

### 安装效果导出（`effects`）

`list` / `query` / `export` 给你的是 MSI 的**原始表**；`effects` 把「装这个包会改动系统什么」翻译成可读、可执行的脚本，以及一份能直接回填 `pack --config` 的 XML 清单。全程只读数据库：不执行安装、不写注册表、不运行 CustomAction。

```bat
msitool effects setup.msi C:\out\effects
msitool effects setup.msi C:\out\effects --format scripts
msitool effects setup.msi C:\out\effects --format manifest
msitool effects setup.msi C:\out\effects --transform custom.mst --verbose
```

`--transform` 与 `info` / `list` / `extract` 一样先叠加只读视图，所以导出的也是 transform 之后的效果。GUI 在「安装效果」页提供同样的三种导出内容与输出目录（留空自动填 `<包名>_effects`）。

| `--format` | 产出 |
| --- | --- |
| `all`（默认） | 脚本包 + `pack.xml` + `effects.md` |
| `scripts` | 仅脚本包与审计文本（仍导出 `icons/` 供 `shortcuts.ps1` 使用） |
| `manifest` | 仅 `pack.xml` 与其引用的 `icons/` / `binaries/` |

```
<outdir>\registry.reg               安装侧注册表写入（UTF-16 LE + BOM，regedit 可直接导入）
<outdir>\registry-uninstall.reg     卸载侧删除（值写 "Name"=-，* 键写 [-Key]）
<outdir>\services-install.cmd      sc create / sc description / 安装侧 ServiceControl
<outdir>\services-uninstall.cmd    sc stop / sc delete / 卸载侧 ServiceControl
<outdir>\environment.cmd           setx / reg delete，[~] 追加写成 %PATH%;...
<outdir>\shortcuts.ps1             PowerShell + WScript.Shell 重建 .lnk
<outdir>\custom-actions.txt        CustomAction + 自定义动作序列行（仅审计）
<outdir>\tables.txt                Registry / ServiceInstall / ServiceControl / Environment / Shortcut / CustomAction 原始表
<outdir>\properties.txt            Property 表，便于人工解析 [Property]
<outdir>\icons\<Icon_>.ico        Shortcut.Icon_ 的原始流
<outdir>\binaries\<Source>.bin    Binary 型自定义动作的 DLL / EXE 流
<outdir>\pack.xml                  可回填的清单（<registry> / <services> / <environment> / <shortcuts> / <customactions> / <executeSequence>）
<outdir>\effects.md                统计、文件说明、回填命令与限制清单
```

关键映射：

- **注册表**：`Root` → 根键，`#` → `dword:`，`#x` → `hex:`，`#%` → `hex(2)`，`[~]` → `hex(7)`，`+` / `-` / `*` → 建键 / 删键 / 卸载删键；`[ProductVersion]` 之类会用 Property 表的字面值替换。
- **服务**：`ServiceInstall` 没有 ImagePath，工具按 `Component.KeyPath` → `File` → `Directory` 推导出可执行文件；`ServiceType` / `StartType` / `ErrorControl` 映射为 `type=` / `start=` / `error=`。`ServiceControl.Event` 的安装位（1/2/8）与卸载位（16/32/128）分别落到两个 `.cmd`。
- **环境变量**：`=-` / `+-` / `!-` 前缀与 `*` 系统标志映射为 `setx` / `setx /M` / `reg delete`；值里的 `[~]` 还原成 `%NAME%;value` 形式的追加/前置。
- **快捷方式**：`[#FileKey]` 通过 `File` / `Component` / `Directory` 还原成真实路径，标准目录（`DesktopFolder` 等）映射为 PowerShell 的 `[Environment]::GetFolderPath(...)`。
- **`pack.xml`**：`<pack>` 带产品名 / 版本 / GUID / 平台 / 语言 / 安装范围，资源段可直接被 `--config` 读取；自动跳过 pack 自己会生成的 `RegShortcutDir*` 行，避免主键冲突；广告式快捷方式、超出 `1..32767` 的自定义动作类型会给出告警并跳过。

回填（实测逐表一致）：

```bat
msitool pack C:\payload C:\out\app.msi --config C:\out\effects\pack.xml --force
```

限制：脚本里的 `[#File]`、条件表达式等 formatted 文本保持原样并写入注释，`[INSTALLFOLDER]` 在 `.cmd` / `.reg` 中按 `Directory` 表推导出可用路径、在 `.ps1` 与 `pack.xml` 中保留原文；`pack.xml` 不重放 UI、Feature、文件树、Transform 与签名；`ServiceControl` 事件在 pack 里归一化为 `163`（原始事件保留在两个 `.cmd` 里）；`Component_` 是源包组件键，新包找不到时 pack 回退到根组件；Binary 型自定义动作的 `Source` 键会被 pack 改成 `<id>Binary`（内容不变）。脚本是审计 / 复现辅助，导入前请人工审阅。

`tools/effects_regression.py` 覆盖三种格式、文件编码、脚本内容与 `pack.xml` 往返打包。

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
│   ├── effects.h / effects.cpp  安装效果导出：脚本包生成、目录重建、pack.xml 往返清单
│   ├── pack.h / pack.cpp        FCI 多 CAB 压缩 + Msi 建库/建表/写流/签名/摘要
│   ├── pack_util.h / .cpp       MSI 行 / 标识符 / 标准目录等公共助手
│   ├── pack_xml.h / .cpp        XML 清单解析、locale 替换、UI 模板导入
│   ├── custom_actions.h / .cpp  自定义动作模型：XML 解析 / MSI 编码 / 检查 / 序列化（GUI 与清单共用）
│   ├── pack_ui.h / .cpp         UI 模型、合并、校验与表写入
│   ├── pack_ui_builtin.cpp      内置 minimal / wizard（从 RCDATA 读取）
│   ├── xml.h / .cpp             MSXML6 只读 DOM（禁 DTD / 外部实体）
│   ├── ui.h                     UI 层接口：主题、GDI 绘制器、控件声明
│   ├── ui_draw.cpp              主题 / DPI / 双缓冲绘制 / 矢量图标
│   ├── ui_widgets.cpp           自绘控件：按钮、输入框、下拉、网格、日志、进度、导航
│   ├── ui_app.cpp               主窗口、十一个页面、业务逻辑与后台任务
│   └── main.cpp                 入口：无参数进 GUI，带参数进 CLI（全部命令）
├── resources/
│   ├── msitool.rc               RCDATA 资源清单
│   └── ui/                      wizard.layout.xml / 中英 locale / EULA / banner / background
├── docs/
│   └── PLAN-pack-xml-ui.md      打包 v2 设计（XML 清单 / 向导 / 快捷方式）
├── schemas/
│   └── msitool-v1.xsd           清单编辑器 schema
├── examples/
│   ├── pack.xml                 打包清单示例（features/shortcuts/registry/services/env）
│   ├── ui-custom.xml            base="wizard" 的自定义 UI 示例
│   └── custom-actions.xml       自定义动作示例（setproperty / exedir）
├── tools/
│   ├── dpi_probe.py             GUI 高 DPI 截图 / 伪造跨屏切换的验证脚本
│   ├── layout_check.py          自定义动作编辑器几何检查（重叠 / 越界，无需截图）
│   ├── make_ui_art.ps1          生成内置 banner / background / 示例 EULA
│   ├── pack_regression.ps1      打包回归：全部 UI 模式 + verify + /a + 向导走查
│   ├── transform_regression.py  transform / apply / mst 回归：生成、应用、往返一致、错误路径
│   ├── effects_regression.py    effects 回归：三种格式、编码、脚本内容、pack.xml 往返打包
│   └── ui_walkthrough.py        真实安装向导自动走查（en / zh-CN，per-user）
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
rc /nologo /fo build\obj\x64\msitool.res resources\msitool.rc
cl /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE ^
   /Isrc /Fobuild\obj\x64\ /Febuild\msitool_x64.exe ^
   src\util.cpp src\msidb.cpp src\cab.cpp src\extract.cpp src\effects.cpp ^
   src\pack.cpp src\pack_util.cpp src\custom_actions.cpp src\xml.cpp src\pack_xml.cpp ^
   src\pack_ui.cpp src\pack_ui_builtin.cpp src\transform.cpp src\main.cpp ^
   src\ui_draw.cpp src\ui_widgets.cpp src\ui_app.cpp ^
   build\obj\x64\msitool.res ^
   Msi.Lib Cabinet.Lib Msxml6.Lib OleAut32.Lib Shell32.Lib Ole32.Lib ^
   User32.Lib Gdi32.Lib Advapi32.Lib Comdlg32.Lib Uuid.Lib ^
   /link /MACHINE:X64 /SUBSYSTEM:WINDOWS
```

> `/SUBSYSTEM:WINDOWS` 让双击 exe 时没有控制台黑框；CLI 模式会 `AttachConsole(ATTACH_PARENT_PROCESS)`（失败则 `AllocConsole`）并重绑标准流，重定向到文件时也不会破坏管道。

| 库 | 用途 |
| --- | --- |
| `Msi.Lib` | `MsiOpenDatabaseW` / `MsiDatabaseOpenViewW` / `MsiRecordReadStream` / … |
| `Cabinet.Lib` | FDI（`FDICopy`）解压 + FCI（`FCIAddFile`）压缩 |
| `Msxml6.Lib` / `OleAut32.Lib` | XML 清单解析（MSXML6 DOM，系统组件）/ `BSTR` 与 `VARIANT` |
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
build\msitool_x64.exe pack C:\payload C:\out\full.msi --config examples\pack.xml --ui wizard --force
build\msitool_x64.exe tables C:\out\full.msi
build\msitool_x64.exe pack C:\payload C:\out\signed.msi --sign-cert C:\cert.pfx --force
build\msitool_x64.exe list C:\out\signed.msi streams

:: 5) 解包进阶（transform 视图 / .msp / 分卷集）
build\msitool_x64.exe list setup.msi properties --transform custom.mst
build\msitool_x64.exe extract setup.msi C:\out\files --transform custom.mst
build\msitool_x64.exe extract update.msp C:\out\patch
build\msitool_x64.exe extract update.msp C:\out\patch --base-msi setup.msi

:: 6) 安装效果（脚本 + 可回填清单）
build\msitool_x64.exe effects setup.msi C:\out\effects
build\msitool_x64.exe effects setup.msi C:\out\effects_scripts --format scripts
build\msitool_x64.exe pack C:\payload C:\out\rebuilt.msi --config C:\out\effects\pack.xml --force

:: 7) transform 侧（生成 → 应用 → 往返比较）
build\msitool_x64.exe transform setup.msi setup2.msi C:\out\diff.mst --force
build\msitool_x64.exe transform setup.msi setup2.msi --dry-run
build\msitool_x64.exe mst C:\out\diff.mst --base-msi setup.msi
build\msitool_x64.exe apply setup.msi C:\out\applied.msi C:\out\diff.mst --force
build\msitool_x64.exe transform C:\out\applied.msi setup2.msi --dry-run   :: 应报「无差异」
```

还可以让 Windows Installer 自己验证生成的包（administrative install 不写注册表）：

```bat
msiexec /a C:\out\app.msi /qn TARGETDIR=C:\out\admin
```

安装效果侧的一键回归：`tools/effects_regression.py` 覆盖三种 `--format`、文件编码与脚本内容、`pack.xml` 的 XSD 兼容性，以及 `pack --config` 回填后注册表 / 服务 / 环境变量 / 快捷方式 / 自定义动作逐表一致（默认拿 `build\f_manifest.msi` / `t_ca.msi` / `t_cabin.msi` / `merge_ref.msi` 做样本，73 项断言）。

transform 侧的一键回归：`tools/transform_regression.py` 用 `build\f_minimal.msi` → `build\f_basic.msi` 覆盖 10 组断言（dry-run 不落盘、摘要流标志、`_TransformView` 变更、校验不匹配即失败且不留半成品、往返逐表一致、相同包不写空文件、`.msp` 被拒、未知标志报错、`--no-summary`、`--transform` 视图仍生效）。

打包侧的一键回归：`tools/pack_regression.ps1` 会自动生成临时载荷，依次打包 `none` / `basic` / `minimal` / `wizard`（中英）/ `custom` / XML 清单 / 快捷方式 / per-user，逐个 `verify` + `msiexec /a`；若装了 Python，还会跑真实安装向导走查（见下）。

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\pack_regression.ps1
```

真实安装向导走查（per-user，不需管理员，自动安装 + 校验 + 卸载）：

```bat
python tools\ui_walkthrough.py --msi build\regression\peruser.msi
python tools\ui_walkthrough.py --msi build\regression\peruser.msi --mode repair
python tools\ui_walkthrough.py --msi build\regression\peruser.msi --mode remove
python tools\ui_walkthrough.py --msi build\regression\peruser_zh.msi --language zh-CN
```

负向用例也应覆盖：非 MSI 文件、目录、伪造 CFB 头、缺失外部 CAB、中文/空格路径、`--dry-run`、`--force`。详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

### GUI 高 DPI 检查

界面是自绘的，缩放全部来自 `theme().dpi`（`S(px) = MulDiv(px, dpi, 96)`）。单显示器开发机也能验证任意缩放：`MSITOOL_DPI` 环境变量会覆盖进程实际 DPI，`tools/dpi_probe.py` 负责启动窗口、截图（`PrintWindow`），并可**伪造一次显示器切换**（向主窗口发 `WM_DPICHANGED`，wParam 带新 DPI）来走真实的 `WM_DPICHANGED` 代码路径。

```bat
:: 100% / 150% 同一逻辑尺寸（1160x860）截图，肉眼或工具对比
python tools\dpi_probe.py --dpi 96  --out shots\p100.bmp
python tools\dpi_probe.py --dpi 144 --out shots\p150.bmp

:: 跨屏热切换：96 → 144（真实 WM_DPICHANGED 路径），切换前后各一张
python tools\dpi_probe.py --dpi 96 --transition 144 ^
    --out shots\switch_before.bmp --out2 shots\switch_after.bmp

:: 往返不漂移：96 → 120 → 96，第三张应与原生 96 截图一致
python tools\dpi_probe.py --dpi 96 --transition 120,96 --out shots\roundtrip.bmp

:: 先点到「包信息」页再切换（--click 用真实鼠标输入，坐标是 96 DPI 下的客户区逻辑坐标）
python tools\dpi_probe.py --dpi 96 --click 100,178 --transition 144 ^
    --out shots\grid_before.bmp --out2 shots\grid_after.bmp
```

已实测的缩放档位：72 / 96 / 110 / 120 / 144 / 168 / 192 / 240 / 288（75%–300%），均无裁剪、错位或字体截断；窗口 DPI 感知上下文为 `PER_MONITOR_AWARE_V2`（脚本会打印）。

### GUI 布局几何检查

截图只能靠肉眼，控件互相压住这种事更适合直接用坐标判定。「打包 → 自定义动作」的编辑器因此带了一个几何自检钩子：设置 `MSITOOL_LAYOUT_DUMP=<前缀>` 启动后，程序会按逻辑尺寸 × DPI 的矩阵（1160x620 / 1160x860 / 1360x736 / 1920x1040 × 96 / 144）重新布局，把每个可见控件的矩形、卡片与文字标签写进 `<前缀>_<dpi>_<w>x<h>.{props,seq}.txt`。

`tools/layout_check.py` 读这些文件并判定：控件之间是否重叠、控件与文字标签是否重叠、控件是否越出页面卡片。它的存在背景是旧版单栏编辑器在 620–860 px 高的窗口里把属性行和底部按钮挤到序列时间线标题上——纯截图对比很难稳定发现，而坐标判定一次就能定位。

```bat
set MSITOOL_LAYOUT_DUMP=%TEMP%\ca
build\msitool_x64.exe
python tools\layout_check.py --dir %TEMP%\ca -v
:: → 16 file(s), 0 problem(s)
```

`MSITOOL_LAYOUT_PAGE=<page>` 可以把同一套检查套到表单页上（`transform` / `export` / `verify` / `pack` / `settings`），输出后缀变成 `<page>`，`layout_check.py` 会一并读取：

```bat
set MSITOOL_LAYOUT_DUMP=%TEMP%\mst
set MSITOOL_LAYOUT_PAGE=transform
build\msitool_x64.exe
python tools\layout_check.py --dir %TEMP%\mst -v
:: → 8 file(s), 0 problem(s)
```

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
| `pack --config` 全量清单 | 3 个 Feature + 快捷方式 + 注册表 + 服务 + 环境变量 + 12 页向导 | `Shortcut` 2（桌面 / 开始菜单子目录）+ `Icon` 1 / `Registry` 3 / `ServiceInstall`+`ServiceControl` / `Environment` / `Dialog` 12 / `Control` 80+ / `ControlCondition` / `InstallUISequence` / `Binary` 2；`MsiVerifyPackage` OK，`msiexec /a` 成功 |
| `pack --ui wizard`（en / zh-CN） | 自包含多页向导 | 欢迎 / 许可 / 目标文件夹 / 浏览 / 确认 / 进度 / 完成 / 取消 / 错误 / 维护共 12 个对话框；`msiexec /a` 成功，且通过真实 UI 自动走查 |
| `pack --per-user` | per-user 安装 | 不写 `ALLUSERS`，文件装到 `%LocalAppData%\<产品名>`；`msiexec /i /qn` 后桌面 / 开始菜单快捷方式出现在当前用户（实测 `E:\Desktop\...` 与 `%AppData%\...\Programs\...`），`/x` 卸载后快捷方式与 HKCU 子菜单 keypath 注册表值被删除 |
| `pack --ui template ref.msi` | 复用现有 MSI 的 UI 表 | 22 个对话框 + `CheckBox` / `RadioButton` / `Binary` 导入，引用 Binary 的自定义动作随流导入 |
| `pack --install-dir` | 自定义安装位置 | `Tools\MyApp` / `LocalAppDataFolder\My App\Sub` / `CommonAppDataFolder\Vendor\App`：`msiexec /i /qn` 后文件真的落在 `%LocalAppData%\My App\Sub` 等位置，`msiexec /a` 与 `extract` 也同构；绝对路径 `E:\...\MyApp` 通过 `Property` 行钉位，实测装到该目录，命令行 `INSTALLFOLDER=` 可再覆盖，`/a` 与 `extract` 仍还原相对树 |
| `pack --sign-cert`（自签名 PFX） | 已签名 MSI | `\x05DigitalSignature` + `\x05MsiDigitalSignatureEx` 流写入，`MsiVerifyPackage` 仍 OK |
| `pack`（2.3 GiB 稀疏文件） | 未压缩伴随 layout | `FileSize=0`、`Attributes=8192`、一条空 `Cabinet` 的 `Media` 行；`extract` 从伴随目录完整还原 2,300,000,000 字节 |
| 纯空目录（0 文件 / 3 目录） | 仅 `CreateFolder` 表 | `MsiVerifyPackage` OK；`msiexec /i` 会真正创建空目录树 |
| `makecab` 分卷集（400 KB 文件跨 `span1/2/3.cab`） | 3 条相同 `LastSequence` 的 `Media` 行 | `fdintNEXT_CABINET` 串起 3 个 CAB，`extract` 还原出 400,000 字节，sha256 与源文件完全一致 |
| `effects` 往返（`f_manifest.msi` / `t_ca.msi` / `t_cabin.msi` / WiX `merge_ref.msi`） | 全量清单 + 脚本包 | `.reg` / `.cmd` / `.ps1` / `pack.xml` 全部写出；`pack --config` 回填后 `Registry`(4) / `ServiceInstall` / `ServiceControl` / `Environment` / `Shortcut`(2) / `CustomAction` 逐表还原，`MsiVerifyPackage` OK；Binary 型自定义动作的 DLL 从 `Binary` 流导出为 `binaries\*.bin` 并重新嵌入 |
| 自生成 transform（`f_minimal.msi` → `f_basic.msi`） | `transform` + `apply` + `mst --base-msi` | 20 KiB `.mst`（9 张表、43 行删除 / 8 行修改）；`apply` 后逐表与 `f_basic.msi` 完全一致（`transform --dry-run` 报「无差异」）；`mst --base-msi` 列出 19 条 `_TransformView` 变更（含 `DELETE` / `DROP` 行） |
| .NET Desktop Runtime 8.0.11 x64 → 8.0.x x64（各 ~27 MiB） | 真实产品包之间的 diff / apply | 3 秒完成 15 张表、877 新增 / 601 删除 / 7 修改的 diff（含 `_Streams` 负载的 FNV-1a 指纹），104 KiB `.mst`；`apply` 1 秒写出 `MsiVerifyPackage` OK 的新包，产品名 / 版本 / ProductCode 变为目标包；已签名原包被提示需重新签名 |
| 同一 payload 改一个字节后重新 `pack` | 流名相同、内容不同 | `_Streams` 正确报「修改 1」而非「新增 + 删除」（用 Name 作为行身份 + 内容指纹） |
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
- [x] ~~`effects`：注册表 / 服务 / 环境变量 / 快捷方式脚本 + 可回填 `pack.xml`，命令行与 GUI~~
- [x] ~~`transform` / `apply` / `mst`：生成 `.mst`、把 MST 落盘合成新包、查看 `_TransformView` 变更明细~~
- [ ] 工程：自动化单元测试 + GitHub Actions CI（MSVC x64/x86）

欢迎在 issue 中讨论或认领（见 [CONTRIBUTING.md](CONTRIBUTING.md)）。

---

## 已知限制

**解包 / 查看**

- **不执行安装**：不解析条件、不执行 CustomAction、不写注册表；`--transform` 只叠加只读的临时视图，不落盘、不修改原包；`transform` / `apply` 只做数据库层面的差异与应用。
- **transform 的固有边界**：安装程序的 transform 只能新增/覆盖流，**不能删除**包里的 `_Streams` 实体；对已签名包应用 transform 后旧签名流仍在但已失效（需要重新签名）。
- 跨 CAB 连续文件依赖完整的**分卷集**：某个 CAB 缺失时 `extract` 会给出 `CAB set requested cabinet ... which is not staged` / `FDICopy ... aborted` 警告并跳过该分卷集；从分卷集中间的 CAB 开始解包则报 `partial file`，被截断的文件无法还原。
- 外部 CAB 必须在 MSI 同目录（或 `Media.Cabinet` 指定的相对路径）；内嵌 CAB 无此要求。
- `extract` 需要一块与 CAB 总大小相当的临时磁盘空间，结束后自动清理（`--keep-temp` 可保留）。
- 目录还原是**相对布局**：`TARGETDIR` 作为输出根，标准目录属性展开为同名目录，不映射到真实的 `C:\Program Files`。
- 未压缩 administrative image 的源目录结构（source 侧）常与安装布局不同，需保留源结构时用 `--source-layout`。
- 不校验数字签名，不处理加密/受口令保护的包。`.msp` 可以导出补丁 CAB 负载 / transform / 原始流；QFE 型补丁没有 transform 流时，只能用 CAB 内部名（`F_CENTRAL_*` 之类）命名 `payload\` 里的文件，需要真实目录名时必须提供 `--base-msi`（且补丁需带 transform）。
- MSI SQL 是子集：`query` 只支持受限的 `SELECT` 语法，聚合函数等常见 SQL 特性不可用。
- `effects` 是**表级翻译**，不是安装仿真：不执行 CustomAction、不求值条件；`[Property]` 仅在 Property 表有字面值时替换（`.cmd` / `.reg` 里的 `[INSTALLFOLDER]` 额外按 `Directory` 表推导）；`pack.xml` 不重放 UI、Feature、文件树、Transform、签名与 `ServiceControl` 的原始事件位。详见 [安装效果导出](#安装效果导出effects)。

**打包**

- 基础形态是单 Feature、单 CAB 的最小包；快捷方式 / 注册表 / 服务 / 环境变量 / 多 Feature / UI / 签名都通过 XML 清单或对应选项按需生成。自定义动作由 `<customactions>` 声明并嵌入 `Binary` 或引用已安装文件；只有引用**未定义**动作的 `DoAction` 会被拒绝。
- UI 清单只支持自有 XML（不再支持 INI）；`_Validation` 已写入各表列元数据，`msiexec` / `MsiVerifyPackage` 不受影响；项目不内置第三方 ICE/校验器，验证以本工具的 `verify`、`msiexec /a` 与真实 UI 走查为准。
- 向导的交互流程已提供自动化走查：`tools/ui_walkthrough.py` 用真实 `msiexec /i`（per-user，无需管理员）驱动 Windows Installer 对话框——欢迎 → 许可（校验 Next 先禁用、勾选后启用）→ 目标文件夹（打开并取消 BrowseDlg）→ 确认 → 安装 → 完成，然后校验负载并 `/x` 卸载；`--mode repair` / `--mode remove` 会先静默安装再驱动维护向导的修复 / 删除；en / zh-CN 均已实测。
- 安装范围：默认 per-machine（`ALLUSERS=1`，快捷方式落在公共桌面 / 公共开始菜单）；`--per-user` 或清单 `<pack scope="perUser">` 可改为 per-user（不写 `ALLUSERS`，默认装到 `LocalAppDataFolder\<产品名>`，快捷方式在当前用户，子菜单目录的 keypath 组件改用 HKCU）。已完成 `msiexec /i /qn` + `/x` 的安装 / 卸载实测。
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

本项目使用 Windows SDK 提供的头文件与导入库（`Msi.Lib`、`Cabinet.Lib` 等），运行时依赖操作系统自带的 `msi.dll` / `cabinet.dll` / `msxml6.dll`；这些组件归 Microsoft 所有，不受本项目许可证约束。

---

## 致谢与参考

- Microsoft **Windows Installer** SDK：`msi.h` / `msiquery.h`，以及 Windows Installer 文档（<https://learn.microsoft.com/windows/win32/msi/>）
- Microsoft **Cabinet SDK**：`fdi.h`（FDI 解压）与 `fci.h`（FCI 压缩）
- MSVC / Windows SDK 工具链
- 用于回归测试的真实安装包：GitHub CLI、VC++ Runtime、Windows SDK Debuggers、NetLimiter、Windows Installer 缓存包
