# 贡献指南

感谢你愿意改进 msitool。这是一个小型单文件命令行工具，贡献流程尽量轻量，但请保持代码与文档的一致性。参与本项目即表示同意遵守 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。

## 开发环境

| 项目 | 要求 |
| --- | --- |
| OS | Windows 10/11（需系统自带 `msi.dll` / `cabinet.dll`） |
| 编译器 | MSVC，Visual Studio 2017+（推荐 VS 2022） |
| SDK | Windows SDK 10（`msi.h`、`msiquery.h`、`fdi.h`、`fci.h`、`Msi.Lib`、`Cabinet.Lib`） |
| 语言标准 | C++17，`/utf-8`、`/EHsc`、`/W4` |
| 可选 | CMake 3.20+ |

源码是 **UTF-8 无 BOM**，编译必须带 `/utf-8`（`build.bat` 与 `CMakeLists.txt` 已配置）。

```bat
build.bat            :: x64 -> build\msitool_x64.exe
build.bat x86        :: x86 -> build\msitool_x86.exe
build.bat clean
```

CMake（标准 VS 安装）：

```bat
cmake -S . -B build-cmake -G "Visual Studio 17 2022" -A x64
cmake --build build-cmake --config Release
```

非标准 VS 安装可用 NMake Makefiles，见 [README.md](README.md#构建详解)。

## 代码风格

- C++17，不引入第三方库；运行时只允许依赖系统 DLL。
- API 一律使用 **W 版**（`CreateFileW`、`MsiOpenDatabaseW`…），字符串统一 `std::wstring`，对外输出 UTF-8（用 `util::outLine` / `util::errLine`）。
- MSI / FDI / FCI 句柄必须 RAII 管理（`msi::Handle`），不要手写 `MsiCloseHandle`。
- 错误通过 `util::fail` / `util::checkMsi` / `util::checkWin` 抛出 `util::Error`，由 `main()` 统一打印并返回退出码 1。
- 路径一律经过 `util::path::sanitizeComponent` / `sanitizeRelative` 净化，禁止把 MSI 里的名字直接拼接后写盘。
- 目标是 **`/W4` 零警告**；新增未使用的回调参数请显式 `(void)param;`。
- 新增命令 / 选项时，同步更新：`src/main.cpp` 的 `kUsage`、`Cli`、`parseCommandLine`、命令分派，以及本仓库的 `README.md`。

## 测试

目前没有自动化单元测试框架，验证以真实 MSI 样本为准。提交前请至少覆盖：

```bat
:: 查看
build\msitool_x64.exe verify path\to\sample.msi
build\msitool_x64.exe tables path\to\sample.msi
build\msitool_x64.exe list path\to\sample.msi streams
build\msitool_x64.exe query path\to\sample.msi "SELECT `File`,`FileName` FROM `File`"

:: 解包
build\msitool_x64.exe extract path\to\sample.msi C:\out\files

:: 打包 + 往返
build\msitool_x64.exe pack C:\payload C:\out\app.msi --force
build\msitool_x64.exe verify C:\out\app.msi
build\msitool_x64.exe extract C:\out\app.msi C:\out\roundtrip
```

建议用 `sha256` 或文件大小逐项比对 `C:\payload` 与 `C:\out\roundtrip\<ProductName>`；也可以让 Windows Installer 自己验证生成的包：

```bat
msiexec /a C:\out\app.msi /qn TARGETDIR=C:\out\admin
```

负向用例同样重要：非 MSI 文件、目录、伪造 CFB 头、缺失外部 CAB、中文/空格路径、`--dry-run`、`--force`、`--skip-existing`。

改了打包逻辑时，请额外确认 `MsiVerifyPackageW` 仍返回 `ERROR_SUCCESS`；改了解包逻辑时，请用 README「实测过的样本」里的几类包做回归（内嵌 CAB / 外部 CAB / 多 CAB / 未压缩 layout / `.msp`）。

**不要**把 MSI、CAB、EXE 等二进制样本提交到仓库（`.gitignore` 已忽略）。

## 提交信息

使用 Conventional Commits 风格，简短中文描述即可，与现有历史保持一致：

```
feat: 新增 pack 命令，把目录打包成自包含 MSI
fix: 解析 Directory.DefaultDir 的 target:source 语法
docs: 完善 README 的开源项目结构
refactor: ...
test: ...
```

正文里说明「为什么」以及验证方式（跑了哪些样本、结果如何）。

## Pull Request

1. 先开 issue 讨论较大的改动（新命令、行为变更、新增依赖）。
2. 保持 PR 聚焦，避免把无关重构混在一起。
3. 在 PR 描述里写明：改动内容、影响的命令、测试过的样本与结果、以及是否需要更新 README / CHANGELOG。
4. 确认 x64 与 x86 都能编译且零警告。

## 报告 Bug

请尽量提供：

- `msitool --help` 的版本/构建信息（`build.bat` 的架构、MSVC 版本）
- 完整命令行（可隐去敏感路径）
- 完整 stdout / stderr（UTF-8）
- `msitool verify <包>` 的输出
- 相关表片段，例如 `msitool list <包> media --tsv`、`list dirs --tsv`、`list files --tsv`
- 期望行为与实际行为

如果样本涉及私有/受许可软件，**不要上传 MSI**；用脱敏后的表数据描述即可。安全漏洞请按 [SECURITY.md](SECURITY.md) 私下报告。
