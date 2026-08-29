# Architecture

```
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

## 模块划分

| 模块 | 目录 | 职责 |
|------|------|------|
| core | src/core/ | 通用类型、Result、Logger、Checksum 引擎 |
| catalog | src/catalog/ | Source/Entry/Container/Checksum/Scan 管理 |
| database | src/database/ | SQLite 封装、Schema、事务、Statement |
| scanner | src/scanner/ | 文件系统扫描、取消、搜索 |
| container | src/container/ | Provider 接口、Registry、VirtualTreeWriter |
| filesystem | src/filesystem/ | 跨平台文件系统操作（预留） |
| platform | src/platform/ | 平台相关代码（预留） |
| providers/iso | providers/iso/ | ISO9660 / Joliet / UDF 解析 |

## 核心原则

1. Catalog 编目 Source，而不是 Disk
2. 物理文件可以同时是 Container
3. Container 内容是 Virtual Entry，不是解压的物理文件
4. Container 解析不需要挂载或解压
5. 数据库支持嵌套容器，扫描深度可配置且有限
6. ISO9660/Joliet/UDF 是 Phase 1 一等公民
7. 其他容器格式是扩展，非核心功能
8. UTF-8 是内部字符串表示
9. 可选元数据（校验码）禁用时零开销
10. 数据库独立于 GUI 和导出格式

## 扫描流程

```
scan_source(path, options)
  → 创建 Source 记录
  → 创建 Scan 记录
  → 递归遍历目录（std::filesystem）
      → 每个 Entry 读取元数据并批量插入（每 1000 条 checkpoint）
      → 同步更新 FTS5 索引
      → 启用 --containers 时：ISO 文件 → Container 记录 → Provider 展开
      → 启用 checksum 时：流式计算 SHA-256/MD5/CRC32
  → 提交事务
  → Scan 状态标记 completed / cancelled
```

## ISO Provider 流程

```
probe(filepath)
  → UDF parser（NSR02/NSR03 检测）
  → ISO9660 parser（CD001 检测）

scan(container_entry_id, db, options)
  → 优先 UDF（最完整）
  → 其次 Joliet（Unicode 文件名）
  → 最后 ISO9660
```

## 错误处理

- 单文件读取失败 → 记录 warning，继续扫描
- Container 损坏 → 记录 error，继续扫描其他文件
- 取消 → 停止后续扫描，事务正确结束，已完成数据保留
