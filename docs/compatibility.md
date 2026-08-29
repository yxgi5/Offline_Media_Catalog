# ISO/UDF 兼容性

## 支持的文件系统

- ISO9660（Primary Volume Descriptor）
- Joliet（Supplementary Volume Descriptor + Escape Sequence）
- UDF 2.01 / 2.50 / 2.60（NSR02 / NSR03）

## 文件系统选择策略

优先级：UDF > Joliet > ISO9660

多文件系统 ISO 不假设"第一个发现的 filesystem = 正确 filesystem"。
按完整性递减选择：UDF 提供最完整的元数据与 Unicode 文件名；
Joliet 提供 Unicode 名称；ISO9660 提供基础兼容。

## UDF Unicode（OSTA Compressed Unicode）

文件名按 UDF/OSTA Compressed Unicode 规则解析：

| Compression ID | 含义 | 处理 |
|----------------|------|------|
| 8 | CS0 8-bit | 按 Latin-1 扩展处理 |
| 16 | CS0 16-bit | UTF-16BE → UTF-8 |
| 0-7 | 老式 8-bit | 单字节兼容 |
| 其他 | 非标准 | heuristic recovery |

必须处理：surrogate pairs、invalid sequences、截断数据。

禁止 `reinterpret bytes as UTF-8`。

## 三级容错策略

```
strict parser
    ↓ failure
compatibility parser
    ↓
heuristic recovery
```

解析结果记录：

- `name_encoding = udf-cs0`，`name_confidence = 100`（严格解析）
- `name_encoding = compatibility`，`name_confidence = 80`（容错）
- `name_encoding = heuristic`，`name_confidence = 40`（启发式恢复）

## 非标准 UDF

现实世界中存在"Windows 可以正确挂载，但 Catalog 软件解析后出现乱码"的
ISO。常见问题与处理：

| 问题 | 处理 |
|------|------|
| 文件名未压缩（裸 UTF-16BE） | 检测并兼容解码 |
| Compression ID 错误 | 尝试多个 CID 解码 |
| 无效 surrogate | 跳过非法单元 |
| 长度字段超界 | 截断到可用数据 |
| 分区映射缺失 | 回退到分区 0 |
| Metadata Partition (UDF 2.50) | 预留 metadata file 支持 |

## 禁止事项

- 不依赖 Windows Mount-DiskImage / Linux mount / macOS hdiutil
- 不进行文件解包
- 不修改系统挂载状态
- 不要求管理员权限

## Regression Test 要求

测试集必须覆盖：

- ISO9660 / Joliet / UDF 纯格式
- 中文 / 日文 / 韩文 / emoji 文件名
- 长文件名
- 混合 ISO/UDF
- 非标准 UDF
- Windows 兼容但其他软件乱码的 UDF
