# Container Provider API

Provider 是容器格式的解析插件。Phase 1 只实现 ISO Provider。

## 接口

```cpp
class ContainerProvider {
public:
    virtual ~ContainerProvider() = default;
    virtual std::string type() const = 0;
    virtual bool probe(const std::string& filepath) = 0;
    virtual bool scan(int64_t container_entry_id,
                      Database& db,
                      const ContainerOptions& options) = 0;
};
```

- `type()`：Provider 标识（"iso"、"zip" ...）
- `probe()`：检测文件是否属于该格式（Discovery）
- `scan()`：解析容器内容并写入 Virtual Entry（Expansion）

`scan()` 契约（ISO Provider 已落实）：

- 一次 `scan()` 调用完成整个容器树的展开，无需外层多次调用
- 按 `options.max_depth` 递归展开目录（ISO9660/Joliet/UDF 统一）
- Rock Ridge 镜像的 rr_moved 占位符还原在 Provider 内部完成，
  外层看到的始终是真实目录树
- 无法解析的子目录/条目记录 warning，不中断整树

Provider 不负责 SQLite、GUI、Search、扫描调度、Catalog 管理。
Provider 只负责解析自己的格式并生成 Virtual Entries。

## Registry

```cpp
ProviderRegistry::instance()
    .register_provider(std::make_shared<IsoProvider>());
```

- `find_provider(type)`：按类型查找
- `probe_file(path)`：依次 probe，返回第一个匹配的 Provider

目录扫描中发现的 ISO 文件与直接扫描的单文件源（scan_file_entry）
都通过 Registry 展开，Provider 无需感知来源差异。

## Virtual Tree Writer

```cpp
class VirtualTreeWriter {
public:
    Result<int64_t> add_entry(const EntryData& entry);
};
```

写入的 Entry 自动标记 `is_virtual = 1`，并同步 FTS5 索引。

## 安全限制

```cpp
struct ContainerOptions {
    int max_depth = 1;            // 最大嵌套深度
    int current_depth = 0;        // 当前嵌套深度（Scanner 传入）
    int max_entries = 1000000;    // 最大虚拟条目数
    int64_t max_virtual_size = 0; // 最大虚拟大小（0 = 不限）
    int max_scan_time_seconds = 0;// 最大扫描时间（0 = 不限）
};
```

已实现：

- `max_depth`：ISO Provider 按此值递归展开容器内目录树；
  CLI `--depth` → ScanOptions.max_container_depth → 此处
- `current_depth`：Scanner 展开入口处设为 1（当前 ISO 内部深度语义）

未实现（`max_entries` / `max_virtual_size` / `max_scan_time_seconds`）：
字段已预留，ISO Provider 暂不限制；UDF 解析器内部有深度上限 64
防止损坏镜像无限递归。未来 Archive Provider（ZIP/7z/RAR）必须落实
全部限制，防止 zip bomb、递归容器、路径穿越。

## 新增 Provider 步骤

1. 实现 `ContainerProvider` 接口（放在 providers/<format>/）
2. 在 CLI 启动时调用注册函数
3. 无需修改数据库 Schema 或 Scanner 核心
