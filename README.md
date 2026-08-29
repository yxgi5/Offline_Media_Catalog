# Offcat — Offline Media Catalog

开源、跨平台的离线存储 Catalog 软件。

即使硬盘已经拔掉，仍然可以搜索离线存储的内容：文件名、目录结构、ISO 内部文件、校验码。

## 功能

- SQLite Catalog（WAL、FTS5、UTF-8）
- Source 模型：physical_disk / volume / directory / file / iso
- 跨平台文件系统扫描（Windows / Linux / macOS）
- FTS5 文件名与路径搜索
- Container 抽象：Discovery 与 Expansion 分离
- ISO Provider：ISO9660 / Joliet / UDF（直接解析，不依赖系统挂载）
- OSTA Compressed Unicode 解析（含非标准 UDF 兼容性恢复）
- 可选校验码：SHA-256 / MD5 / CRC32
- 扫描取消（Ctrl+C）与错误恢复
- CLI：create / scan / search / info

## 构建

```bash
cmake -B build
cmake --build build -j
```

测试：

```bash
ctest --test-dir build
```

依赖通过 CMake FetchContent 自动下载（SQLite3 amalgamation、GoogleTest）。

## 用法

```bash
offcat create catalog.db
offcat scan --containers --sha256 --crc32 /mnt/archive catalog.db
offcat search catalog.db install.wim
offcat info catalog.db
```

## 文档

- [架构](docs/architecture.md)
- [数据库 Schema](docs/catalog-schema.md)
- [Provider API](docs/provider-api.md)
- [ISO/UDF 兼容性](docs/compatibility.md)

## License

GPL-3.0-or-later
