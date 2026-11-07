## 改动内容

<!-- 一句话说明做了什么、为什么 -->

## 影响范围

- [ ] `verify` / `info` / `tables` / `schema` / `query` / `list`
- [ ] `export` / `cabs`
- [ ] `extract`
- [ ] `pack`
- [ ] 构建（`build.bat` / `CMakeLists.txt`）
- [ ] 文档 / CI

## 验证

<!-- 跑过的命令与结果：真实 MSI 样本、往返 sha256、MsiVerifyPackage、x64/x86 构建 -->

```
（粘贴关键输出）
```

- [ ] `build.bat` x64 编译通过且 `/W4` 零警告
- [ ] `build.bat x86` 编译通过且 `/W4` 零警告
- [ ] 相关命令已回归（内嵌 CAB / 外部 CAB / 多 CAB / 未压缩 layout / `.msp` / 非 MSI 文件）
- [ ] 打包改动已确认 `MsiVerifyPackageW` 仍返回 `ERROR_SUCCESS`

## 文档

- [ ] 更新了 `README.md`（新命令 / 选项 / 限制 / FAQ）
- [ ] 更新了 `CHANGELOG.md`
- [ ] 更新了 `src/main.cpp` 的 `kUsage` 与命令/选项解析

## 关联 issue

Closes #
