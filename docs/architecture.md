# Architecture / 架构

```
                offcat.exe（单一入口）
                    │
        ┌───────────┴───────────┐
        │                       │
   Catalog Core                CLI（create / scan / search / info / serve）
   (catalog / scanner /         │
    database / container)   liboffcat_server（serve 子命令）
        │                       ├─ HTTP + Viewer（JSON API）
    ┌───┼───────┐               └─ 前端（可插拔）
    │   │       │                   ├─ 内置 web/index.html（嵌入二进制）
 SQLite Scanner Container          └─ 外部 --web-root <dir>（任意静态站点）
                 │
              ISO/UDF
```

## 模块划分 / Module Layout

| 模块 / Module | 目录 / Directory | 职责 / Responsibility |
|------|------|------|
| core | src/core/ | 通用类型、Result、Logger、Checksum 引擎 / common types, Result, Logger, checksum engine |
| catalog | src/catalog/ | Source/Entry/Container/Checksum/Scan 管理 / source, entry, container, checksum and scan management |
| database | src/database/ | SQLite 封装、Schema、事务、Statement / SQLite wrapper, schema, transactions, statements |
| scanner | src/scanner/ | 文件系统扫描、取消、搜索 / filesystem scanning, cancellation, search |
| container | src/container/ | Provider 接口、Registry、VirtualTreeWriter / provider interface, registry, virtual tree writer |
| server | src/server/ | 只读 Web 服务：HTTP 层 + Viewer（JSON API）+ 前端资源 / read-only web service: HTTP layer + viewer (JSON API) + frontend assets |
| filesystem | src/filesystem/ | 跨平台文件系统操作（预留）/ cross-platform filesystem operations (reserved) |
| platform | src/platform/ | 平台相关代码（预留）/ platform-specific code (reserved) |
| providers/iso | providers/iso/ | ISO9660 / Joliet / UDF 解析；Rock Ridge (SUSP/RRIP) 属性与 rr_moved 还原 / ISO9660 / Joliet / UDF parsing; Rock Ridge (SUSP/RRIP) attributes and rr_moved restoration |
| web | web/ | 内嵌前端源文件（index.html，经 CMake 嵌入二进制）/ embedded frontend source (index.html, embedded into the binary via CMake) |

## Web 查看器（serve）设计思路 / Web Viewer (serve) Design

`serve` 子命令是**只读**的本机 Web 查看器，位于 CLI 与前端之间：

The `serve` subcommand is a **read-only** local web viewer that sits between the CLI and the frontend:

```
CLI serve（薄壳：解析 --port / --web-root）
    → liboffcat_server（src/server/）
        ├─ http.cpp    HTTP 基础设施（请求解析、响应、URL 解码）
        ├─ viewer.cpp  Viewer：JSON API + 静态资源服务
        └─ server.cpp  socket 服务器（仅绑定 127.0.0.1，单连接一线程）
    → 前端（可插拔）
        ├─ 内置：web/index.html（CMake 嵌入 web_resources.h，单文件零依赖）
        └─ 外部：--web-root <dir> 任意静态站点（VVV 等）
```

设计决策 / Design decisions:

1. **前后端通过 HTTP JSON API 解耦（A 契约）**：前端只需实现
   [web-api.md](web-api.md) 描述的契约，可用任何技术栈（内置 HTML、
   VVV 或其它方案），核心零改动即可整体替换前端。
   **Frontend and backend are decoupled through the HTTP JSON API (contract A)**: the frontend only needs to implement the contract in [web-api.md](web-api.md); any stack works (built-in HTML, VVV or others) and the whole frontend can be replaced without touching the core.
2. **核心库 API（B 契约）与 Web API（A 契约）分层**：C++ 程序继续
   直接链接 liboffcat_core 使用完整能力；浏览器前端只消费只读 JSON，
   两层互不依赖。
   **Core library API (contract B) and Web API (contract A) are layered**: C++ programs keep linking liboffcat_core directly for full capabilities, while browser frontends only consume read-only JSON; the two layers are independent.
3. **只读约束**：数据库以 `SQLITE_OPEN_READONLY` 打开（server.cpp），
   API 只有 GET；写操作只通过 CLI（scan 等）进行，Web 层无法修改
   原始扫描数据。
   **Read-only constraint**: the database is opened with `SQLITE_OPEN_READONLY` (server.cpp) and the API only has GET; writes happen exclusively through the CLI (scan etc.), so the web layer cannot alter scanned data.
4. **前端可整体替换**：`--web-root` 提供目录后，`/` 与静态资源改从
   该目录读取（`index.html` 优先于内嵌版本），API 路径不变。
   **The frontend is fully replaceable**: once `--web-root` is given, `/` and static assets are served from that directory (`index.html` takes precedence over the embedded one), while API paths stay unchanged.

核心原则第 10 条（数据库独立于 GUI 和导出格式）是这一设计的基础。

Core principle #10 (the database is independent of the GUI and export formats) is the foundation of this design.

## 核心原则 / Core Principles

1. Catalog 编目 Source，而不是 Disk
   The catalog catalogs sources, not disks.
2. 物理文件可以同时是 Container
   A physical file can also be a container.
3. Container 内容是 Virtual Entry，不是解压的物理文件
   Container contents are virtual entries, not extracted physical files.
4. Container 解析不需要挂载或解压
   Container parsing requires no mounting or extraction.
5. 数据库支持嵌套容器，扫描深度可配置且有限
   The database supports nested containers; scan depth is configurable and bounded.
6. ISO9660/Joliet/UDF 是 Phase 1 一等公民
   ISO9660/Joliet/UDF are first-class citizens in Phase 1.
7. 其他容器格式是扩展，非核心功能
   Other container formats are extensions, not core functionality.
8. UTF-8 是内部字符串表示
   UTF-8 is the internal string representation.
9. 可选元数据（校验码）禁用时零开销
   Optional metadata (checksums) costs nothing when disabled.
10. 数据库独立于 GUI 和导出格式
    The database is independent of the GUI and export formats.

## 扫描流程 / Scan Flow

```
scan_source(path, options)
  → 创建 Source 记录
  → 创建 Scan 记录
  → 路径是目录：递归遍历（std::filesystem）
      → 每个 Entry 读取元数据并批量插入（每 1000 条 checkpoint）
      → 同步更新 FTS5 索引
      → 启用 --containers 时：ISO 文件 → Container 记录 → Provider 展开
      → 启用 checksum 时：流式计算 SHA-256/MD5/CRC32
  → 路径是单个文件（.iso/.img）：同样注册 Container 并展开
  → 提交事务
  → Scan 状态标记 completed / cancelled
```

容器发现与展开共用同一路径：目录中遇到的 ISO 文件与直接扫描的
单文件源都经过 `expand_container_if_needed` → ProviderRegistry →
ISO Provider（见 [Provider API](provider-api.md)）。

Container discovery and expansion share one path: ISO files found in
directories and single-file sources scanned directly both go through
`expand_container_if_needed` → ProviderRegistry → the ISO provider (see
[Provider API](provider-api.md)).

## Windows 非 ASCII 命令行参数 / Non-ASCII Command-Line Arguments on Windows

PowerShell/cmd 启动 native 进程时，命令行参数按系统 ANSI 代码页
（如 GBK）编码，而代码内部一律使用 UTF-8（核心原则第 8 条）：若直接
使用 `main(argc, argv)` 的窄字符串，中文路径/搜索词会因代码页解码
失败而乱码或抛 `filesystem_error`。

When PowerShell/cmd launches a native process, command-line arguments are
encoded in the system ANSI code page (e.g. GBK), while the codebase always uses
UTF-8 internally (core principle #8): using the narrow strings of
`main(argc, argv)` directly would garble Chinese paths/search terms or throw
`filesystem_error` due to code-page decoding failures.

处理方式（cli/main.cpp 的 `utf8_argv()`）：

How it is handled (`utf8_argv()` in cli/main.cpp):

- 通过 `GetCommandLineW` + `CommandLineToArgvW` 获取宽字符参数
  Obtain wide-character arguments via `GetCommandLineW` + `CommandLineToArgvW`.
- 用 `WideCharToMultiByte(CP_UTF8)` 转换为 UTF-8 重建 argv
  Rebuild argv as UTF-8 with `WideCharToMultiByte(CP_UTF8)`.

该转换与控制台代码页无关，保证任意语言路径（含中文）在扫描、搜索
等所有子命令中正确传递。

The conversion is independent of the console code page, so paths in any
language (including Chinese) pass through correctly in all subcommands such as
scan and search.

## ISO Provider 流程 / ISO Provider Flow

```
probe(filepath)
  → UDF parser（NSR02/NSR03 检测）
  → ISO9660 parser（CD001 检测）

scan(container_entry_id, db, options)
  → 优先 UDF（最完整）
  → 其次 Joliet（Unicode 文件名）
  → 最后 ISO9660（含 Rock Ridge）
      → SUSP/RRIP 解析：NM 名优先、PX 权限、TF 时间、SL 符号链接、CE 续区
      → 定位 rr_moved/.rr_moved，占位符（0x02-0x09）还原为真实目录
      → 无法还原的占位符跳过
  → 目录树始终完整展开（防死循环上限 256 层），写入虚拟条目
  → 发现 `.iso`/`.img`/`.udf` 文件且当前嵌套层数 < max_depth 时，
    提取到临时文件后递归展开（ISO Provider 内部完成）
  → 虚拟条目经 VirtualTreeWriter 写入（is_virtual=1 + FTS5）
```

嵌套层数由 CLI `--depth` → ScanOptions.max_container_depth →
ContainerOptions.max_depth 传递；`--depth 0` 不展开任何容器，
`--depth 1`（默认）展开一层容器且其内部目录树完整收录，
`--depth 2+` 逐层展开内嵌容器。UDF 解析器内部另有深度上限 64
防止损坏镜像导致无限递归。

Nesting depth flows from the CLI `--depth` → ScanOptions.max_container_depth
→ ContainerOptions.max_depth; `--depth 0` expands nothing, `--depth 1`
(default) expands one level with its full inner tree, and `--depth 2+`
expands nested containers level by level. The UDF parser additionally enforces
an internal depth limit of 64 to prevent infinite recursion on corrupt images.

## 错误处理 / Error Handling

- 单文件读取失败 → 记录 warning，继续扫描
  A single file read failure → log a warning and keep scanning.
- Container 损坏 → 记录 error，继续扫描其他文件
  A corrupt container → log an error and keep scanning other files.
- 取消 → 停止后续扫描，事务正确结束，已完成数据保留
  Cancellation → stop further scanning, finish the transaction cleanly, and keep what was already scanned.

## 已知限制与未验证项 / Known Limitations and Unverified Items

与 spec 完成标准的差距，如实记录：

Honest gaps against the spec's completion criteria:

| 条目 / Item | 状态 / Status | 说明 / Notes |
|------|------|------|
| Linux / macOS 构建 / builds | 未验证 / unverified | 代码无平台依赖（平台逻辑集中在 src/platform/），但仅 Windows/MinGW 实测过，无 CI / the code has no platform dependencies (platform logic lives in src/platform/) but only Windows/MinGW is tested; no CI |
| million-entry 测试 / tests | 未实现 / not implemented | 无百万级条目规模/性能测试；最大真实验证为单镜像 5k+ 条目 / no million-entry scale/performance tests; largest real validation is 5k+ entries in a single image |
| name_encoding / name_confidence 持久化 / persistence | 未实现 / not implemented | 三级容错解析已实现，但解析方式元数据未入库（见 catalog-schema.md 预留字段）/ three-level fallback parsing exists, but the parsing-mode metadata is not stored (see reserved fields in catalog-schema.md) |
| available_filesystems 记录 / recording | 未实现 / not implemented | 多文件系统回退已实现，检测到的文件系统集合未记录（见 catalog-schema.md）/ multi-filesystem fallback exists but the detected filesystem set is not recorded (see catalog-schema.md) |
| fingerprint 预留 API / API | 未预留 / not reserved | spec 允许 Phase 1 不实现，当前无任何指纹相关接口 / the spec allows skipping it in Phase 1; no fingerprint interface exists yet |
| 按格式容器深度（`--depth-format`）/ per-format depth | 未实现 / not implemented | 当前 `--depth` 对所有容器类型全局统一；未来引入多格式容器（ZIP/TAR 等）时按类型覆盖（如 `iso=3,zip=1`）。深度由 scanner 在调用点查 per-format 表决定，Provider 接口无需改动；CLI 语法与 Scan.options JSON 届时一并扩展 / `--depth` currently applies globally to all container types; when multi-format containers (ZIP/TAR etc.) arrive, it will be overridable per format (e.g. `iso=3,zip=1`). Depth will be looked up from a per-format table at the scanner call site; the provider interface stays unchanged; the CLI syntax and Scan.options JSON will be extended then |

以上条目不阻塞 Phase 1 核心功能（扫描/搜索/ISO 展开/校验码），属于后续补齐项。

None of the above blocks Phase 1 core functionality (scan/search/ISO expansion/checksums); they are follow-up items.
