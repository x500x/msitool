# 打包 v2 设计：XML 清单 + 内置多页向导 + 快捷方式

> 状态：**已实现（M1–M6）**
> 范围：`msitool pack` 的清单格式、安装向导、快捷方式能力与 GUI 入口
> 本文是设计与实现约定的记录；实现细节以代码与 README 为准。
>
> 实现备注：
> - 清单解析使用 MSXML6，禁用 DTD / 外部实体（XXE）。
> - 内置向导以 RCDATA 嵌入 `resources/ui/`，`--dump-ui` 可导出。
> - GUI 的快捷方式编辑器采用“每行一条”的多行文本框（而非表格控件），字段与 XML 一致。
> - `_Validation` 已写入列元数据；`msiexec` / `MsiVerifyPackage` 不受影响；WiX `smoke.exe` 合并 `darice.cub` 时仍会在 `_ICESequence` 上报错（其加载方式，非包结构问题）。
> - `AdminExecuteSequence` 未包含 `CreateFolders`（该动作不允许出现在此序列）。
> - 安装范围已支持 per-user / per-machine（`--per-user` / `<pack scope>`），per-user 的子菜单 keypath 组件改用 HKCU；`ALLUSERS=1` 仅在 per-machine 下写入。
> - 真实 UI 已自动化走查：`tools/ui_walkthrough.py`（install / repair / remove，en / zh-CN，per-user）。

---

## 1. 决策记录

| 项 | 决定 | 原因 |
| --- | --- | --- |
| 清单格式 | **只用 XML**，删除 INI 解析器 | UI 是嵌套结构，INI 只能靠扁平键拼；项目尚未发版，无兼容包袱 |
| 内置向导 | WiX 风格多页向导，**中英两套**，按 `--language` 自动选择 | 覆盖主要用户群；locale 与布局分离，便于扩展 |
| 目录选择 | 直接实现 **BrowseDlg**（`DirectoryList` + `VolumeSelectCombo` + `PathEdit`） | 只让用户手输路径不可接受 |
| 美术 | 内置自制 `banner.bmp` / `background.bmp` | 不引入第三方素材与许可风险 |
| UI 表 | 全套：`Dialog` `Control` `ControlEvent` `ControlCondition` `TextStyle` `Binary` `RadioButton` `Billboard` `CheckBox` `ListBox` `ComboBox` `ListView` `Error` `InstallUISequence` `AdminUISequence` | 「做全」：内置向导与自定义 UI 使用同一套能力 |
| 快捷方式 | 桌面 / 公共桌面、开始菜单子目录、图标索引、广告式、卸载清理 | 桌面快捷方式是常见刚需 |
| 破坏性 | 一次性删除 INI 支持，文档与示例同步重写 | 避免长期维护两套解析器 |

**非目标**

- 不修改已有 MSI（工具只读）；「替换」= 打包时套用模板，不是原位改包。
- 不支持 `MsiEmbeddedUI` 外部 UI DLL、自定义动作驱动 UI。
- 不追求与 WiX `WixUIExtension` 二进制级兼容：只支持自有 XML，WiX 仅作为机制参考。
- 不做可视化拖拽排版编辑器（第一版为表格编辑 + 结构预览 + 实时 lint）。

---

## 2. 现状与差距

| 能力 | 现状 | 差距 |
| --- | --- | --- |
| 清单 | `--config pack.ini`，`[pack]` / `[feature:*]` / `[shortcut:*]` … | 需改为 XML，删除 INI |
| UI 模式 | `--ui none\|basic\|minimal`，`minimal` 为 5 个硬编码对话框 | 缺自定义、模板、多页向导 |
| UI 表 | 仅 `InstallUISequence` / `Dialog` / `Control` / `ControlEvent` / `TextStyle` | 缺 `ControlCondition` / `RadioButton` / `Binary` / `Billboard` / `Error` / `AdminUISequence` 等 |
| 快捷方式 | `dir` 精确匹配标准目录；`IconIndex` 恒 0；仅 `.ico` | 子目录、别名、图标索引、广告式、公共桌面 |
| GUI | UI 下拉 3 项；快捷方式只能写清单 | 需模式/模板/清单选择、快捷方式编辑、主程序、lint |

关键代码位置：

- `src/pack.h`：`Options` / `Stats`
- `src/pack.cpp`：`Manifest` / `loadManifest`（INI）、`kExtraSchema`、`buildMinimalUi`、`kUiSequence*`、`shortcutDirKey`、`targetFor`、快捷方式与 UI 表写入
- `src/ui_app.cpp`：打包页字段、布局、`startPack`
- `build.bat` / `CMakeLists.txt`：编译与链接

---

## 3. XML 清单规范 v1

### 3.1 总体结构

```xml
<?xml version="1.0" encoding="utf-8"?>
<msitool xmlns="https://msitool.dev/schema/pack/v1" minVersion="1">
  <pack .../>
  <features>…</features>
  <shortcuts>…</shortcuts>
  <registry>…</registry>
  <services>…</services>
  <environment>…</environment>
  <sign .../>
  <ui ...>…</ui>
</msitool>
```

### 3.2 通用规则

- **命名空间**：根元素必须带 `https://msitool.dev/schema/pack/v1`；`minVersion` 用于未来结构变更时给出明确报错。
- **嵌套即外键**：`control` 必须位于 `dialog` 内，`publish` / `controlcondition` 必须位于 `control` 内，从结构上消除悬空引用。
- **路径解析**：
  - 载荷路径（`feature.dirs`、`shortcut.target` / `icon`、`service.image`、`binary.file` 之外的源内路径）相对**源目录**；
  - 资源文件（`binary.file`、`sign.cert`、`ui.template`）相对**XML 文件所在目录**。
- **转义**：`&` 写作 `&amp;`（`&Next` 这类助记符是常见坑）；文本中的 `[ProductName]`、`{\DlgFont10}` 原样保留。
- **单位**：对话框坐标使用 MSI 对话框单位；沿用 370×270 基准。
- **未知元素 / 属性**：默认报错并带行号；不静默忽略。需要前向兼容时用 `minVersion` 显式声明。
- **安全**：解析器必须设置 `ProhibitDTD = true`、`resolveExternals = false`、`validateOnParse = false`，禁止外部实体（防 XXE）。
- **编码**：交给 MSXML 处理（UTF-8 / UTF-16，含 BOM）。

### 3.3 元素与属性

#### `<pack>`

| 属性 | 说明 | 默认 |
| --- | --- | --- |
| `name` / `version` / `manufacturer` | ProductName / ProductVersion / Manufacturer | 目录名 / `1.0.0` / `msitool` |
| `productCode` / `upgradeCode` | 固定 GUID（省略花括号自动补全） | 随机 |
| `platform` | `x64` / `Intel` | `Intel` |
| `language` | LCID；同时决定内置向导 locale | `1033` |
| `installDir` | 安装位置（相对 Program Files / 标准目录前缀 / 绝对路径） | `Program Files\<产品名>` |
| `ui` | `none` / `basic` / `minimal` / `wizard` / `custom` | `none` |
| `cabSize` | 未压缩字节上限，支持 `16M` / `1G` | 单 CAB |
| `largeFiles` | `layout` / `error` | `layout` |
| `keepEmptyDirs` | 是否生成 `CreateFolder` | `true` |

#### `<feature>`

`id`、`title`、`description`、`dirs`（`;` 分隔）、`level`、`display`、`attributes`。
生成 `Feature` + `FeatureComponents`；未匹配目录归 `MainFeature`。

#### `<shortcut>`

| 属性 | 说明 |
| --- | --- |
| `id` | Shortcut 主键（自动转合法标识符） |
| `name` | 快捷方式显示名，默认 `<id>.lnk` |
| `directory` | 标准目录名、别名（`desktop` / `startmenu` / `startup` / `sendto`）、`标准目录\子\路径`、源内目录、目录键 |
| `target` | 源内相对路径 → `[#FileKey]`；也接受 `[#FileKey]` / `[Property]`；`advertise=true` 时必须留空 |
| `arguments` / `description` / `workingDirectory` | 原样写入 |
| `icon` | `.ico` / `.exe` / `.dll`，写入 `Icon` 流 |
| `iconIndex` | `IconIndex`，默认 0 |
| `component` | 显式组件；省略时挂到目标文件所属组件 |
| `advertise` | `true` → `Target` 为 NULL，由组件 keypath 决定；要求显式 `component` |

可选子元素 `<msiShortcutProperty>`（Win7+ 任务栏/开始菜单固定属性），属性与 `MsiShortcutProperty` 列一一对应。

#### `<registryValue>` / `<service>` / `<environment>`

属性与既有 INI 键一一对应（`root`/`key`/`name`/`value`/`type`/`component` 等），元素名改为驼峰属性：
`registryValue(root,key,name,value,type,component)`、
`service(name,displayName,image,type,start,error,account,password,arguments,dependencies,description,component)`、
`environment(name,value,action,separator,system,component)`。

#### `<sign>`

`cert`、`password`、`timestamp`、`tool`。与 CLI `--sign-*` 等价，命令行优先。

#### `<ui>`

| 属性 | 说明 |
| --- | --- |
| `base` | `minimal` / `wizard`（内置基座），或省略表示从零开始 |
| `locale` | 覆盖由 `--language` 推出的 locale（如 `zh-CN`） |
| `font` | `DefaultUIFont` 属性值，默认 `DlgFont10` |
| `template` | UI 模板 MSI 路径（与 `--ui-template` 等价） |

子元素：`textstyle`、`binary`、`dialog`（含 `control`，含 `publish` / `controlcondition`）、`billboard`、`error`、`uisequence`、`adminUISequence`。

#### `<textstyle>`

`id`、`face`、`size`、`color`、`style` → `TextStyle` 表。

#### `<dialog>`

| 属性 | 说明 |
| --- | --- |
| `id` | 主键 |
| `title` | 标题（支持 `[Property]` 与 `$(Key)`） |
| `width` / `height` | 对话框单位 |
| `attributes` | 原始位掩码；另提供 `visible` / `modal` / `minimize` / `sysModal` 布尔便捷属性 |
| `first` / `default` / `cancel` | 控件 id |
| `sequence` | 写入 `InstallUISequence` 的序号；省略表示只能由 `NewDialog` / `SpawnDialog` 到达 |
| `condition` | 写入 `InstallUISequence.Condition`，如 `NOT Installed` / `Installed` |

#### `<control>`

| 属性 | 说明 |
| --- | --- |
| `id` / `type` | 控件 id 与类型 |
| `x` / `y` / `width` / `height` | 位置尺寸 |
| `attributes` | 原始位掩码 |
| `indirect` / `sunken` / `enabled` / `visible` | 便捷布尔，映射到 `Attributes` 对应位 |
| `property` | `Property` 列 |
| `text` | `Text` 列 |
| `binary` | 引用 `<binary id>`；用于 `Bitmap` / `Icon` / `ScrollableText`（RTF） |
| `next` | `Control_Next` |
| `help` | `Help` 列 |

`type` 白名单：`Text`、`PushButton`、`Line`、`Bitmap`、`Icon`、`ProgressBar`、`PathEdit`、`Edit`、`CheckBox`、`RadioButtonGroup`、`ScrollableText`、`DirectoryList`、`DirectoryCombo`、`VolumeSelectCombo`、`ListView`、`ListBox`、`ComboBox`、`Billboard`、`GroupBox`。

#### `<publish>`

`control`（父控件 id）、`event`、`value`、`condition`（默认 `1`）、`order`（省略时按出现顺序自增）。
生成 `ControlEvent`。

#### `<controlcondition>`

`control`、`action`（`enable` / `disable` / `show` / `hide`）、`condition` → `ControlCondition`。

#### `<radiobutton>`

`property`、`order`、`value`、`x`、`y`、`width`、`height`、`text`、`help` → `RadioButton`。

#### `<binary>`

`id`、`file` → `Binary` 表（流）。同一文件按路径去重，只写一条。

#### `<billboard>` / `<error>`

- `billboard`：属性与 `Billboard` 表列一一对应（`id`、`feature`、`action`、`ordering`、`control`、`x`、`y`、`width`、`height`），`binary` 引用图片。
- `error`：`id`、`text` → `Error` 表，用于覆盖安装失败等提示。

#### `<uisequence>` / `<adminUISequence>`

```xml
<uisequence>
  <action name="CostInitialize" value="800"/>
  <action name="CostFinalize" value="1000"/>
  <action name="ExecuteAction" value="1300" condition="1"/>
</uisequence>
```

未显式给出时：`dialog.sequence` 自动生成行，并补齐 `CostInitialize` / `FileCost` / `CostFinalize` / `ExecuteAction`。

### 3.4 合并与优先级

```
基座（内置 minimal / 内置 wizard / UI 模板 MSI）  →  用户 <ui> 增量  →  生成
```

- `ui="minimal"`：保留现有 5 页（兼容旧行为）。
- `ui="wizard"`：加载内置 wizard 布局 + locale。
- `ui="custom"`：无内置；可写 `base="wizard"` 复用布局。
- `--ui-template <msi>` / `<ui template>`：模板整段替换基座，用户 XML 仍可增量覆盖。
- 增量规则：
  - `dialog` 按 id 合并，只覆盖出现的属性；
  - `control` 按 `(dialog, id)` 合并；
  - `publish` 按 `(control, event, value)` 替换；
  - `controlcondition` 按 `(control, action)` 替换；
  - `textstyle` / `binary` 按 id 覆盖；
  - `uisequence` 整体替换。

这样「只改一句欢迎语」不必复制整页。

### 3.5 本地化占位符

布局文件中的文本使用 `$(Key)`：

```xml
<dialog id="WelcomeDlg" title="$(WelcomeTitle)">
  <control id="Title" type="Text" text="{\DlgFont12}$(WelcomeTitle)"/>
```

- `$$` 表示字面 `$`。
- 解析顺序：用户 `<ui locale>` / `--language` 对应的 locale 文件 → en 回退 → 报错（缺失 key 必须报错并给出行号）。
- 占位符在 XML 装载阶段替换，生成表中不留 `$(...)`。

---

## 4. 内置向导

### 4.1 页面与序列

| 页面 | 内容 | 序列 | Condition |
| --- | --- | --- | --- |
| `WelcomeDlg` | background 图 + 产品名 + 简介 + 下一步/取消 | 1290 | `NOT Installed` |
| `LicenseDlg` | `ScrollableText`(RTF) + `RadioButtonGroup`(接受/不接受) + 上一步/下一步 | 1291 | |
| `InstallDirDlg` | 说明 + `PathEdit`(`INSTALLFOLDER`) + 浏览… + 上一步/下一步 | 1292 | |
| `VerifyReadyDlg` | 安装摘要 + 安装/上一步/取消 | 1295 | |
| `ProgressDlg` | `ActionText` + `ProgressBar` + banner/billboard + 取消 | 1299 | |
| `ExitDialog` | 完成文案 + 完成 | -1 | |
| `UserExit` | 已取消 | -2 | |
| `FatalError` | 出错 | -3 | |
| `MaintenanceWelcomeDlg` | 修复 / 卸载入口 | 1250 | `Installed` |
| `MaintenanceTypeDlg` | 修复（`REINSTALL=ALL`）/ 删除（`REMOVE=ALL`） | 1255 | `Installed` |
| `BrowseDlg` | 目录浏览 | — | 由 InstallDir 唤起 |

标准动作：`CostInitialize 800`、`FileCost 900`、`CostFinalize 1000`、`ExecuteAction 1300`。

导航：Welcome → License → InstallDir → VerifyReady → Progress → Exit；上一步反向；取消 → `UserExit`；许可未接受时 `Next` 通过 `ControlCondition action="disable"` 禁用。

### 4.2 BrowseDlg

关键接线（实现时以参考包对照，见 §5）：

- `DirectoryCombo`（`VolumeSelectCombo`）、`DirectoryList`、`PathEdit` 均使用 `Property="_BrowseProperty"` + `indirect="true"`（`Control.Attributes` 的 `Indirect` 位）。
- `InstallDirDlg` 的「浏览…」：
  1. `SetProperty` 把 `_BrowseProperty` 指向 `INSTALLFOLDER`；
  2. `SpawnDialog` 打开 `BrowseDlg`。
- `BrowseDlg` 确定：`SetTargetPath [_BrowseProperty]` + `EndDialog Return`。
- 上级 / 打开 / 新建：`DirectoryListUp` / `DirectoryListOpen` / `DirectoryListNew`。
- 取消：`EndDialog Return`，不改属性。

### 4.3 许可页

- `ScrollableText` 绑定 `binary="UiLicense"`（RTF）；若实测 Binary-RTF 不可用，退回纯文本并在文档标注限制。
- `RadioButtonGroup` 的 `property` 为 `IAcceptLicense`，两个 `radiobutton` 行（`Yes` / `No`）。
- `Next` 的 `controlcondition`：`IAcceptLicense <> "Yes"` 时 `disable`。
- 「不接受」不额外弹框，保持 Next 禁用 + 取消退出，避免额外对话框。

### 4.4 美术

- `banner.bmp`：493×58，内页页头，`Bitmap` 控件缩放到 370×44。
- `background.bmp`：493×312，欢迎 / 完成 / 取消 / 错误页背景。
- 图中**不含文字**（产品名用 `Text` 控件叠加），因此与语言无关、无需按 locale 出图。
- 由 `tools/make_ui_art.ps1`（System.Drawing）生成，输出提交到仓库；不引用第三方素材。

### 4.5 维护模式

- `Installed` 条件决定进入 `MaintenanceWelcomeDlg` 还是 `WelcomeDlg`。
- 修复：`REINSTALL=ALL` + `REINSTALLMODE=vomus`。
- 卸载：`REMOVE=ALL`。
- 维护流程复用 `ProgressDlg` / `ExitDialog` / `UserExit` / `FatalError`。

### 4.6 管理安装 UI

生成 `AdminUISequence`（与 `InstallUISequence` 同构，`ExecuteAction` 对应管理安装动作），保证 `msiexec /a` 带 UI 可用。

---

## 5. 参考包与许可边界

为把 BrowseDlg / 许可页 / 维护页的事件接线做对，开发期用本机 WiX 3.14 生成一个最小参考包（`UIRef Id="WixUI_InstallDir"`），再用本工具 dump 其 `Dialog` / `Control` / `ControlEvent` / `RadioButton` / `Binary` 表，核对机制。

**只借鉴机制，不复制内容**：WiX 的文案、坐标、位图、RTF 一律自制（WiX 为 MS-RL，本项目 MIT）。参考包只存在于测试脚本，不随发布物分发。

---

## 6. 资源嵌入

- `resources/msitool.rc` 以 `RCDATA` 嵌入：
  - `UI_WIZARD_LAYOUT`、`UI_WIZARD_EN`、`UI_WIZARD_ZH`
  - `UI_LICENSE_EN`、`UI_LICENSE_ZH`
  - `UI_BANNER`、`UI_BACKGROUND`
- `Binary` / `Icon` 写入需要文件路径（`MsiRecordSetStreamW`），因此运行时把 RCDATA 释放到打包临时目录 → 写流 → 立即删除；异常路径同样清理。
- `msitool pack --dump-ui <dir>` 导出内置 XML / RTF / BMP，供用户复制修改。
- `schemas/msitool-v1.xsd` 仅开发期使用，不嵌入。

资源文件布局：

```
resources/
  msitool.rc
  ui/
    wizard.layout.xml
    wizard.en.xml
    wizard.zh-CN.xml
    license.en.rtf
    license.zh-CN.rtf
    banner.bmp
    background.bmp
tools/
  make_ui_art.ps1
```

---

## 7. 快捷方式

### 7.1 目录解析

`resolveShortcutDir(raw)` 顺序：

1. 标准目录精确名（大小写不敏感）。
2. 别名：`desktop` → `DesktopFolder`、`startmenu` → `ProgramMenuFolder`、`startup` → `StartupFolder`、`sendto` → `SendToFolder`。
3. `标准目录\子\路径`：逐级生成 `Directory` 行，父链挂到标准目录，返回叶 key（修复现有「子路径被错挂到 INSTALLFOLDER」的问题）。
4. 已有 layout 目录键 / 源内相对路径。
5. 其他：`INSTALLFOLDER` 下建目录，并输出 `[warn]`。

### 7.2 图标

- `Icon.Data` 支持 `.ico` / `.exe` / `.dll`；`IconIndex` 取 `iconIndex`。
- 同一源文件按路径去重，只写一条 `Icon` 流。

### 7.3 广告式快捷方式

`advertise="true"`：`Shortcut.Target` 写 NULL，由组件 keypath 决定目标；必须显式 `component`，否则报错。

### 7.4 目标校验

- `target` 为空且非广告式 → 报错（现有实现会静默生成不存在的 `[INSTALLFOLDER]…`）。
- `target` 以 `[` 或 `#` 开头时原样透传。

### 7.5 桌面落点

当前 `ALLUSERS=1` 写死。`DesktopFolder` / `ProgramMenuFolder` 在 `ALLUSERS=1` 下应解析到公共桌面 / 公共开始菜单，但**需实测确认**：

- 若落点正确：文档写明即可。
- 若落在用户桌面并需要公共桌面：增加 `desktopScope="user|common"`，common 走 `%PUBLIC%\Desktop` 目录链 + 属性钉位。
- 注意：`msi.dll` 不识别 `CommonDesktopFolder`（已用字符串验证），不能照搬 WiX 习惯。

### 7.6 GUI 入口

打包页新增：

- `[x] 创建桌面快捷方式`（默认勾选）
- `[x] 创建开始菜单快捷方式`（默认勾选）
- `主程序`（可留空自动探测：源根唯一 `.exe` → 否则取最大 `.exe` → 多候选时提示）
- 「快捷方式」标签页：表格编辑名称 / 目标 / 位置 / 参数 / 图标 / 说明 / 图标索引，可增删行。

GUI 直接填充 `Options.shortcuts`（内存清单），不生成临时清单文件。

---

## 8. 代码结构

### 8.1 新增

| 文件 | 职责 |
| --- | --- |
| `src/xml.h/.cpp` | MSXML6 薄封装：`Node{name, attrs, text, children, line}`，禁 DTD、禁外部实体 |
| `src/pack_xml.cpp` | XML → `Manifest` + `UiSpec`；locale 替换；结构与语义校验 |
| `src/pack_ui.h/.cpp` | `UiSpec` / `UiDialogDef` / `ControlDef` / `PublishDef` / `ConditionDef`；`applyUi(db, spec)`；lint |
| `src/pack_ui_builtin.cpp` | RCDATA 读取、locale 选择、`--dump-ui` 导出 |
| `resources/ui/*`、`resources/msitool.rc` | 内置向导与素材 |
| `schemas/msitool-v1.xsd`、`examples/pack.xml` | 文档与编辑器支持 |

### 8.2 修改

- `pack.h`：`Options` 增加 `uiTemplate`、`uiLocale`、`shortcuts`、`mainExe`、`desktopShortcut`、`startMenuShortcut`、`dumpUiDir`；`Stats` 增加 `uiPages`。
- `pack.cpp`：删除 `parseConfig` / `Section` / INI 版 `loadManifest`；`buildMinimalUi` 系列迁到 `pack_ui.cpp`；`kExtraSchema` 扩展至全部 UI 表。
- 快捷方式：`shortcutDirKey` → `resolveShortcutDir`；`IconIndex`；`advertise`；目标校验。
- `ui_app.cpp`：UI 模式下拉 5 项、`.xml` 文件选择、UI 模板选择、快捷方式标签页、主程序、lint 展示；拖放清单名改为 `pack.xml` / `msitool.xml`。
- `build.bat`：新增源文件，增加 `rc.exe` 编译与 `.res` 链接。
- `CMakeLists.txt`：`project(... LANGUAGES CXX RC)`，加入新源文件与资源。
- `README.md` / `CHANGELOG.md` / `examples/`：重写打包章节，删除 INI 示例。

### 8.3 关键结构

```cpp
struct UiSpec {
    std::wstring font;                              // DefaultUIFont
    std::vector<TextStyleDef> styles;
    std::vector<UiDialogDef>  dialogs;              // 内含 controls
    std::vector<UiSequenceEntry> sequence;          // InstallUISequence
    std::vector<UiSequenceEntry> adminSequence;     // AdminUISequence
    std::map<std::wstring, std::wstring> binaries;  // id -> 文件路径
    std::vector<UiIssue> issues;                    // lint 结果
};
```

---

## 9. 校验规则（生成前失败）

1. `dialog.first/default/cancel`、`control.next` 引用的控件存在。
2. 每个对话框的 `Control_Next` 构成**单环**（否则 `msiexec` 报 2810）。
3. `publish` / `controlcondition` 的父控件存在；事件白名单（`EndDialog`、`NewDialog`、`SpawnDialog`、`SetProperty`、`SetTargetPath`、`ActionText`、`SetProgress`、`DirectoryListUp/Open/New`，`DoAction` 仅标准动作）。
4. `NewDialog` / `SpawnDialog` 的 `value` 指向已定义 dialog。
5. 序列包含 `CostInitialize`、`CostFinalize`、`ExecuteAction`；`-1/-2/-3` 终止页存在；`Installed` / `NOT Installed` 成对出现。
6. `RadioButtonGroup` 必须至少有一个 `radiobutton`；`PathEdit` / `DirectoryList` / `VolumeSelectCombo` 必须有 `property`，`indirect` 生效；`ScrollableText` / `Bitmap` 的 `binary` 存在。
7. `binary` 引用的文件存在；位图 / RTF 大小给出提示。
8. 主键唯一；数值范围（宽高 > 0、attributes 合法）；文本长度软上限告警。
9. 与基座重名时按 §3.4 合并并记录日志。
10. XML 解析错误、未知元素/属性、缺失 locale key 均报行号。

---

## 10. 兼容性破坏与迁移

- 删除 INI 解析；`--config` 保留名字，只接受 XML（内容嗅探 `<`）。
- 传入 `.ini` 时明确报错：「INI 清单已移除，请改用 XML（见 examples/pack.xml）」。
- 删除 `examples/pack.ini`，新增 `examples/pack.xml`。
- README 打包章节全部改为 XML，删除 `[pack]` / `[feature:*]` 等 INI 语法。
- GUI 拖放识别名改为 `pack.xml` / `msitool.xml`；文件选择过滤器改为 `*.xml`。
- `--ui none|basic|minimal` 行为不变；新增 `wizard` / `custom`。

---

## 11. 里程碑

| 里程碑 | 内容 | 验收标准 | 估计 |
| --- | --- | --- | --- |
| M1 | XML 解析 + 全部非 UI 段落 + 删除 INI | `examples/pack.xml` 能生成与旧 INI 等价的包（表数一致） | 1.5 天 |
| M2 | UI 表生成（含 BrowseDlg / 条件 / radio / error / admin）+ 校验器 | 手写一份多页 XML 能安装成功 | 2.5 天 |
| M3 | 内置向导布局 + 中英 locale + EULA + banner/background + RC 嵌入 | `--ui wizard` 中文 / 英文安装包各走通一遍完整流程 | 3 天 |
| M4 | 维护模式（修复 / 卸载）+ `/a` UI + Billboard / Error | 已安装 → 修复 / 卸载均成功 | 1 天 |
| M5 | 快捷方式补齐 + GUI（模式 / XML / 快捷方式页 / 主程序 / lint） | GUI 勾选桌面快捷方式后安装可见，卸载消失 | 2.5 天 |
| M6 | 文档、XSD、示例、ICE 与安装回归脚本 | README 与实现一致；`dark` + `light` 无 ICE 阻断 | 1 天 |

合计 **约 11–12 个工作日**。M3 为风险集中点。

---

## 12. 测试矩阵

| 类别 | 用例 |
| --- | --- |
| XML | 非法元素/属性报行号；XXE 被拒；UTF-8 / UTF-16；`&amp;`；相对路径；`minVersion` |
| 向导 | 6 页完整走查；上一步 / 取消；许可未接受禁用；BrowseDlg 选目录后 `INSTALLFOLDER` 生效；`/qn`、`/qb`、`/a` |
| 维护 | 已安装状态下启动 → 修复；卸载；中英各一遍 |
| 快捷方式 | 桌面 / 开始菜单 / 子菜单 / 启动；`iconIndex`；广告式；公共桌面落点；卸载清理 |
| 本地化 | 1033 / 2052；缺 key 报错；未知语言回退告警 |
| 回归 | `MsiVerifyPackage`；WiX `dark.exe` 反编译 + `light` ICE；x86 构建 |
| 视觉 | 100% / 125% / 150% / 200% DPI；中文字体；banner 缩放 |
| 旧行为 | `--ui minimal` 输出保持可安装；现有表结构与统计一致 |

---

## 13. 风险

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| `ScrollableText` + Binary RTF 写法不确定 | 许可页无法显示 RTF | 先做最小验证；失败退回纯文本并保留接口 |
| BrowseDlg 的 `Indirect` + `_BrowseProperty` 接线复杂 | 目录浏览不可用 | 参考包对照 + 真机验证；必要时降级为仅 `PathEdit` |
| MSI 无深色模式 | 与 GUI 主题不一致 | 素材按浅色设计，文档说明 |
| 中文字体在 125% 下行高/截断 | 中文向导排版差 | 单独设置 zh-CN 字体并在多 DPI 下截图验收 |
| `ALLUSERS=1` 桌面落点未知 | 快捷方式位置错误 | 实测；必要时增加 `desktopScope` |
| RC 嵌入引入 `rc.exe` | 构建链变化 | Windows SDK 自带；CMake 启用 `RC` 语言 |
| 删除 INI 造成旧脚本失效 | 用户报错 | 明确错误信息 + README 迁移说明 |

---

## 14. 待确认（实现中如遇阻塞再定）

1. 内置 EULA 使用占位文本（明确标注「示例」），由用户替换为正式协议。
2. banner / background 采用中性渐变 + 无文字；如需品牌化，用户可替换 `<binary>`。
3. 公共桌面落点依据实测结果决定是否引入 `desktopScope`。

---

## 15. 实现顺序建议

1. M1 完成后先提交一版 XML 清单，确认与旧包行为等价。
2. M2 用一份手写 XML 打通表格生成，再接内置向导。
3. M3 完成后提供可安装的中英文示例包，做实机验收。
4. M4 / M5 并行推进，最后统一 M6 文档与回归。
