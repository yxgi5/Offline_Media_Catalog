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
- Rock Ridge 属性支持（SUSP/RRIP）：NM 长名、PX 权限、TF 时间戳、SL 符号链接、CE 续区
- rr_moved 深目录还原（超过 8 层目录的 Rock Ridge 镜像正确恢复目录树）
- 容器目录递归展开，深度可配置（`--depth`，单文件 ISO 源同样支持）
- OSTA Compressed Unicode 解析（含非标准 UDF 兼容性恢复）
- 可选校验码：SHA-256 / MD5 / CRC32
- 扫描取消（Ctrl+C）与错误恢复
- CLI：create / scan / search / info / serve（只读 Web 查看器，前端可插拔）

## 构建

### 依赖

- CMake ≥ 3.16
- C++17 编译器：GCC / MinGW-w64 ≥ 9、MSVC ≥ 2019、Clang ≥ 10
- SQLite3 amalgamation 与 GoogleTest 通过 CMake FetchContent 自动下载；
  也可放入 `third_party/` 实现离线构建（`third_party/sqlite-amalgamation-3460100/`、
  `third_party/googletest/`）

> 平台状态：Windows（MinGW-w64）已实测通过；Linux / macOS 代码已做平台隔离
> （平台逻辑集中在 `src/platform/`、无 Win32 依赖），但尚未经 CI 验证。
> 详见 [架构文档 - 已知限制](docs/architecture.md)。

### 步骤

```bash
# 配置
cmake -B build

# 编译（默认同时构建 CLI 与测试）
cmake --build build -j

# 运行测试
ctest --test-dir build
```

产物：

| 目标 | 路径 |
|------|------|
| CLI | `build/offcat`（Windows 为 `build/offcat.exe`） |
| 测试 | `build/offcat_tests`（Windows 为 `build/offcat_tests.exe`） |

常用配置：

```bash
# 不构建测试
cmake -B build -DOFFCAT_BUILD_TESTS=OFF

# 使用 OpenSSL 计算 SHA-256/MD5（默认使用内置实现）
cmake -B build -DOFFCAT_USE_OPENSSL=ON
```

## 用法

所有命令操作 SQLite Catalog 文件（`.db`）。典型流程：创建 → 扫描 → 搜索。

### 1. 创建 Catalog

```bash
offcat create catalog.db
```

### 2. 扫描

```bash
# 扫描目录（不含 ISO 容器）
offcat scan /mnt/archive catalog.db

# 扫描目录并展开其中的 ISO 容器（默认展开 1 层）
offcat scan --containers /mnt/archive catalog.db

# 展开容器并递归到更深层目录（depth=2：ISO 内目录的下一层）
offcat scan --containers --depth 2 /mnt/archive catalog.db

# 计算校验码（单参数入口，逗号分隔复合）
offcat scan --containers --checksum crc32 /mnt/archive catalog.db
offcat scan --containers --checksum sha256,crc32 /mnt/archive catalog.db
offcat scan --containers --checksum all /mnt/archive catalog.db   # 三种全部

# 直接扫描单个 ISO 文件（也会展开容器内部，无需目录包裹）
offcat scan --containers --depth 2 movie.iso catalog.db
```

扫描选项：

| 选项 | 说明 |
|------|------|
| `--containers` | 扫描并展开 ISO 容器（默认不展开） |
| `--depth <N>` | 容器内目录最大递归深度（默认 1） |
| `--checksum <spec>` | 校验算法：逗号分隔列表（`sha256`/`md5`/`crc32`）、`all`（全部）或 `none`（不计算）；可重复出现并自动去重；裸 `--checksum` = all |
| `--verbose` / `--debug` / `--quiet` | 日志级别 |

扫描默认记录每个条目的修改时间与创建时间（创建时间在平台无法
提供时保持为空，如 Linux 无 statx 的情况）。

扫描支持 Ctrl+C 取消：已扫描数据保留，Catalog 标记为 cancelled。

### 3. 搜索

```bash
offcat search catalog.db install.wim
```

搜索结果包含 Source 名、完整路径；容器内的虚拟条目会标注
`[container: iso] [virtual]`。ISO 内 Rock Ridge 符号链接也作为独立条目
（类型 symlink）参与搜索。

### 4. 查看统计

```bash
offcat info catalog.db
```

显示 Sources / Entries / Containers 数量，以及每个 Source 的条目数。

### 5. 启动 Web 查看器（serve）

以只读方式启动本机 Web 服务，在浏览器中浏览目录树、展开 ISO 容器、
FTS 搜索：

```bash
# 默认端口 8080，浏览器打开 http://127.0.0.1:8080/
offcat serve catalog.db

# 指定端口
offcat serve --port 8090 catalog.db

# 使用外部前端（替换内置页面）
offcat serve --web-root ./my_frontend catalog.db
```

| 选项 | 说明 |
|------|------|
| `--port <N>` | 监听端口（默认 8080），仅绑定 127.0.0.1 |
| `--web-root <dir>` | 前端资源目录；提供后任意静态站点（HTML/CSS/JS/图片）被托管，`index.html` 优先于内置版本 |

- 数据库以**只读模式**打开，任何接口都不能修改数据；Ctrl+C 停止服务。
- 内置前端为单文件、零外部依赖；外部前端与核心只通过 JSON API 交互，
  契约见 [Web API](docs/web-api.md)。

### Windows 注意事项

- 扫描与校验码计算使用流式读取，无需管理员权限，不挂载 ISO。
- 路径含空格时用引号包裹：`offcat scan "D:\My Archive" catalog.db`。
- 扫描期间 Catalog 使用 WAL 模式；请勿并发复制/删除 `.db-wal` 文件，
  以免丢失未 checkpoint 的数据。

## 文档

- [项目规格（Spec）](docs/spec.md)
- [架构](docs/architecture.md)
- [数据库 Schema](docs/catalog-schema.md)
- [Provider API](docs/provider-api.md)
- [Web API（前端对接契约）](docs/web-api.md)
- [ISO/UDF 兼容性](docs/compatibility.md)

## License

GPL-3.0-or-later
