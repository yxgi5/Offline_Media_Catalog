# Container Provider API / 容器 Provider API

Provider 是容器格式的解析插件。Phase 1 只实现 ISO Provider。

A provider is a parsing plugin for a container format. Phase 1 only ships the ISO provider.

## 接口 / Interface

```cpp
class ContainerProvider {
public:
    virtual ~ContainerProvider() = default;
    virtual std::string type() const = 0;
    virtual bool scan(int64_t container_entry_id,
                      Database& db,
                      const ContainerOptions& options) = 0;
};
```

- `type()`：Provider 标识（"iso"、"zip" ...）/ provider identifier ("iso", "zip", ...)
- `scan()`：解析容器内容并写入 Virtual Entry（Expansion）/ parses container contents and writes virtual entries (expansion)

`scan()` 契约（ISO Provider 已落实）/ The `scan()` contract (as implemented by the ISO provider):

- 一次 `scan()` 调用完成整个容器树的展开，无需外层多次调用
  A single `scan()` call expands the whole container tree; no repeated outer calls needed.
- 容器内部目录树始终完整展开（仅受防死循环上限约束）；
  `options.max_depth` 限制的是容器*嵌套*层数（ISO 内的 ISO），
  嵌套镜像提取到临时文件后递归调用 `scan_udf`/`scan_iso9660`
  The inner directory tree is always fully expanded (bounded only by the
  anti-loop limit); `options.max_depth` limits the container *nesting* level
  (ISOs inside ISOs) — nested images are extracted to temporary files and
  re-scanned via `scan_udf`/`scan_iso9660`.
- Rock Ridge 镜像的 rr_moved 占位符还原在 Provider 内部完成，
  外层看到的始终是真实目录树
  rr_moved placeholder restoration for Rock Ridge images happens inside the
  provider; the outside always sees the real directory tree.
- 无法解析的子目录/条目记录 warning，不中断整树
  Unparseable subdirectories/entries log a warning without interrupting the tree.

Provider 不负责 SQLite、GUI、Search、扫描调度、Catalog 管理。
Provider 只负责解析自己的格式并生成 Virtual Entries。

A provider is not responsible for SQLite, the GUI, search, scan scheduling or
catalog management. It only parses its own format and produces virtual entries.

## Registry / 注册表

```cpp
ProviderRegistry::instance()
    .register_provider(std::make_shared<IsoProvider>());
```

- `find_provider(type)`：按类型查找 / look up by type

容器发现（Discovery）由扫描器按扩展名完成（`.iso` / `.img`），
再经 `find_provider("iso")` 解析为具体 Provider；Provider 不参与发现。

Container discovery is done by the scanner via extension (`.iso` / `.img`),
which then resolves the provider via `find_provider("iso")`; providers do
not take part in discovery.

目录扫描中发现的 ISO 文件与直接扫描的单文件源（scan_file_entry）
都通过 Registry 展开，Provider 无需感知来源差异。

ISO files found during directory scans and single-file sources scanned
directly (scan_file_entry) are both expanded through the registry; providers
do not need to know where the file came from.

## Virtual Tree Writer / 虚拟树写入器

```cpp
class VirtualTreeWriter {
public:
    Result<int64_t> add_entry(const EntryData& entry);
};
```

写入的 Entry 自动标记 `is_virtual = 1`，并同步 FTS5 索引。

Written entries are automatically marked `is_virtual = 1` and kept in sync with the FTS5 index.

## 安全限制 / Safety Limits

```cpp
struct ContainerOptions {
    int max_depth = 1;            // 最大嵌套深度 / max nesting depth
    int current_depth = 0;        // 当前嵌套深度（Scanner 传入）/ current nesting depth (set by the scanner)
    int max_entries = 1000000;    // 最大虚拟条目数 / max virtual entries
    int64_t max_virtual_size = 0; // 最大虚拟大小（0 = 不限）/ max virtual size (0 = unlimited)
    int max_scan_time_seconds = 0;// 最大扫描时间（0 = 不限）/ max scan time in seconds (0 = unlimited)
};
```

已实现 / Implemented:

- `max_depth`：容器嵌套层数。ISO Provider 在 walk 中发现
  `.iso`/`.img`/`.udf` 文件且 `current_depth < max_depth` 时，把
  该文件提取到临时文件并递归展开；目录树本身不受此值限制。
  CLI `--depth` → ScanOptions.max_container_depth → 此处
  Container nesting level. When the ISO provider's walk finds
  `.iso`/`.img`/`.udf` files and `current_depth < max_depth`, it extracts the
  file to a temporary location and expands it recursively; the directory tree
  itself is not limited by this value. CLI `--depth` →
  ScanOptions.max_container_depth → here.
- `current_depth`：Scanner 展开入口处设为 1；嵌套展开时逐层 +1
  Set to 1 at the scanner's expansion entry point; incremented per nesting level.

未实现（`max_entries` / `max_virtual_size` / `max_scan_time_seconds`）：
字段已预留，ISO Provider 暂不限制；UDF 解析器内部有深度上限 64
防止损坏镜像无限递归。未来 Archive Provider（ZIP/7z/RAR）必须落实
全部限制，防止 zip bomb、递归容器、路径穿越。

Not implemented (`max_entries` / `max_virtual_size` / `max_scan_time_seconds`):
the fields are reserved and the ISO provider does not enforce them yet; the
UDF parser has an internal depth cap of 64 against infinite recursion on
corrupt images. Future archive providers (ZIP/7z/RAR) must enforce all limits
to prevent zip bombs, recursive containers and path traversal.

## 新增 Provider 步骤 / Adding a New Provider

1. 实现 `ContainerProvider` 接口（放在 providers/<format>/）
   Implement the `ContainerProvider` interface (under providers/<format>/).
2. 在 CLI 启动时调用注册函数
   Call the registration function at CLI startup.
3. 无需修改数据库 Schema 或 Scanner 核心
   No changes to the database schema or the scanner core are needed.
