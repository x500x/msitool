# 变更记录

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 格式，版本号遵循 [SemVer](https://semver.org/lang/zh-CN/)。

尚未发布正式 Release / tag，以下变更均视为 **`Unreleased`**，按类别记录。

## [Unreleased]

### Added

- **查看**：`verify`（包预检报告 + `MsiVerifyPackageW`）、`info`、`tables`、`schema`、`query`、`list`、`cabs`、`export`。
- **解包**：`extract`，支持内嵌 CAB（`_Streams`）、外部 CAB、未压缩 administrative layout；`--flat`、`--dry-run`、`--skip-existing`、`--source-layout`、`--keep-temp`。
- **解包 · 跨 CAB 连续文件**：用 `FDIIsCabinet` 把 `hasnext/hasprev` + `setID` + `iCabinet` 连成一套的 CAB 分为分卷集，一组只跑一次 `FDICopy`；`fdintNEXT_CABINET` 回调把下一个 CAB 交回 FDI，跨 CAB 的文件写成一个完整文件。
- **解包 · transform 视图**：`--transform <file.mst>`（可重复）用 `MsiDatabaseApplyTransformW` 在只读句柄上叠加临时视图，`verify` / `info` / `tables` / `schema` / `query` / `list` / `extract` 都读 transform 后的表。
- **解包 · .msp**：`extract <file.msp>` 分类 `_Streams`（CAB / transform / 原始流），FDI 解出 `payload\`、导出 `transforms\*.mst` 与 `streams\`、写 `manifest.txt`；`--base-msi <file.msi>` 把补丁 transform 应用到目标 MSI 临时视图并在 `layout\` 下按真实目录名还原。
- **打包**：`pack`，用 FCI 生成 MSZIP CAB 并用 Msi API 建库，产出自包含 MSI；支持 `--name` / `--version` / `--manufacturer` / `--product-code` / `--upgrade-code` / `--platform` / `--language` / `--force` / `--dry-run`。
- **打包 · 分卷**：`--cab-size`，按未压缩字节把文件切到多个内嵌 CAB（`Media` / `_Streams` 一一对应），`extract` 与 `msiexec /a` 均可还原。
- **打包 · 大文件**：`--large-files layout|error`，> 2 GiB 的文件以未压缩伴随 layout 文件保存（空 `Cabinet` 的 `Media` 行 + `File.Attributes=8192`），`extract` 从伴随目录复制还原；`error` 恢复直接拒绝。
- **打包 · 空目录**：默认写 `CreateFolder` 表并在安装 / 管理安装序列里保留 `CreateFolders` 动作，`--no-empty-dirs` 可关闭。
- **打包 · manifest**：`--config <file>`，INI 清单支持多 Feature、快捷方式（含 `Icon`）、注册表、服务（`ServiceInstall` + `ServiceControl`）、环境变量、`[pack]` 默认值；相应标准动作已加入 `InstallExecuteSequence`。完整示例见 `examples/pack.ini`。
- **打包 · UI**：`--ui none|basic|minimal`；`minimal` 生成 `InstallUISequence`、`Dialog`、`Control`、`ControlEvent`、`TextStyle` 与内置欢迎 / 进度 / 完成向导。
- **打包 · 签名**：`--sign-cert` / `--sign-password` / `--sign-timestamp` / `--sign-tool`，提交后调用 Windows SDK `signtool.exe` 写入 `\x05DigitalSignature` 流，并再次自检。
- **包验证**：所有命令打开数据库前检查 OLE 复合文件签名、`MsiOpenDatabaseW` 与 `_Tables` 结构；`verify` 输出完整报告并以退出码区分 VALID / INVALID。
- **构建**：`build.bat`（x64/x86/clean，自动定位 vcvars）与 `CMakeLists.txt`（VS / NMake 生成器）。
- **图形界面（Fluent UI）**：新增 `src/ui.h` / `ui_draw.cpp` / `ui_widgets.cpp` / `ui_app.cpp`，纯 Win32 API（user32 / gdi32 / shell32 / ole32 / advapi32）自绘，无任何第三方 UI 库。包含九个页面（概览 / 包信息 / 数据表 / SQL 查询 / 解包 / 导出 / 打包 / 验证 / 设置）、亮暗主题与跟随系统、Per-Monitor V2 高 DPI、圆角卡片与矢量图标、数据网格 / 下拉 / 开关 / 进度条 / 日志 / Toast、拖放打开、后台任务与实时日志。
- **解包 · 选择存放目录**：命令栏下方新增常驻「解包到」位置条（可直接编辑、点「浏览…」用系统文件夹对话框选择、「最近」下拉复用最近 6 个目录、点「打开」跳转），输出目录不再藏在折叠选项里；对话框默认定位到当前输出目录，选择结果与每次解包使用的目录都会记入最近列表（`HKCU\Software\msitool\RecentOutDirs`）。
- **解包 · 整页资源管理器**：解包页去掉重复的全局「当前包文件」栏与页面大标题，以文件浏览器为主体：命令 / 地址栏（后退、前进、上一级、刷新、可点击面包屑、搜索、解包全部 / 解包选中、选项、日志）+ 左侧导航树 + 右侧详细信息列表（文件夹与文件混排、图标、类型列、点击表头排序、双击进入目录 / 双击单文件解包、右键菜单、Backspace 返回上级、F5 刷新、状态栏），选项面板与日志抽屉默认折叠，主体区域随窗口全量自适应。
- **解包 · 资源管理器式浏览**：GUI 解包页新增基于 `Directory` / `File` 表的目录树 + 文件列表（关键字筛选、已解包状态、双击展开/折叠），支持双击单文件解包、右键菜单（解包此文件 / 解包到… / 解包此目录（递归）/ 复制相对路径 / 复制文件键 / 在资源管理器中显示）、「解包全部 / 解包选中」按钮与输出目录自动填充（`<包名>_files`）。新增 `extract::browse()` 目录清单接口与 `extract::Options::onlyFiles` 选择性解包：只解选中文件，未选中的 CAB 不再被解压到 `_unmapped`。
- **入口**：同一个 exe 支持两种模式 —— 无参数（或 `gui` / `--gui` / `/gui [file]`）启动 GUI，带命令参数时在终端里以原样 CLI 运行；`util::outLine` / `errLine` 增加可选日志钩子供 GUI 捕获输出。
- **文档**：README、CONTRIBUTING、SECURITY、LICENSE 与 issue 模板。

### Fixed

- `DataGrid` 在窗口尺寸变化后未夹紧滚动位置，导致行被跳过、点击命中错行（解包页目录树右键会选中错误目录）：`setRect` 时重新钳制 `scrollX/scrollY`。

- 解析 `Directory.DefaultDir` 的 `target:source` 双路径语法：未压缩 administrative layout 中 `.:x64`、`.:LANGS-~1|Langs-x64` 之类目录不再被误解析成 `._x64`，源文件能正确定位（NetLimiter x64 样例 147 个 `No source data` 警告 → 0）。
- 别名映射只加入文件所属的 CAB，避免跨磁盘把文件写到错误目录。
- `--skip-existing` 跳过的文件不再被误报为「缺少源文件」。
- 表格输出转义控制字符（如 MSI 内部流名 `\x05DigitalSignature`）。
- `tables` 的行数改为通过 `MsiViewFetch` 计数（MSI SQL 不支持 `COUNT(*)`）。
- `.msp` 使用 `MSIDBOPEN_PATCHFILE` 伪指针打开（msi.dll 实际要求值 32，SDK 宏在 Unicode 下算出 16）。
- `export` / `cabs` 在正斜杠路径下 `MsiDatabaseExportW` 报 161「路径无效」的问题（入口统一用 `GetFullPathNameW` 规范化）。
- 盘符根路径（`C:`）的递归建目录处理。
- `list streams` 对 `_Streams` 的 `Data` 列显示流大小而不是读入全部数据。
- GUI 窗口首次显示时残留系统标准标题栏 / 边框（此前要最小化再恢复才消失）：`WM_NCCALCSIZE` 在 `wParam == FALSE` 时也返回 0，并在首次显示前用 `SWP_FRAMECHANGED` 强制重算非客户区。
- GUI 最小尺寸在高 DPI 下超过工作区，恢复窗口时底部被任务栏遮住：最小尺寸改为受工作区限制。
- GUI 打包页在默认窗口尺寸下字段挤成一团、与日志卡重叠且不随窗口缩放：改为「基本 / 高级 / 签名」三个标签页（Widget 增加 tab 分组），行距、控件高度、列宽与日志高度全部按客户区尺寸自适应计算，「开始打包」移到标签栏右侧常驻。
- GUI 包信息页信息过多且三块窄表并列、不便查看：重排为「概览头部（产品名 / 版本 / 大小 / 表数 / CAB / 流）+ 摘要 / 属性 / 媒体分区 + 全宽表格 + 关键字筛选 + 选中行详情」，属性区改为 Property 表全部键值，信息表格使用更紧凑行高。

### Changed

- 输出统一 UTF-8，表格支持 `--tsv`；警告与详细日志输出到 stderr。
- 生成的 MSI 同时包含 `InstallExecuteSequence` 与 `AdminExecuteSequence`，使 `msiexec /i` 与 `msiexec /a` 都能处理。
- README 的「路线图」改为带勾选进度的 TODO 清单，已完成项划掉；两条 `pack` TODO 与 `extract` TODO 均已完成并划掉。
- `extract` 的 CAB 暂存名改为 `Media.Cabinet` 的文件名（分卷集需要），独立 CAB 行为不变；输出行现在按分卷集打印，会列出集合里的全部 CAB。
- `pack` 默认不再因单个文件 > 2 GiB 直接失败，改为伴随 layout 文件；空目录默认在安装时保留。
- `InstallExecuteSequence` / `AdminExecuteSequence` 补齐资源相关的标准动作与 `CreateFolders`。

---

## 发布约定（暂定）

打 tag 时把 `Unreleased` 的内容归入新版本（例如 `0.1.0`），补充日期、变更摘要与对应的构建产物校验值；格式参考本文件顶部链接。
