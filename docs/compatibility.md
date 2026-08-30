# ISO/UDF 兼容性 / ISO/UDF Compatibility

## 支持的文件系统 / Supported Filesystems

- ISO9660（Primary Volume Descriptor）
  ISO9660 (primary volume descriptor).
- Joliet（Supplementary Volume Descriptor + Escape Sequence）
  Joliet (supplementary volume descriptor + escape sequence).
- Rock Ridge（SUSP/RRIP，ISO9660 扩展）
  Rock Ridge (SUSP/RRIP, an ISO9660 extension).
- UDF 2.01 / 2.50 / 2.60（NSR02 / NSR03）
  UDF 2.01 / 2.50 / 2.60 (NSR02 / NSR03).

## 文件系统选择策略 / Filesystem Selection Strategy

优先级：UDF > Joliet > ISO9660

Priority: UDF > Joliet > ISO9660.

多文件系统 ISO 不假设"第一个发现的 filesystem = 正确 filesystem"。
按完整性递减选择：UDF 提供最完整的元数据与 Unicode 文件名；
Joliet 提供 Unicode 名称；ISO9660 提供基础兼容。

For multi-filesystem ISOs we do not assume "the first filesystem found is the
correct one". Selection goes by decreasing completeness: UDF offers the most
complete metadata and Unicode file names, Joliet offers Unicode names, and
ISO9660 offers basic compatibility.

Rock Ridge 不是独立的文件系统，而是附加在 ISO9660 目录记录上的
属性（SUSP 区）；仅在 ISO9660 树解析时生效（Joliet 树不带 RR）。

Rock Ridge is not a separate filesystem but attributes attached to ISO9660
directory records (the SUSP area); it only applies when parsing the ISO9660
tree (Joliet trees carry no RR).

> 已知限制：检测到的 filesystem 集合（spec 的 available_filesystems）
> 当前未记录到数据库，只有最终选用的解析器体现在日志中。
>
> Known limitation: the set of detected filesystems (the spec's
> available_filesystems) is not recorded in the database yet; only the
> eventually chosen parser shows up in the logs.

## Rock Ridge (SUSP/RRIP)

ISO9660 目录记录尾部可带 SUSP 区（System Use Area），RRIP 在其上定义
真实文件属性。解析器按 4 字节对齐逐条读取记录，未知记录跳过。

ISO9660 directory records may carry a SUSP area (System Use Area) at their
tail, on top of which RRIP defines the real file attributes. The parser reads
records one by one, 4-byte aligned, skipping unknown records.

| 记录 / Record | 作用 / Purpose | 处理 / Handling |
|------|------|------|
| SP | SUA 偏移指针 / SUA offset pointer | 校正 SUSP 区起始偏移（默认 14）/ corrects the SUSP start offset (default 14) |
| NM | 真实文件名 / real file name | 优先于 ISO 名；续段（flags 0x01）拼接 / takes precedence over the ISO name; continuation segments (flags 0x01) are concatenated |
| PX | POSIX 权限 / POSIX permissions | little-endian mode，写入 entry.mode / little-endian mode, written to entry.mode |
| TF | 时间戳 / timestamps | 短 7 字节 / 长 17 字节两种格式 / short 7-byte and long 17-byte formats |
| SL | 符号链接 / symlink | 组件解析：`./`、`../`、`/`、文件名 / component parsing: `./`, `../`, `/`, file names |
| CE | 续区 / continuation area | 跨扇区跳转读取，深度上限 8 防循环 / cross-sector reads with a depth cap of 8 against loops |
| RR / ER | 扩展标识 / extension markers | 标记 RRIP 存在，不单独处理 / mark that RRIP is present; no individual handling |

### 名称优先级 / Name Priority

解析到 NM 时以其拼接结果为准（支持长文件名与 UTF-8 名称），否则回退
到 ISO9660 原始名称。

When an NM record is parsed, its concatenated result wins (long file names and
UTF-8 names are supported); otherwise we fall back to the raw ISO9660 name.

### rr_moved 深目录还原 / rr_moved Deep-Directory Restoration

Rock Ridge 将路径深度超过 8 层的目录整体重定位到根目录下的
`rr_moved`（或 `.rr_moved`），原位置只留单字节占位符（0x02-0x09）：

Rock Ridge relocates directories deeper than 8 levels into `rr_moved`
(or `.rr_moved`) at the root, leaving only single-byte placeholders
(0x02-0x09) in the original positions:

- 解析器识别占位符并替换为 rr_moved 中的真实目录条目
  The parser recognizes placeholders and replaces them with the real
  directory entries from rr_moved.
- 映射双策略：占位符数字名匹配（"2".."9"），或位置映射
  （占位符 N ↔ rr_moved 目录数据第 N 条记录）
  Two mapping strategies: numeric-name matching ("2".."9"), or positional
  mapping (placeholder N ↔ the N-th record of rr_moved's directory data).
- 无法还原的占位符直接跳过，不入库（不产生垃圾条目）
  Unresolvable placeholders are skipped and never stored (no junk entries).
- rr_moved 目录缺失或损坏时降级为普通条目，不影响其余树
  If rr_moved is missing or corrupt, entries degrade to plain ones without
  affecting the rest of the tree.

### 符号链接 / Symlinks

SL 记录解析为 EntryType::Symlink 虚拟条目，可被搜索命中；链接目标
不解析、不跟随（容器内目标路径可能不存在）。

SL records become EntryType::Symlink virtual entries that can be found by
search; link targets are not resolved or followed (the target path may not
exist inside the container).

## UDF Unicode（OSTA Compressed Unicode）/ UDF Unicode (OSTA Compressed Unicode)

文件名按 UDF/OSTA Compressed Unicode 规则解析：

File names are parsed per the UDF/OSTA Compressed Unicode rules:

| Compression ID | 含义 / Meaning | 处理 / Handling |
|----------------|------|------|
| 8 | CS0 8-bit | 按 Latin-1 扩展处理 / treated as Latin-1 extension |
| 16 | CS0 16-bit | UTF-16BE → UTF-8 |
| 0-7 | 老式 8-bit / legacy 8-bit | 单字节兼容 / single-byte compatible |
| 其他 / other | 非标准 / non-standard | heuristic recovery |

必须处理：surrogate pairs、invalid sequences、截断数据。

Must handle: surrogate pairs, invalid sequences, and truncated data.

禁止 `reinterpret bytes as UTF-8`。

Never `reinterpret bytes as UTF-8`.

## 三级容错策略 / Three-Level Fallback Strategy

```
strict parser
    ↓ failure
compatibility parser
    ↓
heuristic recovery
```

解析结果记录 / Parsing results are recorded as:

- `name_encoding = udf-cs0`，`name_confidence = 100`（严格解析）/ (strict parsing)
- `name_encoding = compatibility`，`name_confidence = 80`（容错）/ (compatibility)
- `name_encoding = heuristic`，`name_confidence = 40`（启发式恢复）/ (heuristic recovery)

> 已知限制：上述解析方式元数据（name_encoding / name_confidence）
> 目前仅在解析过程中生效，尚未持久化到数据库（entry 表无对应列，
> 见 catalog-schema.md 预留字段），无法事后追溯单个条目的解析方式。
>
> Known limitation: this parsing-mode metadata (name_encoding /
> name_confidence) currently only applies during parsing; it is not persisted
> to the database (the entry table has no such columns; see the reserved
> fields in catalog-schema.md), so the parsing mode of a single entry cannot
> be traced afterwards.

## 非标准 UDF / Non-Standard UDF

现实世界中存在"Windows 可以正确挂载，但 Catalog 软件解析后出现乱码"的
ISO。常见问题与处理：

In the wild there are ISOs that "Windows mounts correctly, but catalog
software shows mojibake". Common problems and their handling:

| 问题 / Problem | 处理 / Handling |
|------|------|
| 文件名未压缩（裸 UTF-16BE）/ uncompressed file names (raw UTF-16BE) | 检测并兼容解码 / detect and decode compatibly |
| Compression ID 错误 / wrong compression ID | 尝试多个 CID 解码 / try multiple CIDs |
| 无效 surrogate / invalid surrogates | 跳过非法单元 / skip invalid units |
| 长度字段超界 / length fields out of bounds | 截断到可用数据 / truncate to available data |
| 分区映射缺失 / missing partition mapping | 回退到分区 0 / fall back to partition 0 |
| Metadata Partition (UDF 2.50) | 预留 metadata file 支持 / reserved metadata file support |

### genisoimage 非标布局容错 / genisoimage Non-Standard Layout Tolerance

genisoimage `-udf` 产出的镜像布局不规范，常见四类问题与对应策略：

Images produced by genisoimage `-udf` have non-standard layouts; the four
common problem classes and the strategies used:

| 问题 / Problem | 表现 / Symptom | 策略 / Strategy |
|------|------|------|
| LVD/PD 指针垃圾 / garbage LVD/PD pointers | FSD 指针为 0、PD partition start 无效 / FSD pointer 0, invalid PD partition start | anchor 后扫描定位真实 FSD，rebasing partition 0 为 FSD 扇区 / scan after the anchor to locate the real FSD, rebasing partition 0 to the FSD sector |
| 残留"假根" FE / leftover "fake root" FE | FSD 后第一个目录 FE 的 extent 极小（88 字节），只有 2 条垃圾 FID / first dir FE after FSD has a tiny extent (88 bytes) with only 2 junk FIDs | 取前 4 个目录 FE 候选逐一解析，选条目最多者作根 / parse the first 4 dir-FE candidates and pick the one with the most entries as root |
| 目录大小少报 / understated dir sizes | FE 的 information_length 小于真实目录数据（如 88 vs 3712 字节）/ FE information_length smaller than the real dir data (e.g. 88 vs 3712) | 目录数据按连续字节流解析，FID 放不下时自动扩展读取下一扇区 / parse dir data as a continuous byte stream, extending into the next sector when a FID does not fit |
| FID 跨扇区 / 续段扇区前部垃圾 / FIDs crossing sectors / junk at the start of continuation sectors | FID 头落在扇区尾部、名字延续到下一扇区；续段扇区 @0-7 是上一项名字的尾部+填充，FID 流从 @8 开始 / FID headers land at a sector tail with names continuing into the next sector; continuation sectors start with 8 bytes of previous-name tail + padding, so the FID stream begins at @8 | 连续字节流解析天然衔接；仅当 FID 头不完整（截断）时才扩展，并以 padding（全零）结束流 / continuous stream parsing joins them naturally; extend only when a FID header is incomplete (truncated), and terminate the stream on padding (all-zero) |

解析原则 / Parsing principles:

- 目录数据视为**连续字节流**，FID 可跨越 2048 字节扇区边界，不做逐扇区独立解析
  Directory data is treated as a **continuous byte stream**; FIDs may cross
  2048-byte sector boundaries and are never parsed sector by sector.
- 仅"FID 放不下当前已读数据"时扩展读入下一扇区（截断驱动），避免误吞相邻目录/文件数据
  Extend into the next sector only when a FID does not fit the data already
  read (truncation-driven), avoiding accidental swallowing of adjacent
  directory/file data.
- 非 FID tag（含全零 padding）视为流结束，不把垃圾扇区解析为条目
  Non-FID tags (including all-zero padding) mark the end of the stream;
  junk sectors are never parsed as entries.

## 禁止事项 / Forbidden Practices

- 不依赖 Windows Mount-DiskImage / Linux mount / macOS hdiutil
  No reliance on Windows Mount-DiskImage, Linux mount, or macOS hdiutil.
- 不进行文件解包
  No file unpacking.
- 不修改系统挂载状态
  No modification of the system's mount state.
- 不要求管理员权限
  No admin privileges required.

## Regression Test 要求 / Regression Test Requirements

测试集必须覆盖 / The test suite must cover:

- ISO9660 / Joliet / UDF 纯格式 / plain formats
- 中文 / 日文 / 韩文 / emoji 文件名 / Chinese, Japanese, Korean and emoji file names
- 长文件名 / long file names
- 混合 ISO/UDF / mixed ISO/UDF
- 非标准 UDF / non-standard UDF
- Windows 兼容但其他软件乱码的 UDF / UDF that Windows handles but other software garbles
