---
name: Bug 报告
about: 报告 msitool 的错误行为或崩溃
title: "[BUG] "
labels: bug
assignees: ''
---

## 环境

- msitool 架构与构建方式：`build.bat` x64 / x86 / CMake / 其它
- MSVC 与 Windows SDK 版本：
- Windows 版本：
- 命令来源：cmd.exe / PowerShell / 其它终端

## 完整命令行

```
（粘贴命令，敏感路径可脱敏）
```

## 完整输出（stdout + stderr，UTF-8）

```
（粘贴输出，包括 [warn] / error 行）
```

## 包诊断（如果是某个 MSI 触发）

```
msitool verify <包> 的输出
msitool list <包> media --tsv 的相关行
msitool list <包> dirs --tsv / files --tsv 的相关行
```

> 请不要上传私有或受许可的 MSI / CAB 文件，用脱敏后的表数据描述即可。

## 期望行为 / 实际行为

- 期望：
- 实际：

## 补充

（是否可稳定复现、是否只出现在特定包、是否已经尝试 `--dry-run` / `--verbose` 等）
