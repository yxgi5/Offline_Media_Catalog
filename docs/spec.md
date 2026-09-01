# Offline Media Catalog — Project Specification / 项目规格

> 项目需求规格文档（Phase 1），由 `spec.txt` 迁移为 Markdown 格式。
> Project requirements specification (Phase 1), migrated to Markdown from `spec.txt`.

| 元信息 / Metadata | 值 / Value |
|--------|-----|
| Version / 版本 | 0.1 |
| Phase / 阶段 | Phase 1 — Catalog Core / Scanner / ISO Provider / CLI |
| License / 许可证 | GPL-3.0-or-later |
| Primary goal / 主要目标 | 构建开源、跨平台的离线存储 Catalog 系统 / build an open-source, cross-platform offline storage catalog system |

---

## 1. 项目定位 / Project Positioning

开发一个开源、跨平台的离线存储 Catalog 软件。

Build an open-source, cross-platform offline storage catalog application.

软件用于记录用户拥有但当前可能处于离线状态的：

It records storage the user owns but which may currently be offline:

- 硬盘 / hard disks
- 分区 / Volume / partitions / volumes
- 光盘 / optical discs
- 普通目录 / plain directories
- ISO 镜像 / ISO images
- 未来可扩展的其他容器 / 镜像 / other containers/images (extensible in the future)

用户可以在不连接实际存储介质的情况下查询：

Users can query, without connecting the actual storage medium:

- 文件名 / file names
- 目录 / directories
- 路径 / paths
- 所属 Source / owning source
- 文件大小 / file sizes
- 时间戳 / timestamps
- 文件属性 / file attributes
- 可选校验码 / optional checksums
- ISO/UDF 内部目录 / ISO/UDF inner directories
- 容器内部文件 / files inside containers

核心目标类似 WhereIsIt，但项目本身不应该复制 WhereIsIt 的实现，而应该建立一个开放的、可扩展的数据模型和扫描框架。

The core goal is similar to WhereIsIt, but the project must not copy WhereIsIt's implementation; it should establish an open, extensible data model and scanning framework.

## 2. 核心使用场景 / Core Use Case

例如用户有 / For example, a user has:

```text
HDD-001
└── Software
    ├── Windows.iso
    ├── Ubuntu.iso
    └── OldTools.iso
```

扫描 HDD 后，Catalog 应保存 / After scanning the HDD, the catalog should store:

```text
HDD-001
└── Software
    ├── Windows.iso
    │   ├── boot
    │   ├── sources
    │   │   └── install.wim
    │   └── setup.exe
    ├── Ubuntu.iso
    │   ├── boot
    │   ├── casper
    │   └── ...
    └── OldTools.iso
```

即使 HDD-001 已经拔掉，用户仍然能够搜索 `install.wim` 得到：

Even after HDD-001 is unplugged, searching `install.wim` must still yield:

```text
HDD-001
Software/Windows.iso/sources/install.wim
```

这属于本项目的核心功能。

This is the core functionality of the project.

## 3. Source，而不是 Disk / Sources, Not Disks

扫描系统的最高层抽象必须是 **Source**，而不是 **Disk**。

The highest-level abstraction of the scanning system must be **Source**, not **Disk**.

因为 Catalog 的输入可能是：

Because the catalog's input can be:

- 整个物理磁盘 / a whole physical disk
- 一个分区 / Volume / a partition / volume
- 一个挂载目录 / a mounted directory
- 任意普通目录 / any plain directory
- 单独的 ISO 文件 / a standalone ISO file
- 未来的其他虚拟 / 远程文件系统 / future virtual/remote filesystems

例如以下全部都是合法输入 / All of the following are valid inputs:

```bash
offcat scan D:\ catalog.db
offcat scan D:\Software catalog.db
offcat scan /mnt/archive catalog.db
offcat scan /mnt/disk catalog.db
offcat scan backup.iso catalog.db
```

因此 Scanner API 应类似 `scanSource()`，而不是 `scanDisk()`。

Therefore the scanner API should look like `scanSource()`, not `scanDisk()`.

## 4. Source 类型 / Source Types

第一版至少支持 / The first version supports at least:

- `physical_disk`
- `volume`
- `directory`
- `file`
- `iso`
- `other`

其中 **`directory` 是重要的一等公民**。例如 `D:\Software` 可以直接建立一个 Catalog Source，不要求用户必须从磁盘根目录开始扫描。

**`directory` is an important first-class citizen**: for example `D:\Software`
can directly become a catalog source; users are not required to scan from the
disk root.

## 5. Source 与 Entry 的关系 / Source-Entry Relationship

核心关系 / The core relationship:

```text
Source
  │
  └── Entry
```

例如 / For example:

```text
Source:
    name = HDD-001
    type = volume

Entry:
    Software
      └── Windows.iso
```

如果直接扫描 `Windows.iso`，则 / If `Windows.iso` is scanned directly:

```text
Source:
    name = Windows.iso
    type = iso

Entry:
    Virtual Root
```

两种情况必须都支持 / Both cases must be supported.

## 6. Catalog 数据模型 / Catalog Data Model

第一版核心实体 / Core entities of the first version:

- Catalog
- Source
- Entry
- Container
- Checksum
- Scan

未来可以增加：Tag、VirtualFolder、Bookmark、Comment，但 **Phase 1 不实现**这些高级功能。

Tag, VirtualFolder, Bookmark and Comment may come later, but these advanced
features are **not implemented in Phase 1**.

## 7. SQLite

SQLite 是项目自己的核心 Catalog 存储格式。

SQLite is the project's own core catalog storage format.

要求 / Requirements:

- SQLite 3
- UTF-8
- FTS5
- WAL
- prepared statements / 预编译语句
- transaction-based bulk insertion / 基于事务的批量插入

数据库必须能够处理**百万级 Entry**。

The database must handle **millions of entries**.

## 8. Source 表 / The Source Table

建议 / Proposed:

```sql
CREATE TABLE source (
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    type            TEXT NOT NULL,
    source_path     TEXT,
    label           TEXT,
    serial          TEXT,
    filesystem      TEXT,
    size            INTEGER,
    created_at      INTEGER,
    cataloged_at    INTEGER
);
```

说明 / Notes:

- `name`：用户看到的 Source 名称 / the source name as shown to users
- `type`：Source 类型 / source type
- `source_path`：扫描时使用的路径，可为空 / path used at scan time, nullable
- `label`：Volume label
- `serial`：Volume/device serial
- `filesystem`：例如 NTFS、ext4、UDF / e.g. NTFS, ext4, UDF
- `size`：Source 容量 / source capacity
- `created_at`：如果可以获得 / when available
- `cataloged_at`：本次 Catalog 时间 / time of this cataloging

不要假设所有字段在所有平台都存在。

Do not assume every field exists on every platform.

## 9. Entry / 条目

Entry 表示文件或目录。建议 / An entry represents a file or directory. Proposed:

```sql
CREATE TABLE entry (
    id              INTEGER PRIMARY KEY,
    source_id       INTEGER NOT NULL,
    parent_id       INTEGER,
    name            TEXT NOT NULL,
    type            INTEGER NOT NULL,

    size            INTEGER,

    mtime           INTEGER,
    ctime           INTEGER,
    atime           INTEGER,
    birthtime       INTEGER,

    mode            INTEGER,
    attributes      INTEGER DEFAULT 0,

    FOREIGN KEY(source_id) REFERENCES source(id),
    FOREIGN KEY(parent_id) REFERENCES entry(id)
);
```

Entry type：`file`、`directory`、`symlink`、`other`。

Entry types: `file`, `directory`, `symlink`, `other`.

必须采用 `parent_id + name` 表示目录树，**不要把完整路径作为唯一存储方式**。

The tree must use `parent_id + name`; **do not store full paths as the sole representation**.

## 10. Unicode

这是本项目的重点。内部数据库统一使用 **UTF-8**，所有 Entry 名称必须以 UTF-8 保存。

This is a key focus of the project. The internal database uniformly uses
**UTF-8**, and every entry name must be stored as UTF-8.

平台转换 / Platform conversion:

| 平台 / Platform | 转换 / Conversion |
|------|------|
| Windows | UTF-16 → UTF-8 |
| Linux | UTF-8 |
| macOS | UTF-8 |

禁止将 ANSI、system code page、locale-dependent encoding 作为内部 Catalog 数据格式。

ANSI, the system code page and locale-dependent encodings are forbidden as
internal catalog data formats.

## 11. 时间戳 / Timestamps

Entry 支持 `mtime` / `ctime` / `atime` / `birthtime`，所有字段允许 `NULL`——因为不同操作系统和文件系统提供的信息不同。

Entries support `mtime` / `ctime` / `atime` / `birthtime`; all fields are
nullable, because different OSes and filesystems provide different information.

**Scanner 不得为了填充字段而伪造时间。**

**The scanner must never fabricate timestamps to fill fields.**

## 12. Container / 容器

任何 Entry 都可以成为 Container。例如 `Windows.iso` 同时属于：

Any entry can be a container. For example, `Windows.iso` is both:

- Entry：`type = file`
- Container：`type = iso`

```sql
CREATE TABLE container (
    id              INTEGER PRIMARY KEY,
    entry_id        INTEGER NOT NULL,
    type            TEXT NOT NULL,
    provider        TEXT,
    version         TEXT,

    FOREIGN KEY(entry_id) REFERENCES entry(id)
);
```

## 13. Virtual Entry / 虚拟条目

Container 内部的文件不是物理文件。例如 / Files inside a container are not physical files. For example:

```text
HDD
└── Windows.iso
    └── setup.exe
```

其中 `Windows.iso` 是 **Physical Entry**，而 `setup.exe` 是 **Virtual Entry**。

Here `Windows.iso` is a **physical entry**, while `setup.exe` is a **virtual entry**.

数据库模型必须能够区分 physical entry / virtual entry，但二者应该尽量共享统一的查询 API。

The database model must distinguish physical and virtual entries, yet both
should share a unified query API as much as possible.

## 14. Container 不进行解包 / No Container Unpacking

Container Provider 的职责是**解析容器中的目录和元数据，而不是解压文件**。

A container provider's job is to **parse the directories and metadata inside a
container, not to extract files**.

- ZIP：Provider 应直接读取 ZIP central directory / the provider should read the ZIP central directory directly
- ISO：

```text
foo.iso
 ↓
ISO/UDF parser
 ↓
Virtual filesystem tree
```

不需要把 ISO 挂载到操作系统。

Mounting the ISO into the OS is not needed.

## 15. Container 嵌套 / Container Nesting

数据库模型必须允许任意深度 / The database model must allow arbitrary depth:

```text
HDD
└── A.iso
    └── B.zip
        └── C.tar
            └── file.dat
```

但是默认扫描深度 `max_container_depth = 0`（仅识别容器，不展开）。

The default scan depth is `max_container_depth = 0` (containers are discovered but not expanded).

第一阶段策略 / Phase 1 strategy:

- 普通文件系统 → 扫描 / plain filesystems → scan
- ISO/UDF → 识别 + 可选展开 / ISO/UDF → discover, expand when requested
- 其他容器 → 暂不支持 / other containers → not supported yet

`max_container_depth` 已实现（`--depth` 0/1/2+）：0 仅识别容器、不展开（默认），
1 展开一层容器且内部目录树完整收录，2+ 逐层展开内嵌容器：

`max_container_depth` is implemented (`--depth` 0/1/2+): 0 discovers containers only, no expansion (default); 1 expands one level with the full inner tree; 2+ expands nested containers level by level:

```text
ISO
└── ZIP
    └── files
```
## 16. Container Discovery 与 Expansion 分离 / Discovery vs Expansion

即使不展开 Container，也应该允许识别 `foo.iso` 是 `container_type = ISO`。

Even without expanding a container, `foo.iso` must still be recognizable as `container_type = ISO`.

因此 **Discovery** 和 **Expansion** 是两个不同概念。Discovery 已实现：
默认（`--depth 0`）扫描即识别容器并写入 `container` 表；Expansion 由
`--depth 1+` 控制。未来用户还可以：

So **discovery** and **expansion** are two distinct concepts. Discovery is
implemented: a scan at the default `--depth 0` still recognizes containers
and records them in the `container` table; expansion is gated by `--depth 1+`.
In the future users can also:

- 只发现容器，不扫描内部 / discover containers without scanning inside
- 对已经 Catalog 的 Container 重新展开 / re-expand already-cataloged containers

## 17. Container Provider API / 容器 Provider API

定义稳定的 Provider 接口 / Define a stable provider interface:

```cpp
class ContainerProvider
{
public:
    virtual ~ContainerProvider() = default;

    virtual std::string type() const = 0;

    virtual bool probe(
        const FileEntry& file
    ) = 0;

    virtual bool scan(
        const ContainerEntry& container,
        VirtualTreeWriter& writer,
        const ContainerOptions& options
    ) = 0;
};
```

Provider **不负责**：SQLite、GUI、Search、Scan scheduling、Catalog management。

A provider is **not responsible for**: SQLite, GUI, search, scan scheduling, catalog management.

Provider **只负责**：解析自己的格式并生成 Virtual Entries。

A provider is **only responsible for**: parsing its own format and producing virtual entries.

## 18. Provider Registry / Provider 注册表

Scanner 使用 `ProviderRegistry` / The scanner uses `ProviderRegistry`:

```text
ProviderRegistry
 ├── ISO Provider
 ├── future ZIP Provider
 ├── future 7z Provider
 └── future VHD Provider
```

Phase 1 只实现 ISO Provider。

Phase 1 implements only the ISO provider.

## 19. ISO Provider / ISO 提供器

ISO Provider 是 Phase 1 最重要的 Provider，必须支持：

The ISO provider is the most important provider in Phase 1 and must support:

- ISO9660
- Joliet
- UDF

尤其重点支持**现实世界中非标准或兼容性较差的 UDF ISO**——Windows 可以正确挂载、但某些 Catalog 软件解析后出现乱码的 ISO。这种 ISO 必须作为 regression test。

Special emphasis goes to **real-world non-standard or poorly compatible UDF
ISOs** — images Windows mounts fine but some catalog software garbles. Such
ISOs must become regression tests.

## 20. ISO Provider 禁止依赖系统挂载 / No System-Mount Dependency

正常扫描路径不能依赖 Windows `Mount-DiskImage`、Linux `mount`、macOS `hdiutil`，而应该：

The normal scan path must not rely on Windows `Mount-DiskImage`, Linux `mount`
or macOS `hdiutil`; instead:

```text
ISO file
   ↓
direct parser
   ↓
Virtual filesystem
```

这样保证：跨平台、离线、无管理员权限依赖、不需要修改系统挂载状态。

This guarantees: cross-platform, offline, no admin-rights dependency, and no
modification of the system mount state.

## 21. UDF Unicode

UDF 文件名必须按照 UDF/OSTA Compressed Unicode 规则解析，不能简单 `reinterpret bytes as UTF-8`。

UDF file names must be parsed per the UDF/OSTA Compressed Unicode rules, never
by simply `reinterpreting bytes as UTF-8`.

必须处理：Compression ID、Character length、CS0、UTF-16BE、surrogate pairs、invalid sequences。

Must handle: compression ID, character length, CS0, UTF-16BE, surrogate pairs and invalid sequences.

对于非标准 ISO / For non-standard ISOs:

```text
strict parser
    ↓ failure
compatibility parser
    ↓
heuristic recovery
```

允许兼容性恢复，但必须记录解析方式，例如：

Compatibility recovery is allowed, but the parsing mode must be recorded, e.g.:

```text
name_encoding = udf-cs0
name_confidence = 100
```

或 / or:

```text
name_encoding = compatibility
name_confidence = 80
```

## 22. 多文件系统 ISO / Multi-Filesystem ISOs

如果 ISO 同时包含 ISO9660、Joliet、UDF，Provider 必须能够识别这些 filesystem，不能简单假设"第一个发现的 filesystem = 正确 filesystem"，需要定义 filesystem selection / fallback 策略。

If an ISO contains ISO9660, Joliet and UDF at once, the provider must recognize
all of them; it must not assume "the first filesystem found is the correct
one", and needs a defined filesystem selection/fallback strategy.

Source/Container metadata 应记录 `filesystem = UDF`，必要时记录 `available_filesystems`。

Source/container metadata should record `filesystem = UDF` and, when
necessary, `available_filesystems`.

## 23. Checksum / 校验码

Checksum 是可选功能，默认 **disabled**。Phase 1 支持：SHA-256、MD5、CRC32。

Checksums are optional and **disabled by default**. Phase 1 supports SHA-256, MD5 and CRC32.

采用独立表 / Use a separate table:

```sql
CREATE TABLE checksum (
    entry_id        INTEGER NOT NULL,
    algorithm       TEXT NOT NULL,
    value           BLOB NOT NULL,
    calculated_at   INTEGER NOT NULL,

    PRIMARY KEY(entry_id, algorithm),

    FOREIGN KEY(entry_id) REFERENCES entry(id)
);
```

未来可以增加 SHA-1、SHA-512、BLAKE3、xxHash 而不改变 Entry。

SHA-1, SHA-512, BLAKE3 and xxHash can be added later without changing the entry table.

## 24. Fingerprint / 指纹

预留轻量级 fingerprint 能力，用途：

Reserve a lightweight fingerprint capability, used for:

- 快速判断重复文件 / fast duplicate detection
- 避免所有文件都计算 SHA-256 / avoiding SHA-256 on every file
- 未来实现跨 Catalog 去重 / cross-catalog deduplication in the future

Phase 1 可以只预留 API，不强制实现。例如 `size + fast fingerprint + optional SHA-256` 形成多级验证。

Phase 1 may reserve the API only, without forcing an implementation. For
example `size + fast fingerprint + optional SHA-256` forms multi-level
verification.

## 25. Scan / 扫描记录

记录每一次扫描 / Record every scan:

```sql
CREATE TABLE scan (
    id              INTEGER PRIMARY KEY,
    source_id       INTEGER NOT NULL,
    started_at      INTEGER NOT NULL,
    finished_at     INTEGER,
    scanner_version TEXT,
    options         TEXT,
    status          INTEGER,

    FOREIGN KEY(source_id) REFERENCES source(id)
);
```

`options` 使用 JSON，例如 / `options` uses JSON, e.g.:

```json
{
    "checksum": ["sha256"],
    "max_container_depth": 1
}
```

这样能够追溯：Catalog 是什么时候、由哪个版本、使用什么扫描选项产生的。

This makes it traceable when the catalog was created, by which version, and with which scan options.

## 26. Scan Options / 扫描选项

至少支持以下组合 / Support at least these combinations:

- metadata / 元数据
- metadata + checksum / 元数据 + 校验码
- metadata + containers / 元数据 + 容器
- metadata + checksum + containers / 元数据 + 校验码 + 容器

CLI 示例 / CLI example:

```bash
offcat scan /mnt/archive catalog.db
```

默认记录：names、directories、sizes、timestamps、attributes。

By default it records: names, directories, sizes, timestamps and attributes.

展开容器 / Expanding containers:

```bash
offcat scan --depth 1 /mnt/archive catalog.db
```

启用 SHA-256 / Enabling SHA-256:

```bash
offcat scan --checksum sha256 /mnt/archive catalog.db
```

## 27. Search / 搜索

使用 SQLite FTS5，至少支持 / Use SQLite FTS5, supporting at least:

- filename search / 文件名搜索
- path search / 路径搜索

例如 / For example:

```bash
offcat search catalog.db ubuntu
```

输出 / Output:

```text
HDD-001
Software/Linux/ubuntu.iso

HDD-017
Backup/ubuntu-22.04.iso
```

搜索结果必须能够区分 Physical Entry / Virtual Entry，并显示 Container 层级。

Search results must distinguish physical and virtual entries and show the container hierarchy.

## 28. 路径显示 / Path Display

数据库不依赖完整绝对路径，通过 `parent_id + name` 构造路径。

The database does not rely on full absolute paths; paths are built from `parent_id + name`.

例如 / For example:

```text
Source:  HDD-001
Entry:   Software > Windows.iso > sources > install.wim
```

UI/CLI 显示 / The UI/CLI shows:

```text
HDD-001
Software/Windows.iso/sources/install.wim
```

这样 Catalog 不会过度依赖创建它的计算机。

This way the catalog does not depend too heavily on the machine that created it.

## 29. Source Path / 源路径

Source 可以保存 `source_path`，但它只是**扫描时的路径信息，不能把它作为文件身份**。

A source may store `source_path`, but it is only **path information at scan
time and must not be treated as file identity**.

例如 `D:\Software` 以后移动到 `E:\OldSoftware`，Catalog 中的历史记录仍然有效。

For example, if `D:\Software` is later moved to `E:\OldSoftware`, the historical records in the catalog remain valid.

## 30. 大规模扫描 / Large-Scale Scanning

必须考虑百万级 Entry，要求 / Millions of entries must be considered; requirements:

- prepared statements / 预编译语句
- batch insert / 批量插入
- SQLite transactions / SQLite 事务
- WAL
- indexes / 索引
- 避免逐 Entry commit / no per-entry commits
- 可取消 / cancellable
- 内存使用稳定 / stable memory usage

目标不是极限 benchmark，而是：扫描大型离线 HDD 时不会因为数据库设计导致性能不可接受。

The goal is not an extreme benchmark but: scanning a large offline HDD must
not become unacceptably slow due to database design.

## 31. Cancellation / 取消

Scanner 必须支持取消（例如 Ctrl+C），要求：

The scanner must support cancellation (e.g. Ctrl+C), with these requirements:

- 停止后续扫描 / stop further scanning
- 正确结束 transaction / finish the transaction correctly
- SQLite 不损坏 / SQLite stays uncorrupted
- 已完成的数据可以保留 / already-scanned data is kept
- Scan 状态标记为 cancelled / the scan status is marked cancelled

## 32. 错误处理 / Error Handling

单个文件无法读取时，不能导致整个 Scan 失败。例如 100000 files 中 1 file permission denied，应该：

An unreadable single file must not fail the whole scan. For example, 1 file
with permission denied among 100,000 should:

- 记录 warning / log a warning
- 继续扫描 / keep scanning

Container 损坏（如 `bad.iso`）时应该 / A corrupt container (e.g. `bad.iso`) should:

- 记录 Container error / log a container error
- 继续扫描其他文件 / keep scanning other files

## 33. Logging / 日志

至少提供：`quiet` / `normal` / `verbose` / `debug`。

Provide at least: `quiet` / `normal` / `verbose` / `debug`.

CLI 可以 `offcat scan --verbose ...`。调试 ISO/UDF 时可以看到：

The CLI supports `offcat scan --verbose ...`. When debugging ISO/UDF you can see:

```text
Detected UDF
Volume Identifier: XXXXX
Unicode mode: CS0
Entry count: XXXXX
```

## 34. 测试数据 / Test Data

必须建立真实测试集，至少覆盖：

Real test data must be established, covering at least:

- ISO9660
- Joliet
- UDF
- Chinese filenames / 中文文件名
- Japanese filenames / 日文文件名
- Korean filenames / 韩文文件名
- emoji filenames / emoji 文件名
- long filenames / 长文件名
- mixed ISO/UDF / 混合 ISO/UDF
- non-standard UDF / 非标准 UDF
- Windows-compatible malformed UDF / Windows 兼容但格式错误的 UDF

特别是现实中出现"Windows 挂载正常，但 Catalog 软件出现乱码"的 ISO，这些必须成为 regression tests。

In particular, real-world ISOs where "Windows mounts fine but catalog software
shows mojibake" must become regression tests.

## 35. Unicode Regression Test / Unicode 回归测试

例如 / For example:

```text
测试 ISO
├── 中文目录
│   └── 测试文件.txt
├── 日本語
│   └── ファイル.txt
└── emoji
    └── 😀.txt
```

解析结果必须保持 UTF-8，且名称不能发生：乱码、截断、非法 UTF-8。

Parsed results must remain UTF-8, and names must not suffer: mojibake,
truncation, or invalid UTF-8.
## 36. CLI / 命令行

Phase 1 提供 `offcat`，至少 / Phase 1 ships `offcat`, with at least:

- `offcat create`
- `offcat scan`
- `offcat search`
- `offcat info`

示例 / Examples:

```bash
offcat scan D:\ catalog.db
offcat scan D:\Software catalog.db
offcat scan backup.iso catalog.db
offcat search catalog.db "install.wim"
offcat info catalog.db
```

## 37. GUI

Phase 1 不实现 GUI。未来 GUI 可以 / Phase 1 does not implement a GUI. A future GUI may use:

- Qt 6
- 或者研究将 **VVV** 作为现有开源 GUI 前端 / or research **VVV** as an existing open-source GUI frontend

但必须保证 GUI 不参与 Catalog 核心逻辑。最终架构：

But the GUI must never participate in catalog core logic. Final architecture:

```text
                 Catalog Core
                      │
              ┌───────┴───────┐
              │               │
             CLI             GUI
                              │
                         Qt / VVV
```

## 38. VVV / WinCatalog / WhereIsIt 兼容性 / Compatibility

项目应该研究 VVV、WinCatalog、WhereIsIt 的数据模型。目标不是复制它们的私有实现，而是找出成熟 Catalog 软件普遍需要表达的数据。

The project should study the data models of VVV, WinCatalog and WhereIsIt.
The goal is not to copy their private implementations but to find the data
that mature catalog software universally needs to express.

重点研究：Source / Media、Entry、Directory、File、Timestamp、Attributes、Checksum、Container、Virtual Entry、Tag。

Focus areas: source/media, entry, directory, file, timestamp, attributes, checksum, container, virtual entry, tag.

## 39. Export / Import / 导入导出

未来设计 / Future design:

```text
Catalog DB
   │
   ├── VVV exporter
   ├── WinCatalog exporter
   ├── WhereIsIt exporter
   └── JSON exporter
```

Phase 1 只预留架构，不实现第三方格式写入。

Phase 1 only reserves the architecture; no third-party format writers are implemented.

特别注意：SQLite 本身并不意味着可以直接兼容 WinCatalog 的 `.w3cat`。WinCatalog 的 SQLite schema 应作为研究资料，而不是直接作为本项目数据库 schema。

Note: SQLite by itself does not imply direct compatibility with WinCatalog's
`.w3cat`. WinCatalog's SQLite schema should serve as research material, not as
this project's database schema.

## 40. 图片、描述、封面 / Images, Descriptions, Covers

Phase 1 明确不实现：thumbnail、cover、preview、description、user notes。

Phase 1 explicitly does not implement: thumbnails, covers, previews, descriptions and user notes.

这些属于 Digital Asset Management，而不是本项目核心目标。未来如果需要，可以通过 extension table 实现，核心 Catalog 不应该被这些功能污染。

These belong to digital asset management, not the core goal of this project.
If needed later, they can be implemented via extension tables; the core
catalog must not be polluted by them.

## 41. Tags / Virtual Folders / 标签与虚拟文件夹

同样，Tag、Virtual Folder、Bookmark、Comment 在 Phase 1 不实现，但是数据库设计应该避免以后无法添加。

Likewise, tags, virtual folders, bookmarks and comments are not implemented in
Phase 1, but the database design must not preclude adding them later.

## 42. 插件架构 / Plugin Architecture

Provider API 必须从一开始保持独立。未来可以：

The provider API must stay independent from day one. In the future:

- ISO Provider
- ZIP Provider
- 7z Provider
- RAR Provider
- TAR Provider
- VHD Provider
- VHDX Provider
- DMG Provider

Phase 1：ISO/UDF only。不要为了证明架构而实现大量 Provider。

Phase 1: ISO/UDF only. Do not implement many providers just to prove the architecture.

## 43. Provider 安全限制 / Provider Safety Limits

未来 Archive Provider 必须支持 / Future archive providers must support:

- `max_depth`
- `max_entries`
- `max_virtual_size`
- `max_scan_time`

避免：zip bomb、recursive container、path traversal、巨大虚拟目录、恶意容器。

Avoid: zip bombs, recursive containers, path traversal, huge virtual trees and malicious containers.

Phase 1 ISO Provider 也应保留这些限制接口。

The Phase 1 ISO provider should also keep these limit interfaces.

## 44. 项目目录结构 / Project Directory Layout

推荐 / Recommended:

```text
offcat/
│
├── CMakeLists.txt
├── LICENSE
├── README.md
│
├── docs/
│   ├── architecture.md
│   ├── catalog-schema.md
│   ├── provider-api.md
│   └── compatibility.md
│
├── src/
│   ├── core/
│   ├── catalog/
│   ├── scanner/
│   ├── filesystem/
│   ├── container/
│   ├── database/
│   └── platform/
│
├── providers/
│   └── iso/
│       ├── iso9660/
│       ├── joliet/
│       └── udf/
│
├── cli/
│
└── tests/
    ├── database/
    ├── scanner/
    ├── unicode/
    ├── iso9660/
    ├── joliet/
    ├── udf/
    └── container/
```

## 45. 跨平台要求 / Cross-Platform Requirements

目标：Windows、Linux、macOS。

Targets: Windows, Linux, macOS.

Core 不得依赖：Win32、Qt、GTK、systemd、Linux mount、Windows Mount-DiskImage。

The core must not depend on: Win32, Qt, GTK, systemd, Linux mount or Windows Mount-DiskImage.

平台相关代码集中到 `src/platform/`。

Platform-specific code is confined to `src/platform/`.

## 46. 第一阶段完成标准 / Phase 1 Completion Criteria

Phase 1 只有在以下功能全部完成后才算完成：

Phase 1 is complete only when all of the following are done:

- [x] CMake build / CMake 构建
- [x] C++17
- [x] Windows build / Windows 构建
- [x] Linux build / Linux 构建
- [x] macOS build / macOS 构建
- [x] SQLite Catalog
- [x] Source model / Source 模型
- [x] Directory scanner / 目录扫描器
- [x] File scanner / 文件扫描器
- [x] UTF-8 internal representation / UTF-8 内部表示
- [x] timestamps / 时间戳
- [x] attributes / 属性
- [x] FTS5 search / FTS5 搜索
- [x] Container abstraction / 容器抽象
- [x] Virtual Entry abstraction / 虚拟条目抽象
- [x] Container depth / 容器深度
- [x] ISO9660
- [x] Joliet
- [x] UDF
- [x] UDF Unicode
- [x] non-standard UDF compatibility / 非标准 UDF 兼容
- [x] optional MD5 / 可选 MD5
- [x] optional SHA-256 / 可选 SHA-256
- [x] Scan metadata / 扫描元数据
- [x] cancellation / 取消
- [x] error recovery / 错误恢复
- [x] million-entry test / 百万条目测试
- [x] regression tests / 回归测试
- [x] documentation / 文档

## 47. 明确禁止 Coding Agent 在 Phase 1 做的事情 / Explicitly Forbidden in Phase 1

Coding agent 不要自行扩大项目范围。禁止主动实现：

The coding agent must not expand the project scope on its own. Actively implementing the following is forbidden:

- GUI
- ZIP
- 7z
- RAR
- TAR
- VHD
- VHDX
- DMG
- 全文搜索 / full-text search (as a separate product feature)
- 图片缩略图 / image thumbnails
- 媒体播放器 / media player
- 云同步 / cloud sync
- 网络数据库 / networked database
- WhereIsIt CTF writer
- WinCatalog W3CAT writer
- VVV GUI integration

除非明确收到新的任务。

Unless explicitly tasked.

## 48. 开发优先级 / Development Priority

严格按照 / Strictly in this order:

1. Catalog schema / Catalog 结构
2. Database layer / 数据库层
3. Source / Entry model / Source/Entry 模型
4. Filesystem scanner / 文件系统扫描器
5. Search / 搜索
6. Container API / 容器 API
7. ISO9660
8. Joliet
9. UDF
10. UDF Unicode compatibility / UDF Unicode 兼容
11. checksum / 校验码
12. CLI
13. performance tests / 性能测试
14. documentation / 文档

不要先做 GUI。

Do not start with a GUI.

## 49. 最重要的架构原则 / The Most Important Architectural Principles

1. **Catalog catalogs Sources, not Disks.** / **Catalog 编目 Source，而不是 Disk。**
2. **A physical file can also be a Container.** / **物理文件可以同时是 Container。**
3. **Container contents are Virtual Entries, not extracted physical files.** / **Container 内容是 Virtual Entry，不是解压的物理文件。**
4. **Container parsing must not require mounting or extraction.** / **Container 解析不需要挂载或解压。**
5. **The database model supports nested containers, but scanning depth is configurable and limited.** / **数据库支持嵌套容器，扫描深度可配置且有限。**
6. **ISO9660/Joliet/UDF are first-class Phase 1 functionality.** / **ISO9660/Joliet/UDF 是 Phase 1 一等公民功能。**
7. **Other container formats are extensions, not core functionality.** / **其他容器格式是扩展，不是核心功能。**
8. **UTF-8 is the internal string representation.** / **UTF-8 是内部字符串表示。**
9. **Optional metadata such as checksums must not impose scanning cost when disabled.** / **校验码等可选元数据在禁用时不得增加扫描开销。**
10. **The Catalog database is independent from the GUI and export formats.** / **Catalog 数据库独立于 GUI 与导出格式。**

## 50. 给 Coding Agent 的最终指令 / Final Instructions to the Coding Agent

Build Phase 1 of an open-source, cross-platform offline media catalog system according to this specification.

根据本规格构建开源、跨平台离线媒体 Catalog 系统的 Phase 1。

- Do not implement a GUI in Phase 1. / Phase 1 不实现 GUI。
- The primary deliverable is a clean Catalog Core consisting of: SQLite database, Source/Entry data model, filesystem scanner, FTS5 search, Container abstraction, Virtual Entry abstraction, ISO9660/Joliet/UDF provider, robust UDF Unicode handling, optional checksum calculation, scan metadata, configurable container depth, CLI, automated tests.
  主要交付物是一个干净的 Catalog Core：SQLite 数据库、Source/Entry 数据模型、文件系统扫描器、FTS5 搜索、容器抽象、虚拟条目抽象、ISO9660/Joliet/UDF Provider、健壮的 UDF Unicode 处理、可选校验码、扫描元数据、可配置容器深度、CLI、自动化测试。
- The architecture must allow future Container Providers and Exporters to be added without changing the core database model.
  架构必须允许未来在不修改核心数据库模型的前提下添加新的 Container Provider 与 Exporter。
- Do not implement ZIP/7z/RAR or other archive formats in Phase 1. / Phase 1 不实现 ZIP/7z/RAR 等归档格式。
- Do not implement GUI, thumbnails, descriptions, media preview, cloud synchronization, or third-party catalog writers in Phase 1.
  Phase 1 不实现 GUI、缩略图、描述、媒体预览、云同步或第三方 Catalog 写入器。
- Prioritize correctness, Unicode handling, UDF compatibility, database integrity, scalability, cancellation, testability, and clean separation of concerns over feature count.
  优先考虑正确性、Unicode 处理、UDF 兼容性、数据库完整性、可扩展性、可取消、可测试性与清晰的关注点分离，而非功能数量。
- In particular, treat real-world non-standard UDF ISO images as an important compatibility requirement. The parser must not assume that every ISO image encountered in the wild strictly follows the ideal standard.
  尤其要把现实世界的非标准 UDF ISO 镜像视为重要兼容性要求。解析器不得假设现实中遇到的每个 ISO 镜像都严格遵守理想标准。

---

## 补充：技术栈 / Appendix: Tech Stack

| 项 / Item | 选择 / Choice |
|----|------|
| Language / 语言 | C++17 |
| Build / 构建 | CMake |
| Database / 数据库 | SQLite 3 |
| Search / 搜索 | SQLite FTS5 |
| Testing / 测试 | GoogleTest |
| CLI | C++17 |
| GUI | Not implemented in Phase 1 / Phase 1 不实现 |

最终架构 / Final architecture:

```text
                offcat
                  │
          ┌───────┴───────┐
          │               │
     Catalog Core        CLI
          │
    ┌─────┼──────┐
    │     │      │
 SQLite Scanner Container
                 │
              ISO/UDF
```
