# Catalog Schema

数据库格式：SQLite 3（WAL 模式、FTS5、UTF-8）。

## source

| 列 | 类型 | 说明 |
|----|------|------|
| id | INTEGER PK | |
| name | TEXT NOT NULL | 用户看到的 Source 名称 |
| type | TEXT NOT NULL | physical_disk / volume / directory / file / iso / other |
| source_path | TEXT | 扫描时的路径（仅信息，非身份） |
| label | TEXT | Volume label |
| serial | TEXT | Volume/device serial |
| filesystem | TEXT | NTFS、ext4、UDF 等 |
| size | INTEGER | Source 容量 |
| created_at | INTEGER | 创建时间（如可获得） |
| cataloged_at | INTEGER | 本次 Catalog 时间 |

## entry

| 列 | 类型 | 说明 |
|----|------|------|
| id | INTEGER PK | |
| source_id | INTEGER FK | 所属 Source |
| parent_id | INTEGER FK | 父 Entry（NULL = 根） |
| name | TEXT NOT NULL | UTF-8 名称 |
| type | INTEGER | 1=file 2=directory 3=symlink 4=other |
| size | INTEGER | 文件大小 |
| mtime/ctime/atime/birthtime | INTEGER | 时间戳，允许 NULL |
| mode | INTEGER | 权限（Unix） |
| attributes | INTEGER | 属性（Windows） |
| is_virtual | INTEGER | 1 = Virtual Entry（容器内部） |

树结构使用 `parent_id + name`，不存储完整路径。路径通过递归查询构造。

## container

| 列 | 类型 | 说明 |
|----|------|------|
| id | INTEGER PK | |
| entry_id | INTEGER FK | 对应物理 Entry |
| type | TEXT NOT NULL | iso 等 |
| provider | TEXT | 解析 Provider 名称 |
| version | TEXT | Provider 版本 |

## checksum

| 列 | 类型 | 说明 |
|----|------|------|
| entry_id | INTEGER FK | |
| algorithm | TEXT | sha256 / md5 / crc32 |
| value | BLOB | 校验值（CRC32 为大端序 4 字节） |
| calculated_at | INTEGER | 计算时间 |

复合主键 (entry_id, algorithm)。

## scan

| 列 | 类型 | 说明 |
|----|------|------|
| id | INTEGER PK | |
| source_id | INTEGER FK | |
| started_at | INTEGER | |
| finished_at | INTEGER | |
| scanner_version | TEXT | |
| options | TEXT | JSON（checksum、containers、max_container_depth） |
| status | INTEGER | 0=in_progress 1=completed 2=cancelled 3=failed |

## entry_fts（FTS5 虚拟表）

```
entry_fts(name, path, source_name)
```

独立内容表（`content=''`），由应用显式维护，支持：
- 文件名搜索（name MATCH）
- 路径搜索（path MATCH）
- 按 Source 过滤（source_name MATCH）

## 索引

- idx_entry_source (source_id)
- idx_entry_parent (parent_id)
- idx_entry_source_parent (source_id, parent_id)
- idx_container_entry (entry_id)
- idx_checksum_entry (entry_id)
- idx_scan_source (source_id)

## 预留字段（未实现）

以下字段在 spec 中有要求或预留，但当前版本**未实现**，加表/加列不会
破坏既有数据（通过 ALTER TABLE ADD COLUMN 或新扩展表引入）：

| 归属 | 字段 | spec 来源 | 用途 |
|------|------|-----------|------|
| entry | name_encoding | §21 | UDF 文件名解析方式（udf-cs0 / compatibility / heuristic） |
| entry | name_confidence | §21 | 解析置信度（100 / 80 / 40） |
| container | available_filesystems | §22 | 镜像内检测到的全部文件系统（UDF/Joliet/ISO9660 等） |
| 独立表/列 | fingerprint | §24 | 轻量指纹（size + fast fingerprint + optional SHA-256 多级验证） |

设计约束：这些字段必须允许 NULL；不得作为 Entry 身份的一部分。
