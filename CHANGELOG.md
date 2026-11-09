# 变更记录

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 格式，版本号遵循 [SemVer](https://semver.org/lang/zh-CN/)。

尚未发布正式 Release / tag，以下变更均视为 **`Unreleased`**，按类别记录。

## [Unreleased]

### Added

- **GUI · 可视化自定义动作编辑器**：打包页新增第 5 个标签页——左侧动作列表（新增 / 复制 / 删除 / 上移下移 / 清空、状态图标），右侧按类型动态显隐的属性表单（8 种类型 / 9 个标志 / 插入阶段建议序号 / 序号 / 条件），右下方把标准动作与自定义动作按 `InstallExecuteSequence` 混排的时间线（整页布局：该页隐藏运行日志卡，编辑区独占内容区）（自动滚动到当前动作、点击可跳回编辑）；实时 lint（ID 重复、必填来源 / 目标、序号冲突、延迟动作未排序列、Binary 文件缺失）在打包前拦截错误；支持从 XML 清单「导入」、复制 XML 片段、导出可直接 `--config` 的清单。XML 解析与 MSI 编码抽到 `custom_actions.h/.cpp` 由清单加载器和 GUI 共用；编辑器一旦被编辑就以本页列表为准替换清单中的 `<customactions>`，拖放清单时也会自动载入动作。实测「新增 → 打包」生成的 MSI 含 `CustomAction` 行与 4100 序列行，「导入 examples/custom-actions.xml → 复制片段」字段（含 `deferred` / 条件 / 序号）完整往返。
- **打包 · 自定义动作**：清单新增 `<customactions>`（`CustomAction` 表 + 可选 `Binary` 流）与 `<executeSequence>` / `<adminExecuteSequence>` 追加行；`type` 支持 `dll` / `exe` / `dllfile` / `exefile` / `exedir` / `setproperty` / `setdirectory` / `runcommand` 或数字，支持 `deferred` / `commit` / `rollback` / `async` / `continue` / `impersonate` / `taware` / `x64` / `hidetarget`；UI 的 `DoAction` 可直接引用已定义动作，导入 UI 模板时 Binary 类动作随流一起导入。示例见 `examples/custom-actions.xml`。
- **GUI · 快捷方式表格**：打包页「快捷方式」标签页改为可增删行的内联表格（名称 / 位置 / 目标 / 参数 / 图标 / 索引 / 说明），不再使用多行文本；打包时逐行读取。

- **打包 · 安装范围**：`--per-user` / `--all-users`（或清单 `<pack scope="perUser">`）切换 per-user / per-machine；per-user 不写 `ALLUSERS`，默认装到 `LocalAppDataFolder\<产品名>`，快捷方式与子菜单目录的 keypath 组件改用 HKCU；GUI「高级」页新增「安装范围」下拉。实测 `msiexec /i /qn` 后文件、桌面 / 开始菜单快捷方式落在当前用户，`/x` 完整清理。
- **打包 · `_Validation` 元数据**：数据库提交前根据已执行的 DDL 写入 `_Validation` 列元数据（表 / 列 / Nullable），方便 ICE 类工具读取列类型（`_Validation` 表结构按 MSI 标准定义）。
- **测试 · 向导自动走查**：新增 `tools/ui_walkthrough.py`，用真实 `msiexec /i`（per-user，无需管理员）驱动 Windows Installer 对话框：欢迎 → 许可（校验 Next 先禁用、勾选后启用）→ 目标文件夹（打开并取消 BrowseDlg）→ 确认 → 安装 → 完成，然后校验负载并 `/x` 卸载；`--mode repair` / `--mode remove` 先静默安装再驱动维护向导的修复 / 删除；en / zh-CN 均已实测通过。`tools/pack_regression.ps1` 新增 per-user 用例，并在装有 Python 时跑 install / repair / remove / zh-CN 四轮走查。
- **打包 · XML 清单（破坏性）**：`--config` 改为只接受 XML（根 `<msitool>`，命名空间 `https://msitool.dev/schema/pack/v1`），覆盖 `<pack>` / `<features>` / `<shortcuts>` / `<registry>` / `<services>` / `<environment>` / `<sign>` / `<ui>`；删除 INI 解析器，传 `.ini` 会报错并指向 `examples/pack.xml`。新增 `examples/pack.xml` 与 `examples/ui-custom.xml`。
- **打包 · 内置多页向导**：`--ui wizard` 生成 12 个对话框（欢迎 / 许可 / 目标文件夹 + BrowseDlg / 确认 / 进度 / 完成 / 取消 / 错误 / 维护欢迎 / 维护类型），许可页用 `ScrollableText` + `CheckBox` + `ControlCondition`，目录页用 `PathEdit` + `DirectoryList` / `DirectoryCombo`（`_BrowseProperty` + `Indirect`），维护模式用 `Installed` / `NOT Installed` 条件切换，修复 / 删除走 `Reinstall All` / `Remove All`；中英两套文案与字体（`--ui-locale`，1033 → en，2052 / 1028 → zh-CN）；`--dump-ui <dir>` 导出内置 XML / EULA / banner / background。
- **打包 · UI 模板**：`--ui-template <msi>` 读取现有包的 `Dialog` / `Control` / `ControlEvent` / `ControlCondition` / `CheckBox` / `RadioButton` / `ComboBox` / `ListBox` / `ListView` / `TextStyle` / `Binary` / `Error` / `Billboard` / `BBControl` 与 `InstallUISequence` / `AdminUISequence`，只保留标准动作；模板里的自定义动作按类型导入（Binary / 文本类）或丢弃并告警。
- **打包 · 自定义 UI**：XML `<ui base="wizard">` 增量覆盖内置布局（`dialog` 按 id、`control` 按 `(dialog,id)`、`publish` / `controlcondition` 按键合并），支持 `textstyle` / `dialog` / `control` / `publish` / `setproperty` / `controlcondition` / `checkbox` / `binary` / `error` / `billboard` / `uisequence` / `adminuisequence`；生成前校验 `Control_Next` 单环、外键、事件白名单、终止页与序列必需动作。
- **打包 · 快捷方式增强**：`directory` 支持 `desktop` / `startmenu` / `startup` / `sendto` 别名与 `ProgramMenuFolder\厂商\产品` 多级子目录（自动生成 `Directory` + HKLM keypath 组件 + `RemoveFile`），`icon` 支持 `.ico` / `.exe` / `.dll`，新增 `iconIndex` / `advertise` / `workingDirectory` / `MsiShortcutProperty`；CLI 新增 `--desktop-shortcut` / `--start-menu-shortcut` / `--main-exe`。
- **GUI · 打包页**：安装界面下拉扩为 `none` / `basic` / `minimal` / `wizard` / `custom`，新增「UI 模板 (.msi)」「UI 语言」字段与第四个「快捷方式」标签页（主程序、桌面 / 开始菜单勾选、逐行 `名称 | 位置 | 目标 | 参数 | 图标 | 图标索引 | 说明` 编辑器）；拖放清单改为 `.xml`，新增 UI 模板投放区。
- **打包 · 校验元数据**：数据库写入空 `_Validation` 表；`AdminExecuteSequence` 移除不合法的 `CreateFolders`；UI 表覆盖 `CheckBox` / `ControlCondition` / `RadioButton` / `ComboBox` / `ListBox` / `ListView` / `Binary` / `Error` / `Billboard` / `BBControl`。
- **构建**：`resources/msitool.rc`（RCDATA 嵌入向导 XML / EULA / BMP）与 `rc.exe` 步骤，链接 `Msxml6.Lib` / `OleAut32.Lib`；`tools/make_ui_art.ps1` 生成 banner / background / 示例 EULA。

- **设置 · 自定义快捷键**：新增「快捷键」卡片，点击右侧按键框即可重新录制组合键（如把「打开包文件」改成 `Ctrl+P`），Backspace 清除、Esc 取消，冲突与系统保留组合（`Alt+F4`、`Alt+Tab`、`Ctrl+C` 等）就地提示并拒绝，右上角「恢复默认」一键还原。前四项（打开包文件 / 重新载入 / 打开设置 / 切换明暗主题）可在 `HKCU\Software\msitool\Key*` 中持久化，启动与修改后重建加速键表；录制期间临时绕过加速键，保证正在输入的组合不会被已有快捷键抢走。

- **查看**：`verify`（包预检报告 + `MsiVerifyPackageW`）、`info`、`tables`、`schema`、`query`、`list`、`cabs`、`export`。
- **解包**：`extract`，支持内嵌 CAB（`_Streams`）、外部 CAB、未压缩 administrative layout；`--flat`、`--dry-run`、`--skip-existing`、`--source-layout`、`--keep-temp`。
- **解包 · 跨 CAB 连续文件**：用 `FDIIsCabinet` 把 `hasnext/hasprev` + `setID` + `iCabinet` 连成一套的 CAB 分为分卷集，一组只跑一次 `FDICopy`；`fdintNEXT_CABINET` 回调把下一个 CAB 交回 FDI，跨 CAB 的文件写成一个完整文件。
- **解包 · transform 视图**：`--transform <file.mst>`（可重复）用 `MsiDatabaseApplyTransformW` 在只读句柄上叠加临时视图，`verify` / `info` / `tables` / `schema` / `query` / `list` / `extract` 都读 transform 后的表。
- **解包 · .msp**：`extract <file.msp>` 分类 `_Streams`（CAB / transform / 原始流），FDI 解出 `payload\`、导出 `transforms\*.mst` 与 `streams\`、写 `manifest.txt`；`--base-msi <file.msi>` 把补丁 transform 应用到目标 MSI 临时视图并在 `layout\` 下按真实目录名还原。
- **打包**：`pack`，用 FCI 生成 MSZIP CAB 并用 Msi API 建库，产出自包含 MSI；支持 `--name` / `--version` / `--manufacturer` / `--product-code` / `--upgrade-code` / `--platform` / `--language` / `--force` / `--dry-run`。
- **打包 · 安装位置**：`--install-dir <path>`（清单 `<pack installDir>`、GUI「基本」页「安装目录」）把源目录映射到指定位置：`App` / `Tools\App` 相对 Program Files，`LocalAppDataFolder\App`、`CommonAppDataFolder\Vendor\App` 等标准目录前缀按其关键目录键展开，中间层自动生成 `Directory` 行；绝对路径（`D:\Apps\App`、`\\server\share\App`）不能写进 `Directory.DefaultDir`（`:` 是 target:source 分隔符，实测 `/a` 报 2714、`/i` 会建出名叫 `D` 的文件夹），改按 MSI 文档用一条 `Property` 行把 `INSTALLFOLDER` 属性设成完整路径完成钉位，实测装到该目录；`INSTALLFOLDER` 仍是安装根，命令行 `INSTALLFOLDER=` 可覆盖，`/a` 与 `extract` 仍还原相对树。
- **打包 · 分卷**：`--cab-size`，按未压缩字节把文件切到多个内嵌 CAB（`Media` / `_Streams` 一一对应），`extract` 与 `msiexec /a` 均可还原。
- **打包 · 大文件**：`--large-files layout|error`，> 2 GiB 的文件以未压缩伴随 layout 文件保存（空 `Cabinet` 的 `Media` 行 + `File.Attributes=8192`），`extract` 从伴随目录复制还原；`error` 恢复直接拒绝。
- **打包 · 空目录**：默认写 `CreateFolder` 表并在安装 / 管理安装序列里保留 `CreateFolders` 动作，`--no-empty-dirs` 可关闭。
- **打包 · manifest**：`--config <file.xml>`，XML 清单支持多 Feature、快捷方式（含 `Icon`）、注册表、服务（`ServiceInstall` + `ServiceControl`）、环境变量、`<pack>` 默认值；相应标准动作已加入 `InstallExecuteSequence`。完整示例见 `examples/pack.xml`。
- **打包 · UI**：`--ui none|basic|minimal`；`minimal` 生成 `InstallUISequence`、`Dialog`、`Control`、`ControlEvent`、`TextStyle` 与内置欢迎 / 进度 / 完成向导（`wizard` 多页向导与 XML 自定义 UI 见上）。
- **打包 · 签名**：`--sign-cert` / `--sign-password` / `--sign-timestamp` / `--sign-tool`，提交后调用 Windows SDK `signtool.exe` 写入 `\x05DigitalSignature` 流，并再次自检。
- **包验证**：所有命令打开数据库前检查 OLE 复合文件签名、`MsiOpenDatabaseW` 与 `_Tables` 结构；`verify` 输出完整报告并以退出码区分 VALID / INVALID。
- **构建**：`build.bat`（x64/x86/clean，自动定位 vcvars）与 `CMakeLists.txt`（VS / NMake 生成器）。
- **图形界面（Fluent UI）**：新增 `src/ui.h` / `ui_draw.cpp` / `ui_widgets.cpp` / `ui_app.cpp`，纯 Win32 API（user32 / gdi32 / shell32 / ole32 / advapi32）自绘，无任何第三方 UI 库。包含九个页面（概览 / 包信息 / 数据表 / SQL 查询 / 解包 / 导出 / 打包 / 验证 / 设置）、亮暗主题与跟随系统、Per-Monitor V2 高 DPI、圆角卡片与矢量图标、数据网格 / 下拉 / 开关 / 进度条 / 日志 / Toast、拖放打开、后台任务与实时日志。
- **GUI · 打包页拖放（文件 / 文件夹）**：拖放按**落点**路由到字段——「源目录」收目录（拖文件则取其所在目录，多项目取最深共同目录）并自动补齐产品名与输出路径；「输出 MSI」收 `.msi` 作为目标，收目录则在其下按产品名生成 `<产品名>.msi`；「配置文件」收 XML 清单，拖目录会在其中找 `pack.xml` / `msitool.xml` / `manifest.xml`；「签名证书」收 `.pfx` / `.p12`；落在表单 / 页面空白处则按内容自动分配，需要时会自己切到对应标签页。解包页「解包到」与导出页「输出目录」同样可接目录（并记入「最近」）；字段以外的空白处按内容在两条流程间路由（文件夹 → 打包源目录，`.msi` / `.msp` → 查看 / 解包）。实现上改用 ole32 `RegisterDragDrop` + `IDropTarget`（`WM_DROPFILES` 保留为旧式拖放的兜底路径），拖动过程中就能拿到光标位置：命中字段描边高亮、表单卡片加边框与「拖放自动分配」标签、Toast 粘滞显示「松开：设为…」；只读路径所以只报告 `DROPEFFECT_COPY`（绝不返回 `MOVE` / `LINK`，不会移动或删除源文件），每次拖放写一行 `[drop]` 日志且不自动开始打包。
- **解包 · 选择存放目录**：命令栏下方新增常驻「解包到」位置条（可直接编辑、点「浏览…」用系统文件夹对话框选择、「最近」下拉复用最近 6 个目录、点「打开」跳转），输出目录不再藏在折叠选项里；对话框默认定位到当前输出目录，选择结果与每次解包使用的目录都会记入最近列表（`HKCU\Software\msitool\RecentOutDirs`）。
- **解包 · 整页资源管理器**：解包页去掉重复的全局「当前包文件」栏与页面大标题，以文件浏览器为主体：命令 / 地址栏（后退、前进、上一级、刷新、可点击面包屑、搜索、解包全部 / 解包选中、选项、日志）+ 左侧导航树 + 右侧详细信息列表（文件夹与文件混排、图标、类型列、点击表头排序、双击进入目录 / 双击单文件解包、右键菜单、Backspace 返回上级、F5 刷新、状态栏），选项面板与日志抽屉默认折叠，主体区域随窗口全量自适应。
- **解包 · 资源管理器式浏览**：GUI 解包页新增基于 `Directory` / `File` 表的目录树 + 文件列表（关键字筛选、已解包状态、双击展开/折叠），支持双击单文件解包、右键菜单（解包此文件 / 解包到… / 解包此目录（递归）/ 复制相对路径 / 复制文件键 / 在资源管理器中显示）、「解包全部 / 解包选中」按钮与输出目录自动填充（`<包名>_files`）。新增 `extract::browse()` 目录清单接口与 `extract::Options::onlyFiles` 选择性解包：只解选中文件，未选中的 CAB 不再被解压到 `_unmapped`。
- **入口**：同一个 exe 支持两种模式 —— 无参数（或 `gui` / `--gui` / `/gui [file]`）启动 GUI，带命令参数时在终端里以原样 CLI 运行；`util::outLine` / `errLine` 增加可选日志钩子供 GUI 捕获输出。
- **文档**：README、CONTRIBUTING、SECURITY、LICENSE 与 issue 模板。
- **设置 · 关于**：新增项目信息面板，展示版本号、开源协议、项目主页、构建信息（日期 / MSVC 版本 / 架构）、命令行入口与运行时依赖；项目主页是可点击的超链接（手形光标，点击 / Enter / Space 用默认浏览器打开）。
- **GUI 高 DPI · 可测性**：`MSITOOL_DPI=<72..480>` 环境变量可覆盖进程 DPI，单显示器机器上即可验证任意缩放（100%–300%）；`tools/dpi_probe.py` 自动启动窗口、打印 DPI 感知上下文、`PrintWindow` 截图，并可用 `--transition` 向主窗口发送 `WM_DPICHANGED` 伪造一次跨显示器切换，走真实的「重建主题 → 刷新控件 → 重排布局」路径。

### Fixed

- **查看 / 查询：超过 511 字符的 LONGCHAR 被读成空**：`MsiRecordGetStringW` 首次用 512 字符缓冲区读取，遇到 `ERROR_MORE_DATA` 后虽然放大了缓冲区，却仍用旧长度当作容量，第二次读取继续失败并返回空串。现在按 API 返回的所需长度循环扩容（`msidb.cpp`），`Control.Text`（许可协议 RTF）、`Property.Value`、`Error.Text` 等长字段都能完整显示；内置向导的许可页也因此能真正显示 RTF。
- **打包：`AdminExecuteSequence` 包含不合法的 `CreateFolders`**：MSI 标准不允许该动作出现在管理安装序列；已移除，`/a` 仍能还原目录树。
- **GUI 高 DPI · 跨显示器后表格列宽 / 行高不跟随缩放**：网格的列宽是用「数据到达时的 DPI」测量出来的像素值，行高（`rowHeightPx = S(28)`）也是启动时算好的一次性常量；窗口被拖到另一块缩放比例不同的显示器（`WM_DPICHANGED`）后，布局会重排，但网格仍用旧像素值 → 每列过窄、文本一律被省略号截断，行高被压扁。现在 `DataGrid` 记住当前列宽是按哪个 DPI 拟合的，检测到变化时自动列宽重新测量、显式列宽按比例缩放，密排行高改在每次 `layout()` 重新计算；`WM_DPICHANGED` 改为使用 wParam 携带的新 DPI 重建主题（不再重复 `rebuild` 两次）。附 `tools/dpi_probe.py` 截图 / 伪造切换脚本，实测 96 → 144 切换后的表格与原生的 144 启动完全一致。
- `layout()` 增加控件尚未创建的空指针保护（`WM_SIZE` 可能早于 `onCreate()` 到达）。

- GUI Toast 通知不随窗口移动：Toast 是独立的置顶 popup（不属于主窗口的子控件），此前只在显示 / 重新布局时定位一次，拖动标题栏移动窗口后就留在原地。现在处理 `WM_MOVE`，窗口每次移动都会重新锚定 Toast；缩放 / 最大化仍走原有的 `layout()` 路径。

- GUI / CLI `pack --ui minimal` 生成的 MSI 无法安装：`WelcomeDlg` 的 `Control_Next` 指针不成环（`Description` 与 `Cancel` 都指向 `Next`），Windows Installer 创建对话框时报 2810「next control pointers do not form a cycle」并中止安装（`MsiVerifyPackage` 检测不到这类 UI 表错误）。现在每个对话框的控件链改为单一环 `Title → Description → … → Title`，并补上进度对话框缺失的 `ActionText` / `SetProgress` 订阅，安装进度与动作文本可正常刷新。

- GUI 拖放把「解包」与「打包」混在一起：在非打包页拖入文件夹只会提示「仅支持 `.msi` / `.msp`」，而「打包」页的「当前包文件」栏又始终显示上一个为解包打开的包。现在字段以外的落点按内容严格分流——文件夹永远作为打包源目录（必要时自动切到「打包」页并补齐产品名 / 输出路径），`.msi` / `.msp` 永远属于查看 / 解包；「打包」页不再打开安装包，也不再显示「当前包文件」栏，若在打包页显式打开包（Ctrl+O 或打开按钮）会切到「包信息」页。

- GUI「运行日志」卡片副标题小字被下方日志框遮挡：解包页日志框从 `S(40)`、打包页从 `S(44)` 开始，早于卡片头部副标题的结束位置 `S(48)`；现统一钳制到 `S(54)` 并同步调整日志框高度，副标题绘制高度也收紧为 `S(16)` 留出间隙。

- GUI Toast 通知会被内容区控件 / 卡片重绘遮挡：改为独立置顶 `WS_POPUP` 浮层（`WS_EX_TOPMOST | WS_EX_NOACTIVATE`，随窗口尺寸重新定位），所有子窗口补上 `WS_CLIPSIBLINGS`；隐藏时重绘下方控件，避免残影。
- 折叠「选项」面板时第三行开关溢出面板、压到文件列表：开关改为 3 列 × 2 行并留足面板高度。

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

- GUI 解包页「日志」抽屉展开后由固定 `S(150)` 改为按页面高度自适应（约 38%，上限 `S(380)`）：日志内容区在默认窗口从 ~4 行提升到 ~8 行、最大化 ~10 行；同时保证文件浏览器至少保留 `S(190)`，选项与日志同时展开也不挤压重叠。

- 输出统一 UTF-8，表格支持 `--tsv`；警告与详细日志输出到 stderr。
- 生成的 MSI 同时包含 `InstallExecuteSequence` 与 `AdminExecuteSequence`，使 `msiexec /i` 与 `msiexec /a` 都能处理。
- README 的「路线图」改为带勾选进度的 TODO 清单，已完成项划掉；两条 `pack` TODO 与 `extract` TODO 均已完成并划掉。
- `extract` 的 CAB 暂存名改为 `Media.Cabinet` 的文件名（分卷集需要），独立 CAB 行为不变；输出行现在按分卷集打印，会列出集合里的全部 CAB。
- `pack` 默认不再因单个文件 > 2 GiB 直接失败，改为伴随 layout 文件；空目录默认在安装时保留。
- `InstallExecuteSequence` / `AdminExecuteSequence` 补齐资源相关的标准动作与 `CreateFolders`。

---

## 发布约定（暂定）

打 tag 时把 `Unreleased` 的内容归入新版本（例如 `0.1.0`），补充日期、变更摘要与对应的构建产物校验值；格式参考本文件顶部链接。
