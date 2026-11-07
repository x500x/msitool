# msitool

一个用 **MSVC + C++ + Windows Installer (Msi) API** 编写的命令行工具，用于**查看**和**解包** MSI 安装包。纯 Win32 / Unicode，运行时只依赖系统自带的 `msi.dll` 和 `cabinet.dll`，不需要 WiX、Orca、7-Zip，也不需要管理员权限或真正安装任何东西。

| 命令 | 作用 |
| --- | --- |
| `info` | 摘要信息（SummaryInformation）、关键属性、Media/CAB 清单 |
| `tables` | 列出所有表、是否持久表、行数 |
| `schema <表>` | 列定义、MSI 类型、主键 |
| `query <SQL>` | 执行任意 MSI SQL 查询（只读） |
| `list <名称>` | 查看常用表或任意表（files / components / features / dirs / media / properties / streams / cabs / …） |
| `cabs <目录>` | 导出内嵌/外部 CAB + `manifest.txt` |
| `export <目录>` | 导出全部表为 `.idt`、全部原始流、Binary 表、`summary.txt` |
| `extract <目录>` | **解包**：还原安装目录树，把 CAB 里的文件直接解压到目标路径 |

`.msp` 补丁包也能打开查看（`info`/`tables`/`query`/`export`/`cabs`），其内容是 transform 流而不是 MSI 目录树，所以 `extract` 只面向 `.msi`。

---

## 编译

依赖：Visual Studio 2017+ 的 MSVC 工具链、Windows SDK（提供 `msi.h`、`msiquery.h`、`fdi.h`、`Msi.Lib`、`Cabinet.Lib`）。这些都在标准 Windows SDK 中。

### 方式一：build.bat（推荐）

```bat
cd E:\work\winmsi
build.bat            :: x64 -> build\msitool_x64.exe
build.bat x86        :: x86 -> build\msitool_x86.exe
build.bat clean      :: 清理构建产物
```

脚本会自动查找 `vcvars64.bat` / `vcvars32.bat`：先检查是否已处于 Developer Command Prompt，再用 `vswhere.exe`，再扫描常见安装目录。若 VS 装在特殊位置：

```bat
set MSIT_VCVARS=D:\vs\Community\VC\Auxiliary\Build\vcvars64.bat
build.bat
```

核心编译命令等价于：

```bat
cl /std:c++17 /EHsc /W4 /O2 /utf-8 /DUNICODE /D_UNICODE ^
   /Isrc /Fobuild\obj\x64\ /Febuild\msitool_x64.exe ^
   src\util.cpp src\msidb.cpp src\cab.cpp src\extract.cpp src\main.cpp ^
   Msi.Lib Cabinet.Lib Shell32.Lib /link /MACHINE:X64
```

### 方式二：CMake

标准 VS 安装：

```bat
cmake -S . -B build-cmake -G "Visual Studio 17 2022" -A x64
cmake --build build-cmake --config Release
```

若 VS 是绿色/非标准安装（CMake 找不到实例），在 Native Tools Command Prompt 里用 NMake：

```bat
cmake -S . -B build-cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
```

---

## 使用

```bat
msitool --help
msitool info setup.msi
msitool tables setup.msi
msitool schema setup.msi File
msitool query setup.msi "SELECT `File`,`FileName`,`FileSize` FROM `File`"
msitool list setup.msi files --tsv > files.tsv
msitool list setup.msi streams
msitool cabs setup.msi C:\out\cabs
msitool export setup.msi C:\out\archive
msitool extract setup.msi C:\out\files --verbose
```

`extract` 的常用选项：

| 选项 | 说明 |
| --- | --- |
| `--flat` | 不还原目录层次，所有文件平铺到输出目录（重名自动加序号） |
| `--dry-run` | 只列出计划（CAB 内容会被枚举但不写文件） |
| `--skip-existing` | 跳过已存在的目标文件 |
| `--keep-temp` | 保留暂存 CAB 的临时目录，便于排查 |
| `--tsv` / `-v` | 表格 TSV 输出 / 详细日志（stderr） |

输出统一为 **UTF-8**（`SetConsoleOutputCP(CP_UTF8)` + 自己按 UTF-8 写 stdout），重定向到文件也是 UTF-8；表格里的控制字符会显示为 `\xNN` 转义，避免出现 MSI 内部流名 `\x05DigitalSignature` 之类的乱码。

`export` 的输出结构：

```
<outdir>\tables\<Table>.idt    每张持久表的文本归档（MsiDatabaseExportW）
<outdir>\streams\<name>        _Streams 里的所有原始流（内嵌 CAB 也在这里）
<outdir>\binary\<name>         Binary 表条目（图标、自定义操作 DLL 等）
<outdir>\summary.txt           SummaryInformation + Property 表（UTF-8）
```

> `MsiDatabaseExportW` 导出含二进制列的表（如 `Binary`）时，会在同目录额外生成同名子目录存放流，这是 MSI API 的正常行为。

---

## 实现原理

**查看侧（全部走 Msi API，只读打开）**

- `MsiOpenDatabaseW(path, MSIDBOPEN_READONLY, &h)`；对 `.msp` 使用 `MSIDBOPEN_PATCHFILE` 伪指针（msi.dll 实际要求值 32，SDK 宏在 Unicode 下算出 16，代码两种都试）。
- `MsiDatabaseOpenViewW` + `MsiViewExecute` + `MsiViewFetch` 逐行取数据，`MsiViewGetColumnInfo(MSICOLINFO_NAMES/TYPES)` 拿列名与类型，`MsiRecordGetStringW` / `MsiRecordGetInteger` / `MsiRecordDataSize` 渲染单元格。
- `MsiGetSummaryInformationW` + `MsiSummaryInfoGetPropertyW` 读取 PID_TITLE / PID_TEMPLATE / PID_REVNUMBER(PackageCode) 等。
- `MsiDatabaseExportW` 导出 `.idt`；`MsiRecordReadStream` 读取 `_Streams` / `Binary` 的二进制流。
- MSI SQL 是 SQL 的**子集**：没有 `COUNT(*)`，`tables` 的行数通过 `MsiViewFetch` 计数得到。

**解包侧**

1. 读 `Media` 表：`Cabinet` 以 `#` 开头表示流名（内嵌 CAB，从 `_Streams` 读出）；否则是外部 CAB 文件名，按 MSI 所在目录查找；为空则是未压缩 layout。
2. 按 `Media.LastSequence` 升序划分 `File.Sequence` 区间，确定每个文件属于哪个 CAB。
3. 读 `Directory`（`DefaultDir` 的 `short|long` 取长名、`.` 表示与父目录相同、`[Property]` 展开）、`Component.Directory_`、`File.FileName`，还原相对目录树；`TARGETDIR` 映射到输出根目录。
4. 把 CAB 暂存到一个短的临时目录（规避 FDI 的 256 字节路径限制，同时支持中文路径），再调用 `cabinet.dll` 的 **FDI**（`FDICreate` / `FDICopy`）。在 `fdintCOPY_FILE` 回调里按 CAB 内文件名查表，直接用 `CreateFileW` 打开最终目标路径，FDI 把数据写进去；`fdintCLOSE_FILE_INFO` 关闭句柄并恢复文件时间。CAB 里没匹配上的条目落到 `_unmapped\`，不会丢文件。
5. CAB 未覆盖的文件（未压缩 layout / 外置文件）尝试从 MSI 同目录的源目录树复制，仍找不到才给出警告。

所有句柄用 RAII 封装，路径/字符串统一 `std::wstring`，文件 I/O 用 Win32 W 版 API，不需要 CRT 的 ANSI 路径。

---

## 限制

- **不执行安装**：不解析条件、不执行 CustomAction、不写注册表、不应用 transform 后的视图；解出来的只是 `File` 表描述的静态文件布局。
- 不支持**跨多个 CAB 连续存放**的单个文件（FDI 的 `fdintNEXT_CABINET`）；主流打包工具（WiX / InstallShield / VS Setup）不会生成这种布局。
- 外部 CAB 必须存在于 MSI 同目录（或 `Media.Cabinet` 指定的相对路径）；内嵌 CAB 无此要求。
- `extract` 需要一块与 CAB 总大小相当的临时磁盘空间，结束后自动清理（`--keep-temp` 可保留）。
- 目录还原是**相对布局**：`TARGETDIR` 作为输出根，标准目录属性（如 `[ProgramFilesFolder]`）展开为同名目录，不会映射到真实的 `C:\Program Files`。
- 不校验数字签名，也不处理加密/受口令保护的包。

---

## 实测过的样本

| 包 | 形态 | 结果 |
| --- | --- | --- |
| GitHub CLI 2.89.0 MSI | 1 个内嵌 CAB / 1 文件 | 还原 `Program Files\GitHub CLI\gh.exe`，40,592,888 字节与 `File.FileSize` 一致 |
| VC++ Runtime Minimum x64 | 外部 `cab1.cab` / 12 文件 | 12 个 DLL 全部还原到 `System64\` |
| X64 Debuggers And Tools (Windows SDK) | 18 个内嵌 CAB / 473 文件 | 473 文件、约 84 MiB，0 警告 |
| Windows Installer 缓存 MSI | 1 个内嵌 CAB / 146 文件 | 146 文件、约 21 MiB |
| VS Setup TestMsi | 无 Media 表 | 0 文件，正常退出 |
| KB2565063 `.msp` | 补丁包 | `info` / `tables` / `export` 可读取 `MsiPatchMetadata` 等表 |

---

## 源码结构

```
src/util.h / util.cpp        Unicode/UTF-8 输出、表格打印、路径与错误处理
src/msidb.h / msidb.cpp      Msi API 的 RAII 封装：查询、列信息、流、摘要、导出
src/cab.h / cab.cpp          FDI (cabinet.dll) 解压，回调式直接落盘
src/extract.h / extract.cpp  表解析、目录树、Media/Sequence 映射、CAB 导出与解包
src/main.cpp                 命令行解析与 info/tables/schema/query/list 命令
build.bat                    MSVC 构建脚本
CMakeLists.txt               CMake 构建（MSVC）
```
