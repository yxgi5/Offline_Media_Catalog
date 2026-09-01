# Offcat — Offline Media Catalog

开源、跨平台的离线存储 Catalog 软件。

Open-source, cross-platform offline media catalog software.

即使硬盘已经拔掉，仍然可以搜索离线存储的内容：文件名、目录结构、ISO 内部文件、校验码。

Even after a disk is unplugged, you can still search its offline content: file names, directory structures, ISO inner files, and checksums.

## 项目背景 / Project Background

市面上已有的离线存储索引工具，在使用中各有明显短板：

Existing offline-storage indexing tools all have obvious shortcomings in practice:

- **whereist**：对 FUSE 挂载盘（如 securefs 加密盘）支持不完善，扫描这类存储经常失败或漏扫；
  **whereist**: weak support for FUSE-mounted disks (e.g. securefs encrypted drives); scans often fail or miss files.
- **wincatlog**：ISO 解析能力太差，对非标准镜像常解析错误，中文文件名频繁乱码，目录结构也容易错乱。
  **wincatlog**: poor ISO parsing — non-standard images are often mis-parsed, Chinese file names frequently show as mojibake, and directory trees get corrupted.

我们的需求是：既能可靠扫描 FUSE 挂载的存储，又能直接解析 ISO 镜像（含非标准 UDF），同时保证中文路径不乱码——现有工具都无法满足，于是产生了 Offcat 这个项目。

We needed a tool that could reliably scan FUSE-mounted storage, parse ISO images directly (including non-standard UDF), and keep Chinese paths intact — none of the existing tools could do this, so Offcat was born.

## 功能 / Features

- SQLite Catalog（WAL、FTS5、UTF-8）
  SQLite catalog (WAL, FTS5, UTF-8).
- Source 模型：physical_disk / volume / directory / file / iso
  Source model: physical_disk / volume / directory / file / iso.
- 跨平台文件系统扫描（Windows / Linux / macOS）
  Cross-platform filesystem scanning (Windows / Linux / macOS).
- FTS5 文件名与路径搜索
  FTS5 file-name and path search.
- Container 抽象：Discovery 与 Expansion 分离
  Container abstraction: discovery and expansion are separated.
- ISO Provider：ISO9660 / Joliet / UDF（直接解析，不依赖系统挂载）
  ISO provider: ISO9660 / Joliet / UDF, parsed directly without relying on system mounting.
- Rock Ridge 属性支持（SUSP/RRIP）：NM 长名、PX 权限、TF 时间戳、SL 符号链接、CE 续区
  Rock Ridge support (SUSP/RRIP): NM long names, PX permissions, TF timestamps, SL symlinks, CE continuation areas.
- rr_moved 深目录还原（超过 8 层目录的 Rock Ridge 镜像正确恢复目录树）
  rr_moved deep-directory restoration (correctly restores trees beyond 8 levels on Rock Ridge images).
- 容器目录树完整展开，嵌套容器层数可配置（`--depth`，单文件 ISO 源同样支持）
  Full container tree expansion with configurable nesting depth (`--depth`; single-file ISO sources are also supported).
- OSTA Compressed Unicode 解析（含非标准 UDF 兼容性恢复）
  OSTA Compressed Unicode parsing, with recovery for non-standard UDF layouts.
- 可选校验码：SHA-256 / MD5 / CRC32
  Optional checksums: SHA-256 / MD5 / CRC32.
- 扫描取消（Ctrl+C）与错误恢复
  Scan cancellation (Ctrl+C) and error recovery.
- CLI：create / scan / search / info / serve（只读 Web 查看器，前端可插拔）
  CLI: create / scan / search / info / serve (read-only web viewer with pluggable frontend).

## 构建 / Building

### 依赖 / Dependencies

- CMake ≥ 3.16
- C++17 编译器：GCC / MinGW-w64 ≥ 9、MSVC ≥ 2019、Clang ≥ 10
  C++17 compiler: GCC / MinGW-w64 ≥ 9, MSVC ≥ 2019, Clang ≥ 10.
- SQLite3 amalgamation 与 GoogleTest 的 zip 包均随仓库维护在 `third_party/`（
  `third_party/sqlite-amalgamation-3460100.zip`、`third_party/googletest-1.14.0.zip`），
  通过 CMake FetchContent 解压使用，无需联网即可构建；也可手动解压到
  `third_party/` 下对应的源码目录跳过解压步骤
  The SQLite3 amalgamation and GoogleTest zips are maintained in `third_party/`
  (`third_party/sqlite-amalgamation-3460100.zip`, `third_party/googletest-1.14.0.zip`)
  and extracted via CMake FetchContent, so building works fully offline; you may
  also extract them into `third_party/` yourself to skip the extraction step.

> 平台状态：Windows（MinGW-w64）已实测通过；Linux / macOS 代码已做平台隔离
> （平台逻辑集中在 `src/platform/`、无 Win32 依赖），但尚未经 CI 验证。
> 详见 [架构文档 - 已知限制](docs/architecture.md)。
>
> Platform status: Windows (MinGW-w64) is tested and working; Linux / macOS code
> is platform-isolated (platform logic lives in `src/platform/`, no Win32
> dependencies) but not yet verified by CI. See the
> [architecture doc - known limitations](docs/architecture.md).

### 步骤 / Steps

```bash
# 配置
# Configure
cmake -B build

# 编译（默认同时构建 CLI 与测试）
# Build (CLI and tests by default)
cmake --build build -j

# 运行测试
# Run tests
ctest --test-dir build
```

产物 / Artifacts:

| 目标 / Target | 路径 / Path |
|------|------|
| CLI | `build/offcat`（Windows 为 `build/offcat.exe`）/ (`build/offcat.exe` on Windows) |
| 测试 / Tests | `build/offcat_tests`（Windows 为 `build/offcat_tests.exe`）/ (`build/offcat_tests.exe` on Windows) |

常用配置 / Common options:

```bash
# 不构建测试
# Skip tests
cmake -B build -DOFFCAT_BUILD_TESTS=OFF

# 使用 OpenSSL 计算 SHA-256/MD5（默认使用内置实现）
# Use OpenSSL for SHA-256/MD5 (built-in implementation is the default)
cmake -B build -DOFFCAT_USE_OPENSSL=ON
```

## 用法 / Usage

所有命令操作 SQLite Catalog 文件（`.db`）。典型流程：创建 → 扫描 → 搜索。

All commands operate on a SQLite catalog file (`.db`). Typical flow: create → scan → search.

查看版本号 / Show the version:

```bash
offcat --version   # 或 / or: offcat -V
```

### 1. 创建 Catalog / Create a Catalog

```bash
offcat create catalog.db
```

### 2. 扫描 / Scanning

```bash
# 扫描目录（ISO 容器默认只识别、不展开）
# Scan a directory (ISO containers are discovered but not expanded by default)
offcat scan /mnt/archive catalog.db

# 展开一层容器（内部目录树完整收录）
# Expand one level of ISO containers (full inner tree)
offcat scan --depth 1 /mnt/archive catalog.db

# 递归展开内嵌容器（depth=2：一层 + 二层容器都展开）
# Expand containers recursively (depth=2: first- and second-level containers)
offcat scan --depth 2 /mnt/archive catalog.db

# 计算校验码（单参数入口，逗号分隔复合）
# Compute checksums (single-flag entry, comma-separated combos)
offcat scan --depth 1 --checksum crc32 /mnt/archive catalog.db
offcat scan --depth 1 --checksum sha256,crc32 /mnt/archive catalog.db
offcat scan --depth 1 --checksum all /mnt/archive catalog.db   # 三种全部 / all three

# 直接扫描单个 ISO 文件（也会展开容器内部，无需目录包裹）
# Scan a single ISO file directly (containers are expanded too, no parent dir needed)
offcat scan --depth 2 movie.iso catalog.db
```

扫描选项 / Scan options:

| 选项 / Option | 说明 / Description |
|------|------|
| `--depth <N>` | 容器展开深度（0 = 仅识别容器、不展开，默认；1 = 展开一层容器且其内部目录树完整收录；2+ = 同时展开内嵌容器）/ container expansion depth (0 = discover containers only, no expansion (default); 1 = one level with its full inner tree; 2+ = also expand nested containers) |
| `--checksum <spec>` | 校验算法：逗号分隔列表（`sha256`/`md5`/`crc32`）、`all`（全部）或 `none`（不计算）；可重复出现并自动去重；裸 `--checksum` = all / algorithms: comma-separated list (`sha256`/`md5`/`crc32`), `all`, or `none`; repeatable and auto-deduped; bare `--checksum` = all |
| `--progress <on\|off>` | 扫描进度输出（实时显示当前扫描路径与已扫文件/目录计数；交互终端每秒刷新同一行，输出重定向时每 5 秒一行；默认开启，裸 `--progress` = on，`--progress off` 关闭；`--quiet` 自动关闭）/ live scan progress (current path plus files/dirs counts; interactive terminals redraw one line per second, redirected output appends a line every 5 s; on by default, bare `--progress` = on, `--progress off` disables; `--quiet` also disables) |
| `--quiet` / `--verbose` / `--debug` | 日志级别，由少到多：Quiet < Normal < Verbose < Debug，默认 Normal。`--quiet`：仅警告/错误（同时关闭进度）；`--verbose`：增加逐目录、容器与条目细节；`--debug`：增加内部诊断（检查点、解析过程）/ log levels, increasing detail: Quiet < Normal < Verbose < Debug, default Normal. `--quiet`: warnings/errors only (also disables progress); `--verbose`: adds per-directory, container and entry details; `--debug`: adds internal diagnostics (checkpoints, parser internals) |

扫描默认记录每个条目的修改时间与创建时间（创建时间在平台无法
提供时保持为空，如 Linux 无 statx 的情况）。

Scans record each entry's modification time and creation time by default
(creation time stays empty when the platform cannot provide it, e.g. Linux
without statx).

扫描支持 Ctrl+C 取消：已扫描数据保留，Catalog 标记为 cancelled。

Scans can be cancelled with Ctrl+C: data scanned so far is kept and the
catalog is marked as cancelled.

### 3. 搜索 / Searching

```bash
offcat search catalog.db install.wim
```

搜索结果包含 Source 名、完整路径；容器内的虚拟条目会标注
`[container: iso] [virtual]`。ISO 内 Rock Ridge 符号链接也作为独立条目
（类型 symlink）参与搜索。

Results include the source name and full path; virtual entries inside
containers are tagged `[container: iso] [virtual]`. Rock Ridge symlinks inside
ISOs are indexed as standalone entries (type symlink) and participate in search.

### 4. 查看统计 / Statistics

```bash
offcat info catalog.db
```

显示 Sources / Entries / Containers 数量，以及每个 Source 的条目数。

Shows the number of sources, entries and containers, plus the entry count of each source.

### 5. 启动 Web 查看器（serve）/ Web Viewer (serve)

以只读方式启动本机 Web 服务，在浏览器中浏览目录树、展开 ISO 容器、
FTS 搜索：

Starts a read-only local web service to browse the directory tree, expand ISO
containers and run FTS searches in a browser:

```bash
# 默认端口 8080，浏览器打开 http://127.0.0.1:8080/
# Default port 8080; open http://127.0.0.1:8080/ in a browser
offcat serve catalog.db

# 指定端口
# Custom port
offcat serve --port 8090 catalog.db

# 使用外部前端（替换内置页面）
# Use an external frontend (replaces the built-in pages)
offcat serve --web-root ./my_frontend catalog.db
```

| 选项 / Option | 说明 / Description |
|------|------|
| `--port <N>` | 监听端口（默认 8080），仅绑定 127.0.0.1 / listening port (default 8080), bound to 127.0.0.1 only |
| `--web-root <dir>` | 前端资源目录；提供后任意静态站点（HTML/CSS/JS/图片）被托管，`index.html` 优先于内置版本 / frontend asset directory; when provided, any static site (HTML/CSS/JS/images) is served and its `index.html` takes precedence over the built-in one |

- 数据库以**只读模式**打开，任何接口都不能修改数据；Ctrl+C 停止服务。
  The database is opened in **read-only mode**; no endpoint can modify data; Ctrl+C stops the server.
- 内置前端为单文件、零外部依赖；外部前端与核心只通过 JSON API 交互，
  契约见 [Web API](docs/web-api.md)。
  The built-in frontend is a single file with zero external dependencies; an
  external frontend talks to the core only through the JSON API, whose contract
  is in [Web API](docs/web-api.md).

### Windows 注意事项 / Windows Notes

- 扫描与校验码计算使用流式读取，无需管理员权限，不挂载 ISO。
  Scanning and checksums use streaming reads: no admin rights needed, ISOs are never mounted.
- 路径含空格时用引号包裹：`offcat scan "D:\My Archive" catalog.db`。
  Quote paths containing spaces: `offcat scan "D:\My Archive" catalog.db`.
- 扫描期间 Catalog 使用 WAL 模式；请勿并发复制/删除 `.db-wal` 文件，
  以免丢失未 checkpoint 的数据。
  The catalog uses WAL mode during scans; do not copy/delete `.db-wal` files
  concurrently or un-checkpointed data may be lost.

## 文档 / Documentation

- [项目规格（Spec）/ Project Spec](docs/spec.md)
- [架构 / Architecture](docs/architecture.md)
- [数据库 Schema / Database Schema](docs/catalog-schema.md)
- [Provider API](docs/provider-api.md)
- [Web API（前端对接契约）/ Web API (frontend contract)](docs/web-api.md)
- [ISO/UDF 兼容性 / ISO/UDF Compatibility](docs/compatibility.md)

## License

GPL-3.0-or-later
