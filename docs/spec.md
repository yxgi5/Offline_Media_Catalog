# Offline Media Catalog — Project Specification

> 项目需求规格文档（Phase 1），由 `spec.txt` 迁移为 Markdown 格式。

| 元信息 | 值 |
|--------|-----|
| Version | 0.1 |
| Phase | Phase 1 — Catalog Core / Scanner / ISO Provider / CLI |
| License | GPL-3.0-or-later |
| Primary goal | 构建开源、跨平台的离线存储 Catalog 系统 |

---

## 1. 项目定位

开发一个开源、跨平台的离线存储 Catalog 软件。

软件用于记录用户拥有但当前可能处于离线状态的：

- 硬盘
- 分区 / Volume
- 光盘
- 普通目录
- ISO 镜像
- 未来可扩展的其他容器 / 镜像

用户可以在不连接实际存储介质的情况下查询：

- 文件名
- 目录
- 路径
- 所属 Source
- 文件大小
- 时间戳
- 文件属性
- 可选校验码
- ISO/UDF 内部目录
- 容器内部文件

核心目标类似 WhereIsIt，但项目本身不应该复制 WhereIsIt 的实现，而应该建立一个开放的、可扩展的数据模型和扫描框架。

## 2. 核心使用场景

例如用户有：

```text
HDD-001
└── Software
    ├── Windows.iso
    ├── Ubuntu.iso
    └── OldTools.iso
```

扫描 HDD 后，Catalog 应保存：

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

```text
HDD-001
Software/Windows.iso/sources/install.wim
```

这属于本项目的核心功能。

## 3. Source，而不是 Disk

扫描系统的最高层抽象必须是 **Source**，而不是 **Disk**。

因为 Catalog 的输入可能是：

- 整个物理磁盘
- 一个分区 / Volume
- 一个挂载目录
- 任意普通目录
- 单独的 ISO 文件
- 未来的其他虚拟 / 远程文件系统

例如以下全部都是合法输入：

```bash
offcat scan D:\ catalog.db
offcat scan D:\Software catalog.db
offcat scan /mnt/archive catalog.db
offcat scan /mnt/disk catalog.db
offcat scan backup.iso catalog.db
```

因此 Scanner API 应类似 `scanSource()`，而不是 `scanDisk()`。

## 4. Source 类型

第一版至少支持：

- `physical_disk`
- `volume`
- `directory`
- `file`
- `iso`
- `other`

其中 **`directory` 是重要的一等公民**。例如 `D:\Software` 可以直接建立一个 Catalog Source，不要求用户必须从磁盘根目录开始扫描。

## 5. Source 与 Entry 的关系

核心关系：

```text
Source
  │
  └── Entry
```

例如：

```text
Source:
    name = HDD-001
    type = volume

Entry:
    Software
      └── Windows.iso
```

如果直接扫描 `Windows.iso`，则：

```text
Source:
    name = Windows.iso
    type = iso

Entry:
    Virtual Root
```

两种情况必须都支持。

## 6. Catalog 数据模型

第一版核心实体：

- Catalog
- Source
- Entry
- Container
- Checksum
- Scan

未来可以增加：Tag、VirtualFolder、Bookmark、Comment，但 **Phase 1 不实现**这些高级功能。

## 7. SQLite

SQLite 是项目自己的核心 Catalog 存储格式。

要求：

- SQLite 3
- UTF-8
- FTS5
- WAL
- prepared statements
- transaction-based bulk insertion

数据库必须能够处理**百万级 Entry**。

## 8. Source 表

建议：

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

说明：

- `name`：用户看到的 Source 名称
- `type`：Source 类型
- `source_path`：扫描时使用的路径，可为空
- `label`：Volume label
- `serial`：Volume/device serial
- `filesystem`：例如 NTFS、ext4、UDF
- `size`：Source 容量
- `created_at`：如果可以获得
- `cataloged_at`：本次 Catalog 时间

不要假设所有字段在所有平台都存在。

## 9. Entry

Entry 表示文件或目录。建议：

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

必须采用 `parent_id + name` 表示目录树，**不要把完整路径作为唯一存储方式**。

## 10. Unicode

这是本项目的重点。内部数据库统一使用 **UTF-8**，所有 Entry 名称必须以 UTF-8 保存。

平台转换：

| 平台 | 转换 |
|------|------|
| Windows | UTF-16 → UTF-8 |
| Linux | UTF-8 |
| macOS | UTF-8 |

禁止将 ANSI、system code page、locale-dependent encoding 作为内部 Catalog 数据格式。

## 11. 时间戳

Entry 支持 `mtime` / `ctime` / `atime` / `birthtime`，所有字段允许 `NULL`——因为不同操作系统和文件系统提供的信息不同。

**Scanner 不得为了填充字段而伪造时间。**

## 12. Container

任何 Entry 都可以成为 Container。例如 `Windows.iso` 同时属于：

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

## 13. Virtual Entry

Container 内部的文件不是物理文件。例如：

```text
HDD
└── Windows.iso
    └── setup.exe
```

其中 `Windows.iso` 是 **Physical Entry**，而 `setup.exe` 是 **Virtual Entry**。

数据库模型必须能够区分 physical entry / virtual entry，但二者应该尽量共享统一的查询 API。

## 14. Container 不进行解包

Container Provider 的职责是**解析容器中的目录和元数据，而不是解压文件**。

- ZIP：Provider 应直接读取 ZIP central directory
- ISO：

```text
foo.iso
 ↓
ISO/UDF parser
 ↓
Virtual filesystem tree
```

不需要把 ISO 挂载到操作系统。

## 15. Container 嵌套

数据库模型必须允许任意深度：

```text
HDD
└── A.iso
    └── B.zip
        └── C.tar
            └── file.dat
```

但是默认扫描深度 `max_container_depth = 1`。

第一阶段策略：

- 普通文件系统 → 扫描
- ISO/UDF → 展开
- 其他容器 → 暂不支持

`max_container_depth` 已实现（`--depth` 0/1/2+）：0 不展开任何容器，
1 展开一层容器且内部目录树完整收录，2+ 逐层展开内嵌容器：

```text
ISO
└── ZIP
    └── files
```

## 16. Container Discovery 与 Expansion 分离

即使不展开 Container，也应该允许识别 `foo.iso` 是 `container_type = ISO`。

因此 **Discovery** 和 **Expansion** 是两个不同概念。未来用户可以：

- 只发现容器，不扫描内部
- 对已经 Catalog 的 Container 重新展开

## 17. Container Provider API

定义稳定的 Provider 接口：

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

Provider **只负责**：解析自己的格式并生成 Virtual Entries。

## 18. Provider Registry

Scanner 使用 `ProviderRegistry`：

```text
ProviderRegistry
 ├── ISO Provider
 ├── future ZIP Provider
 ├── future 7z Provider
 └── future VHD Provider
```

Phase 1 只实现 ISO Provider。

## 19. ISO Provider

ISO Provider 是 Phase 1 最重要的 Provider，必须支持：

- ISO9660
- Joliet
- UDF

尤其重点支持**现实世界中非标准或兼容性较差的 UDF ISO**——Windows 可以正确挂载、但某些 Catalog 软件解析后出现乱码的 ISO。这种 ISO 必须作为 regression test。

## 20. ISO Provider 禁止依赖系统挂载

正常扫描路径不能依赖 Windows `Mount-DiskImage`、Linux `mount`、macOS `hdiutil`，而应该：

```text
ISO file
   ↓
direct parser
   ↓
Virtual filesystem
```

这样保证：跨平台、离线、无管理员权限依赖、不需要修改系统挂载状态。

## 21. UDF Unicode

UDF 文件名必须按照 UDF/OSTA Compressed Unicode 规则解析，不能简单 `reinterpret bytes as UTF-8`。

必须处理：Compression ID、Character length、CS0、UTF-16BE、surrogate pairs、invalid sequences。

对于非标准 ISO：

```text
strict parser
    ↓ failure
compatibility parser
    ↓
heuristic recovery
```

允许兼容性恢复，但必须记录解析方式，例如：

```text
name_encoding = udf-cs0
name_confidence = 100
```

或：

```text
name_encoding = compatibility
name_confidence = 80
```

## 22. 多文件系统 ISO

如果 ISO 同时包含 ISO9660、Joliet、UDF，Provider 必须能够识别这些 filesystem，不能简单假设"第一个发现的 filesystem = 正确 filesystem"，需要定义 filesystem selection / fallback 策略。

Source/Container metadata 应记录 `filesystem = UDF`，必要时记录 `available_filesystems`。

## 23. Checksum

Checksum 是可选功能，默认 **disabled**。Phase 1 支持：SHA-256、MD5、CRC32。

采用独立表：

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

## 24. Fingerprint

预留轻量级 fingerprint 能力，用途：

- 快速判断重复文件
- 避免所有文件都计算 SHA-256
- 未来实现跨 Catalog 去重

Phase 1 可以只预留 API，不强制实现。例如 `size + fast fingerprint + optional SHA-256` 形成多级验证。

## 25. Scan

记录每一次扫描：

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

`options` 使用 JSON，例如：

```json
{
    "checksum": ["sha256"],
    "containers": true,
    "max_container_depth": 1
}
```

这样能够追溯：Catalog 是什么时候、由哪个版本、使用什么扫描选项产生的。

## 26. Scan Options

至少支持以下组合：

- metadata
- metadata + checksum
- metadata + containers
- metadata + checksum + containers

CLI 示例：

```bash
offcat scan /mnt/archive catalog.db
```

默认记录：names、directories、sizes、timestamps、attributes。

启用容器：

```bash
offcat scan --containers /mnt/archive catalog.db
```

启用 SHA-256：

```bash
offcat scan --checksum sha256 /mnt/archive catalog.db
```

## 27. Search

使用 SQLite FTS5，至少支持：

- filename search
- path search

例如：

```bash
offcat search catalog.db ubuntu
```

输出：

```text
HDD-001
Software/Linux/ubuntu.iso

HDD-017
Backup/ubuntu-22.04.iso
```

搜索结果必须能够区分 Physical Entry / Virtual Entry，并显示 Container 层级。

## 28. 路径显示

数据库不依赖完整绝对路径，通过 `parent_id + name` 构造路径。

例如：

```text
Source:  HDD-001
Entry:   Software > Windows.iso > sources > install.wim
```

UI/CLI 显示：

```text
HDD-001
Software/Windows.iso/sources/install.wim
```

这样 Catalog 不会过度依赖创建它的计算机。

## 29. Source Path

Source 可以保存 `source_path`，但它只是**扫描时的路径信息，不能把它作为文件身份**。

例如 `D:\Software` 以后移动到 `E:\OldSoftware`，Catalog 中的历史记录仍然有效。

## 30. 大规模扫描

必须考虑百万级 Entry，要求：

- prepared statements
- batch insert
- SQLite transactions
- WAL
- indexes
- 避免逐 Entry commit
- 可取消
- 内存使用稳定

目标不是极限 benchmark，而是：扫描大型离线 HDD 时不会因为数据库设计导致性能不可接受。

## 31. Cancellation

Scanner 必须支持取消（例如 Ctrl+C），要求：

- 停止后续扫描
- 正确结束 transaction
- SQLite 不损坏
- 已完成的数据可以保留
- Scan 状态标记为 cancelled

## 32. 错误处理

单个文件无法读取时，不能导致整个 Scan 失败。例如 100000 files 中 1 file permission denied，应该：

- 记录 warning
- 继续扫描

Container 损坏（如 `bad.iso`）时应该：

- 记录 Container error
- 继续扫描其他文件

## 33. Logging

至少提供：`quiet` / `normal` / `verbose` / `debug`。

CLI 可以 `offcat scan --verbose ...`。调试 ISO/UDF 时可以看到：

```text
Detected UDF
Volume Identifier: XXXXX
Unicode mode: CS0
Entry count: XXXXX
```

## 34. 测试数据

必须建立真实测试集，至少覆盖：

- ISO9660
- Joliet
- UDF
- Chinese filenames
- Japanese filenames
- Korean filenames
- emoji filenames
- long filenames
- mixed ISO/UDF
- non-standard UDF
- Windows-compatible malformed UDF

特别是现实中出现"Windows 挂载正常，但 Catalog 软件出现乱码"的 ISO，这些必须成为 regression tests。

## 35. Unicode Regression Test

例如：

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

## 36. CLI

Phase 1 提供 `offcat`，至少：

- `offcat create`
- `offcat scan`
- `offcat search`
- `offcat info`

示例：

```bash
offcat scan D:\ catalog.db
offcat scan D:\Software catalog.db
offcat scan backup.iso catalog.db
offcat search catalog.db "install.wim"
offcat info catalog.db
```

## 37. GUI

Phase 1 不实现 GUI。未来 GUI 可以：

- Qt 6
- 或者研究将 **VVV** 作为现有开源 GUI 前端

但必须保证 GUI 不参与 Catalog 核心逻辑。最终架构：

```text
                 Catalog Core
                      │
              ┌───────┴───────┐
              │               │
             CLI             GUI
                              │
                         Qt / VVV
```

## 38. VVV / WinCatalog / WhereIsIt 兼容性

项目应该研究 VVV、WinCatalog、WhereIsIt 的数据模型。目标不是复制它们的私有实现，而是找出成熟 Catalog 软件普遍需要表达的数据。

重点研究：Source / Media、Entry、Directory、File、Timestamp、Attributes、Checksum、Container、Virtual Entry、Tag。

## 39. Export / Import

未来设计：

```text
Catalog DB
   │
   ├── VVV exporter
   ├── WinCatalog exporter
   ├── WhereIsIt exporter
   └── JSON exporter
```

Phase 1 只预留架构，不实现第三方格式写入。

特别注意：SQLite 本身并不意味着可以直接兼容 WinCatalog 的 `.w3cat`。WinCatalog 的 SQLite schema 应作为研究资料，而不是直接作为本项目数据库 schema。

## 40. 图片、描述、封面

Phase 1 明确不实现：thumbnail、cover、preview、description、user notes。

这些属于 Digital Asset Management，而不是本项目核心目标。未来如果需要，可以通过 extension table 实现，核心 Catalog 不应该被这些功能污染。

## 41. Tags / Virtual Folders

同样，Tag、Virtual Folder、Bookmark、Comment 在 Phase 1 不实现，但是数据库设计应该避免以后无法添加。

## 42. 插件架构

Provider API 必须从一开始保持独立。未来可以：

- ISO Provider
- ZIP Provider
- 7z Provider
- RAR Provider
- TAR Provider
- VHD Provider
- VHDX Provider
- DMG Provider

Phase 1：ISO/UDF only。不要为了证明架构而实现大量 Provider。

## 43. Provider 安全限制

未来 Archive Provider 必须支持：

- `max_depth`
- `max_entries`
- `max_virtual_size`
- `max_scan_time`

避免：zip bomb、recursive container、path traversal、巨大虚拟目录、恶意容器。

Phase 1 ISO Provider 也应保留这些限制接口。

## 44. 项目目录结构

推荐：

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

## 45. 跨平台要求

目标：Windows、Linux、macOS。

Core 不得依赖：Win32、Qt、GTK、systemd、Linux mount、Windows Mount-DiskImage。

平台相关代码集中到 `src/platform/`。

## 46. 第一阶段完成标准

Phase 1 只有在以下功能全部完成后才算完成：

- [x] CMake build
- [x] C++17
- [x] Windows build
- [x] Linux build
- [x] macOS build
- [x] SQLite Catalog
- [x] Source model
- [x] Directory scanner
- [x] File scanner
- [x] UTF-8 internal representation
- [x] timestamps
- [x] attributes
- [x] FTS5 search
- [x] Container abstraction
- [x] Virtual Entry abstraction
- [x] Container depth
- [x] ISO9660
- [x] Joliet
- [x] UDF
- [x] UDF Unicode
- [x] non-standard UDF compatibility
- [x] optional MD5
- [x] optional SHA-256
- [x] Scan metadata
- [x] cancellation
- [x] error recovery
- [x] million-entry test
- [x] regression tests
- [x] documentation

## 47. 明确禁止 Coding Agent 在 Phase 1 做的事情

Coding agent 不要自行扩大项目范围。禁止主动实现：

- GUI
- ZIP
- 7z
- RAR
- TAR
- VHD
- VHDX
- DMG
- 全文搜索
- 图片缩略图
- 媒体播放器
- 云同步
- 网络数据库
- WhereIsIt CTF writer
- WinCatalog W3CAT writer
- VVV GUI integration

除非明确收到新的任务。

## 48. 开发优先级

严格按照：

1. Catalog schema
2. Database layer
3. Source / Entry model
4. Filesystem scanner
5. Search
6. Container API
7. ISO9660
8. Joliet
9. UDF
10. UDF Unicode compatibility
11. checksum
12. CLI
13. performance tests
14. documentation

不要先做 GUI。

## 49. 最重要的架构原则

1. **Catalog catalogs Sources, not Disks.**
2. **A physical file can also be a Container.**
3. **Container contents are Virtual Entries, not extracted physical files.**
4. **Container parsing must not require mounting or extraction.**
5. **The database model supports nested containers, but scanning depth is configurable and limited.**
6. **ISO9660/Joliet/UDF are first-class Phase 1 functionality.**
7. **Other container formats are extensions, not core functionality.**
8. **UTF-8 is the internal string representation.**
9. **Optional metadata such as checksums must not impose scanning cost when disabled.**
10. **The Catalog database is independent from the GUI and export formats.**

## 50. 给 Coding Agent 的最终指令

Build Phase 1 of an open-source, cross-platform offline media catalog system according to this specification.

- Do not implement a GUI in Phase 1.
- The primary deliverable is a clean Catalog Core consisting of: SQLite database, Source/Entry data model, filesystem scanner, FTS5 search, Container abstraction, Virtual Entry abstraction, ISO9660/Joliet/UDF provider, robust UDF Unicode handling, optional checksum calculation, scan metadata, configurable container depth, CLI, automated tests.
- The architecture must allow future Container Providers and Exporters to be added without changing the core database model.
- Do not implement ZIP/7z/RAR or other archive formats in Phase 1.
- Do not implement GUI, thumbnails, descriptions, media preview, cloud synchronization, or third-party catalog writers in Phase 1.
- Prioritize correctness, Unicode handling, UDF compatibility, database integrity, scalability, cancellation, testability, and clean separation of concerns over feature count.
- In particular, treat real-world non-standard UDF ISO images as an important compatibility requirement. The parser must not assume that every ISO image encountered in the wild strictly follows the ideal standard.

---

## 补充：技术栈

| 项 | 选择 |
|----|------|
| Language | C++17 |
| Build | CMake |
| Database | SQLite 3 |
| Search | SQLite FTS5 |
| Testing | GoogleTest |
| CLI | C++17 |
| GUI | Not implemented in Phase 1 |

最终架构：

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
