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

Provider 不负责 SQLite、GUI、Search、扫描调度、Catalog 管理。
Provider 只负责解析自己的格式并生成 Virtual Entries。

## Registry

```cpp
ProviderRegistry::instance()
    .register_provider(std::make_shared<IsoProvider>());
```

- `find_provider(type)`：按类型查找
- `probe_file(path)`：依次 probe，返回第一个匹配的 Provider

## Virtual Tree Writer

```cpp
class VirtualTreeWriter {
public:
    Result<int64_t> add_entry(const EntryData& entry);
};
```

写入的 Entry 自动标记 `is_virtual = 1`，并同步 FTS5 索引。

## 安全限制（Phase 1 预留）

```cpp
struct ContainerOptions {
    int max_depth = 1;            // 最大嵌套深度
    int max_entries = 1000000;    // 最大虚拟条目数
    int64_t max_virtual_size = 0; // 最大虚拟大小（0 = 不限）
    int max_scan_time_seconds = 0;// 最大扫描时间（0 = 不限）
};
```

未来 Archive Provider（ZIP/7z/RAR）必须落实这些限制，防止 zip bomb、
递归容器、路径穿越。

## 新增 Provider 步骤

1. 实现 `ContainerProvider` 接口（放在 providers/<format>/）
2. 在 CLI 启动时调用注册函数
3. 无需修改数据库 Schema 或 Scanner 核心
