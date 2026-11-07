# 变更记录

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 格式，版本号遵循 [SemVer](https://semver.org/lang/zh-CN/)。

尚未发布正式 Release / tag，以下变更均视为 **`Unreleased`**，按类别记录。

## [Unreleased]

### Added

- **查看**：`verify`（包预检报告 + `MsiVerifyPackageW`）、`info`、`tables`、`schema`、`query`、`list`、`cabs`、`export`。
- **解包**：`extract`，支持内嵌 CAB（`_Streams`）、外部 CAB、未压缩 administrative layout；`--flat`、`--dry-run`、`--skip-existing`、`--source-layout`、`--keep-temp`。
- **打包**：`pack`，用 FCI 生成 MSZIP CAB 并用 Msi API 建库，产出自包含 MSI；支持 `--name` / `--version` / `--manufacturer` / `--product-code` / `--upgrade-code` / `--platform` / `--language` / `--force` / `--dry-run`。
- **打包 · 分卷**：`--cab-size`，按未压缩字节把文件切到多个内嵌 CAB（`Media` / `_Streams` 一一对应），`extract` 与 `msiexec /a` 均可还原。
- **打包 · 大文件**：`--large-files layout|error`，> 2 GiB 的文件以未压缩伴随 layout 文件保存（空 `Cabinet` 的 `Media` 行 + `File.Attributes=8192`），`extract` 从伴随目录复制还原；`error` 恢复直接拒绝。
- **打包 · 空目录**：默认写 `CreateFolder` 表并在安装 / 管理安装序列里保留 `CreateFolders` 动作，`--no-empty-dirs` 可关闭。
- **打包 · manifest**：`--config <file>`，INI 清单支持多 Feature、快捷方式（含 `Icon`）、注册表、服务（`ServiceInstall` + `ServiceControl`）、环境变量、`[pack]` 默认值；相应标准动作已加入 `InstallExecuteSequence`。完整示例见 `examples/pack.ini`。
- **打包 · UI**：`--ui none|basic|minimal`；`minimal` 生成 `InstallUISequence`、`Dialog`、`Control`、`ControlEvent`、`TextStyle` 与内置欢迎 / 进度 / 完成向导。
- **打包 · 签名**：`--sign-cert` / `--sign-password` / `--sign-timestamp` / `--sign-tool`，提交后调用 Windows SDK `signtool.exe` 写入 `\x05DigitalSignature` 流，并再次自检。
- **包验证**：所有命令打开数据库前检查 OLE 复合文件签名、`MsiOpenDatabaseW` 与 `_Tables` 结构；`verify` 输出完整报告并以退出码区分 VALID / INVALID。
- **构建**：`build.bat`（x64/x86/clean，自动定位 vcvars）与 `CMakeLists.txt`（VS / NMake 生成器）。
- **文档**：README、CONTRIBUTING、SECURITY、LICENSE 与 issue 模板。

### Fixed

- 解析 `Directory.DefaultDir` 的 `target:source` 双路径语法：未压缩 administrative layout 中 `.:x64`、`.:LANGS-~1|Langs-x64` 之类目录不再被误解析成 `._x64`，源文件能正确定位（NetLimiter x64 样例 147 个 `No source data` 警告 → 0）。
- 别名映射只加入文件所属的 CAB，避免跨磁盘把文件写到错误目录。
- `--skip-existing` 跳过的文件不再被误报为「缺少源文件」。
- 表格输出转义控制字符（如 MSI 内部流名 `\x05DigitalSignature`）。
- `tables` 的行数改为通过 `MsiViewFetch` 计数（MSI SQL 不支持 `COUNT(*)`）。
- `.msp` 使用 `MSIDBOPEN_PATCHFILE` 伪指针打开（msi.dll 实际要求值 32，SDK 宏在 Unicode 下算出 16）。
- `export` / `cabs` 在正斜杠路径下 `MsiDatabaseExportW` 报 161「路径无效」的问题（入口统一用 `GetFullPathNameW` 规范化）。
- 盘符根路径（`C:`）的递归建目录处理。
- `list streams` 对 `_Streams` 的 `Data` 列显示流大小而不是读入全部数据。

### Changed

- 输出统一 UTF-8，表格支持 `--tsv`；警告与详细日志输出到 stderr。
- 生成的 MSI 同时包含 `InstallExecuteSequence` 与 `AdminExecuteSequence`，使 `msiexec /i` 与 `msiexec /a` 都能处理。
- README 的「路线图」改为带勾选进度的 TODO 清单，已完成项划掉；两条 `pack` TODO 均已完成并划掉。
- `pack` 默认不再因单个文件 > 2 GiB 直接失败，改为伴随 layout 文件；空目录默认在安装时保留。
- `InstallExecuteSequence` / `AdminExecuteSequence` 补齐资源相关的标准动作与 `CreateFolders`。

---

## 发布约定（暂定）

打 tag 时把 `Unreleased` 的内容归入新版本（例如 `0.1.0`），补充日期、变更摘要与对应的构建产物校验值；格式参考本文件顶部链接。
