# ISO/UDF 兼容性

## 支持的文件系统

- ISO9660（Primary Volume Descriptor）
- Joliet（Supplementary Volume Descriptor + Escape Sequence）
- Rock Ridge（SUSP/RRIP，ISO9660 扩展）
- UDF 2.01 / 2.50 / 2.60（NSR02 / NSR03）

## 文件系统选择策略

优先级：UDF > Joliet > ISO9660

多文件系统 ISO 不假设"第一个发现的 filesystem = 正确 filesystem"。
按完整性递减选择：UDF 提供最完整的元数据与 Unicode 文件名；
Joliet 提供 Unicode 名称；ISO9660 提供基础兼容。

Rock Ridge 不是独立的文件系统，而是附加在 ISO9660 目录记录上的
属性（SUSP 区）；仅在 ISO9660 树解析时生效（Joliet 树不带 RR）。

> 已知限制：检测到的 filesystem 集合（spec 的 available_filesystems）
> 当前未记录到数据库，只有最终选用的解析器体现在日志中。

## Rock Ridge (SUSP/RRIP)

ISO9660 目录记录尾部可带 SUSP 区（System Use Area），RRIP 在其上定义
真实文件属性。解析器按 4 字节对齐逐条读取记录，未知记录跳过。

| 记录 | 作用 | 处理 |
|------|------|------|
| SP | SUA 偏移指针 | 校正 SUSP 区起始偏移（默认 14） |
| NM | 真实文件名 | 优先于 ISO 名；续段（flags 0x01）拼接 |
| PX | POSIX 权限 | little-endian mode，写入 entry.mode |
| TF | 时间戳 | 短 7 字节 / 长 17 字节两种格式 |
| SL | 符号链接 | 组件解析：`./`、`../`、`/`、文件名 |
| CE | 续区 | 跨扇区跳转读取，深度上限 8 防循环 |
| RR / ER | 扩展标识 | 标记 RRIP 存在，不单独处理 |

### 名称优先级

解析到 NM 时以其拼接结果为准（支持长文件名与 UTF-8 名称），否则回退
到 ISO9660 原始名称。

### rr_moved 深目录还原

Rock Ridge 将路径深度超过 8 层的目录整体重定位到根目录下的
`rr_moved`（或 `.rr_moved`），原位置只留单字节占位符（0x02-0x09）：

- 解析器识别占位符并替换为 rr_moved 中的真实目录条目
- 映射双策略：占位符数字名匹配（"2".."9"），或位置映射
  （占位符 N ↔ rr_moved 目录数据第 N 条记录）
- 无法还原的占位符直接跳过，不入库（不产生垃圾条目）
- rr_moved 目录缺失或损坏时降级为普通条目，不影响其余树

### 符号链接

SL 记录解析为 EntryType::Symlink 虚拟条目，可被搜索命中；链接目标
不解析、不跟随（容器内目标路径可能不存在）。

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

> 已知限制：上述解析方式元数据（name_encoding / name_confidence）
> 目前仅在解析过程中生效，尚未持久化到数据库（entry 表无对应列，
> 见 catalog-schema.md 预留字段），无法事后追溯单个条目的解析方式。

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
