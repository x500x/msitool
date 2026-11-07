# msitool

一个用 **MSVC + C++ + Windows Installer (Msi) API** 编写的命令行工具，用于**查看**、**解包**和**打包** MSI 安装包。纯 Win32 / Unicode，运行时只依赖系统自带的 `msi.dll` 和 `cabinet.dll`，不需要 WiX、Orca、7-Zip，也不需要管理员权限或真正安装任何东西。

| 命令 | 作用 |
| --- | --- |
| `verify` | 验证文件是否是真正的 MSI/MSP 包（签名、打开、`_Tables`、摘要、CAB） |
| `info` | 摘要信息（SummaryInformation）、关键属性、Media/CAB 清单 |
| `tables` | 列出所有表、是否持久表、行数 |
| `schema <表>` | 列定义、MSI 类型、主键 |
| `query <SQL>` | 执行任意 MSI SQL 查询（只读） |
| `list <名称>` | 查看常用表或任意表（files / components / features / dirs / media / properties / streams / cabs / …） |
| `cabs <目录>` | 导出内嵌/外部 CAB + `manifest.txt` |
| `export <目录>` | 导出全部表为 `.idt`、全部原始流、Binary 表、`summary.txt` |
| `extract <目录>` | **解包**：还原安装目录树，把 CAB 里的文件直接解压到目标路径 |
| `pack <源目录> <输出.msi>` | **打包**：把一个目录做成自包含 MSI（内嵌 MSZIP CAB） |

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
   src\util.cpp src\msidb.cpp src\cab.cpp src\extract.cpp src\pack.cpp src\main.cpp ^
   Msi.Lib Cabinet.Lib Shell32.Lib Ole32.Lib /link /MACHINE:X64
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
msitool verify setup.msi
msitool info setup.msi
msitool tables setup.msi
msitool schema setup.msi File
msitool query setup.msi "SELECT `File`,`FileName`,`FileSize` FROM `File`"
msitool list setup.msi files --tsv > files.tsv
msitool list setup.msi streams
msitool cabs setup.msi C:\out\cabs
msitool export setup.msi C:\out\archive
msitool extract setup.msi C:\out\files --verbose
msitool pack C:\payload C:\out\app.msi --name "My App" --version 1.2.3
```

`extract` 的常用选项：

| 选项 | 说明 |
| --- | --- |
| `--flat` | 不还原目录层次，所有文件平铺到输出目录（重名自动加序号） |
| `--dry-run` | 只列出计划（CAB 内容会被枚举但不写文件） |
| `--skip-existing` | 跳过已存在的目标文件 |
| `--source-layout` | 按源镜像（layout）的目录树输出，而不是安装后的目录树 |
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

## 包验证

**所有命令**在打开数据库之前都会自动预检；传错文件（普通文本、EXE/ZIP、`.mst` transform、目录、不存在路径）会立即被拒绝，不会进入任何表查询或解包逻辑，并给出明确原因、退出码 1。`verify` 把每一步结果打印成完整报告并返回退出码（VALID=0 / INVALID=1）：

```bat
msitool verify setup.msi
```

| 检查项 | 判定 |
| --- | --- |
| 存在 / 非目录 / 大小 ≥ 512 字节 | 否则直接失败 |
| OLE 复合文件头 `D0 CF 11 E0 A1 B1 1A E1` | MSI/MSP 都是 CFB 存储；不符即拒绝（会显示实际前 8 字节） |
| `MsiOpenDatabaseW`（`.msp` 用 patch 模式） | 打不开则不是有效 Installer 数据库 |
| `MsiVerifyPackageW`（仅 `.msi`） | Windows Installer 自己判定该包是否可安装 |
| `_Tables` 虚拟表可查询 | 排除「有 CFB 头但不是 MSI 存储」的文件 |
| `SummaryInformation` / `_Streams` / `Media` | 附加诊断信息，打印种类、表数、流数、CAB 数 |

报告中的 `Package kind` 会标明是 `.msi` 还是 `.msp`；`.msp` 可以查看但只适合用 `info`/`tables`/`query`/`export`/`cabs`，`extract` 面向 `.msi`。

---

## 打包（pack）

```bat
msitool pack <源目录> <输出.msi> [选项]
```

递归收集目录里的所有文件，生成一个**自包含** MSI：文件压缩进一个 MSZIP CAB，CAB 作为 `_Streams` 流内嵌，不依赖外部文件。

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `--name <文本>` | 源目录名 | ProductName，同时作为安装目录下的文件夹名 |
| `--version <x.y.z>` | `1.0.0` | ProductVersion |
| `--manufacturer <文本>` | `msitool` | Manufacturer |
| `--product-code <GUID>` | 随机 GUID | 固定 ProductCode（可省略花括号，会自动补全） |
| `--upgrade-code <GUID>` | 随机 GUID | 固定 UpgradeCode |
| `--platform <x64\|Intel>` | `Intel` | `x64` 使用 `ProgramFiles64Folder` + 64 位组件；`Intel` 使用 `ProgramFilesFolder` |
| `--language <LCID>` | `1033` | ProductLanguage 与 SummaryInformation Template |
| `--force` | - | 覆盖已存在的输出 MSI |
| `--dry-run` | - | 只打印计划，不写 CAB / 数据库 |
| `-v` | - | 逐文件详细日志 |

生成的数据库包含 `Directory`、`Component`、`File`、`Media`、`Feature`、`FeatureComponents`、`Property`、`InstallExecuteSequence`、`AdminExecuteSequence` 九张表，并写入 SummaryInformation（Title / Template / PackageCode / PageCount=200 / WordCount=2 等）。`pack` 结束会调用 `MsiVerifyPackageW` 自检，`verify` 也会显示这一项。

示例输出：

```
Packing 7 file(s) in 4 directory(ies) -> E:\work\winmsi\app.msi
Product   : Test App 1.2.3
Codes     : ProductCode={...} UpgradeCode={...}
Platform  : Intel;1033
Cabinet   : 1114953 bytes (MSZIP, embedded as #cab1.cab)
MSI       : 1138688 bytes
MsiVerifyPackage: OK (Windows Installer accepts this package)
Done      : E:\work\winmsi\app.msi
```

不写注册表也能验证安装行为：因为带有 `AdminExecuteSequence`，可以用 `msiexec /a app.msi /qn TARGETDIR=C:\out\admin`（或 `ACTION=ADMIN`）让 Windows Installer 自己把 CAB 解到目录里。

---

## 实现原理

**查看侧（全部走 Msi API，只读打开）**

- **预检**：`probe()` 读文件属性与前 8 字节校验 CFB 签名；`Db` 构造函数在 `MsiOpenDatabaseW` 之后再次确认 `_Tables` 虚拟表可查询。`verify` 复用这层检查生成报告。
- `MsiOpenDatabaseW(path, MSIDBOPEN_READONLY, &h)`；对 `.msp` 使用 `MSIDBOPEN_PATCHFILE` 伪指针（msi.dll 实际要求值 32，SDK 宏在 Unicode 下算出 16，代码两种都试）。
- `MsiDatabaseOpenViewW` + `MsiViewExecute` + `MsiViewFetch` 逐行取数据，`MsiViewGetColumnInfo(MSICOLINFO_NAMES/TYPES)` 拿列名与类型，`MsiRecordGetStringW` / `MsiRecordGetInteger` / `MsiRecordDataSize` 渲染单元格。
- `MsiGetSummaryInformationW` + `MsiSummaryInfoGetPropertyW` 读取 PID_TITLE / PID_TEMPLATE / PID_REVNUMBER(PackageCode) 等。
- `MsiDatabaseExportW` 导出 `.idt`；`MsiRecordReadStream` 读取 `_Streams` / `Binary` 的二进制流。
- MSI SQL 是 SQL 的**子集**：没有 `COUNT(*)`，`tables` 的行数通过 `MsiViewFetch` 计数得到。

**解包侧**

1. 读 `Media` 表：`Cabinet` 以 `#` 开头表示流名（内嵌 CAB，从 `_Streams` 读出）；否则是外部 CAB 文件名，按 MSI 所在目录查找；为空则是未压缩 layout。
2. 按 `Media.LastSequence` 升序划分 `File.Sequence` 区间，确定每个文件属于哪个 CAB。
3. 读 `Directory` / `Component.Directory_` / `File.FileName` 还原相对目录树，`TARGETDIR` 映射到输出根目录。`DefaultDir` 使用 MSI 的 `target:source` 双路径语法：
   - 冒号左边是**安装目录**（target），右边是**未压缩源镜像里的目录**（source）；没有冒号时两者相同；
   - 每一侧都可以是 `short|long`（取长名），`.` 表示与父目录相同，`SourceDir` 是根标记，`[Property]` 会展开。
   - 例如 `.:x64` 表示「安装到父目录，但源文件在 `x64\` 子目录」；`.:LANGS-~1|Langs-x64` 表示「安装到父目录，源文件在 `Langs-x64\`」。
   - 输出默认用 target 侧；查找未压缩 layout 的源文件用 source 侧；`--source-layout` 可改用 source 侧作为输出布局。
4. 把 CAB 暂存到一个短的临时目录（规避 FDI 的 256 字节路径限制，同时支持中文路径），再调用 `cabinet.dll` 的 **FDI**（`FDICreate` / `FDICopy`）。在 `fdintCOPY_FILE` 回调里按 CAB 内文件名查表，直接用 `CreateFileW` 打开最终目标路径，FDI 把数据写进去；`fdintCLOSE_FILE_INFO` 关闭句柄并恢复文件时间。CAB 里没匹配上的条目落到 `_unmapped\`，不会丢文件。
5. CAB 未覆盖的文件（未压缩 layout / 外置文件）尝试从 MSI 同目录的源目录树复制，仍找不到才给出警告。

所有句柄用 RAII 封装，路径/字符串统一 `std::wstring`，文件 I/O 用 Win32 W 版 API，不需要 CRT 的 ANSI 路径。

**打包侧（pack）**

1. 递归枚举源目录：每个目录生成一行 `Directory` 与一个 `Component`，每个文件生成一行 `File`；`File` 主键（`F1`、`F2`…）同时作为 CAB 内文件名，`Sequence` 从 1 递增。
2. `cabinet.dll` 的 **FCI**（`FCICreate` / `FCIAddFile` / `FCIFlushCabinet` / `FCIDestroy`）生成单个 MSZIP CAB。源路径以 UTF-8 交给 FCI，回调里再转回 UTF-16 打开，因此中文/空格路径可用。
3. `MsiOpenDatabaseW(MSIDBOPEN_CREATE)` 新建数据库，用 `CREATE TABLE` 建表（MSI SQL 的主键语法很特殊：`PRIMARY KEY <列名>` 必须附在最后一个列定义之后），再用参数化 `INSERT ... VALUES (?, ?)` 写入各行。
4. `MsiRecordSetStreamW` + `MsiViewModify(MSIMODIFY_INSERT)` 把 CAB 写进 `_Streams` 表，`Media.Cabinet` 设为 `#cab1.cab`。
5. `MsiSummaryInfoSetPropertyW` 写摘要属性，`MsiSummaryInfoPersist` 持久化，`MsiDatabaseCommit` 提交；最后用 `MsiVerifyPackageW` 自检。
6. 写入标准 `InstallExecuteSequence` 与 `AdminExecuteSequence`，因此 `msiexec /i` 和 `msiexec /a` 都能处理生成的包。

---

## 限制

- **不执行安装**：不解析条件、不执行 CustomAction、不写注册表、不应用 transform 后的视图；解出来的只是 `File` 表描述的静态文件布局。
- 不支持**跨多个 CAB 连续存放**的单个文件（FDI 的 `fdintNEXT_CABINET`）；主流打包工具（WiX / InstallShield / VS Setup）不会生成这种布局。
- 外部 CAB 必须存在于 MSI 同目录（或 `Media.Cabinet` 指定的相对路径）；内嵌 CAB 无此要求。
- `extract` 需要一块与 CAB 总大小相当的临时磁盘空间，结束后自动清理（`--keep-temp` 可保留）。
- 目录还原是**相对布局**：`TARGETDIR` 作为输出根，标准目录属性（如 `[ProgramFilesFolder]`）展开为同名目录，不会映射到真实的 `C:\Program Files`。
- 未压缩 administrative image 的**源目录结构**（`DefaultDir` 的 source 侧，例如 `x64\`、`Langs-x64\`、`Win7_x86\`）通常与安装后的结构不同；`extract` 默认按安装布局输出，需要保留源镜像结构时用 `--source-layout`。
- 不校验数字签名，也不处理加密/受口令保护的包。

**打包（pack）的限制**

- 生成的是**最小可安装包**：单 Feature、单 CAB、无 UI、无快捷方式 / 注册表 / 服务 / 自定义动作、无数字签名；空目录不会被保留。
- 单个文件不得超过 2 GiB（`File.FileSize` 是 32 位整数）；CAB 也不分卷，总量需小于 2 GiB。
- 文件名 / 目录名中的 `\ / : * ? " < > |` 与控制字符会被替换为 `_`，结尾的点与空格会被去掉，MSI 里的名字可能与原名略有差异。
- 需要一块与 CAB 相当的临时磁盘空间（FCI 临时文件 + CAB），结束后自动清理。
- 输出路径已存在时默认拒绝，需 `--force` 覆盖。

---

## 实测过的样本

| 包 | 形态 | 结果 |
| --- | --- | --- |
| GitHub CLI 2.89.0 MSI | 1 个内嵌 CAB / 1 文件 | 还原 `Program Files\GitHub CLI\gh.exe`，40,592,888 字节与 `File.FileSize` 一致 |
| VC++ Runtime Minimum x64 | 外部 `cab1.cab` / 12 文件 | 12 个 DLL 全部还原到 `System64\` |
| X64 Debuggers And Tools (Windows SDK) | 18 个内嵌 CAB / 473 文件 | 473 文件、约 84 MiB，0 警告 |
| Windows Installer 缓存 MSI | 1 个内嵌 CAB / 146 文件 | 146 文件、约 21 MiB |
| VS Setup TestMsi | 无 Media 表 | 0 文件，正常退出 |
| NetLimiter 5.3.28 x64 | 未压缩 administrative layout，`target:source` 目录语法 / 178 文件 | 178 文件全部从源目录树还原，0 警告 |
| KB2565063 `.msp` | 补丁包 | `verify` 判定 VALID（patch 模式），`info` / `tables` / `export` 可读取 `MsiPatchMetadata` 等表 |
| 文本 / EXE / 伪造 CFB 头 / 目录 / 不存在路径 | 非 MSI | 预检拒绝；`verify` 输出 INVALID + 完整原因，退出码 1，其它命令同样报错退出 |
| `pack` 往返（7 文件：中文名 / 空格 / 空文件 / 嵌套目录 / 1 MiB 随机数据） | 自包含 MSI | `MsiVerifyPackage` OK；`extract` 往返 sha256 完全一致；`ACTION=ADMIN` 让 Windows Installer 自己也解出全部 7 文件 |
| `pack` 中文源目录与中文输出路径、`x64` / `Intel`、固定 GUID、`--force` / `--dry-run` / x86 版 | 选项覆盖 | 全部通过；CMake 构建同样包含 pack |

---

## 源码结构

```
src/util.h / util.cpp        Unicode/UTF-8 输出、表格打印、路径与错误处理
src/msidb.h / msidb.cpp      Msi API 的 RAII 封装：包预检、查询、列信息、流、摘要、导出
src/cab.h / cab.cpp          FDI (cabinet.dll) 解压，回调式直接落盘
src/extract.h / extract.cpp  表解析、目录树、Media/Sequence 映射、CAB 导出与解包
src/pack.h / pack.cpp        FCI 压缩 + Msi 建库/建表/写流/摘要，把目录打包成 MSI
src/main.cpp                 命令行解析与全部命令（含 pack / verify）
build.bat                    MSVC 构建脚本
CMakeLists.txt               CMake 构建（MSVC）
```
