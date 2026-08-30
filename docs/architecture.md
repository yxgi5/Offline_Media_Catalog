# Architecture

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

## 模块划分

| 模块 | 目录 | 职责 |
|------|------|------|
| core | src/core/ | 通用类型、Result、Logger、Checksum 引擎 |
| catalog | src/catalog/ | Source/Entry/Container/Checksum/Scan 管理 |
| database | src/database/ | SQLite 封装、Schema、事务、Statement |
| scanner | src/scanner/ | 文件系统扫描、取消、搜索 |
| container | src/container/ | Provider 接口、Registry、VirtualTreeWriter |
| server | src/server/ | 只读 Web 服务：HTTP 层 + Viewer（JSON API）+ 前端资源 |
| filesystem | src/filesystem/ | 跨平台文件系统操作（预留） |
| platform | src/platform/ | 平台相关代码（预留） |
| providers/iso | providers/iso/ | ISO9660 / Joliet / UDF 解析；Rock Ridge (SUSP/RRIP) 属性与 rr_moved 还原 |
| web | web/ | 内嵌前端源文件（index.html，经 CMake 嵌入二进制） |

## Web 查看器（serve）设计思路

`serve` 子命令是**只读**的本机 Web 查看器，位于 CLI 与前端之间：

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

设计决策：

1. **前后端通过 HTTP JSON API 解耦（A 契约）**：前端只需实现
   [web-api.md](web-api.md) 描述的契约，可用任何技术栈（内置 HTML、
   VVV 或其它方案），核心零改动即可整体替换前端。
2. **核心库 API（B 契约）与 Web API（A 契约）分层**：C++ 程序继续
   直接链接 liboffcat_core 使用完整能力；浏览器前端只消费只读 JSON，
   两层互不依赖。
3. **只读约束**：数据库以 `SQLITE_OPEN_READONLY` 打开（server.cpp），
   API 只有 GET；写操作只通过 CLI（scan 等）进行，Web 层无法修改
   原始扫描数据。
4. **前端可整体替换**：`--web-root` 提供目录后，`/` 与静态资源改从
   该目录读取（`index.html` 优先于内嵌版本），API 路径不变。

核心原则第 10 条（数据库独立于 GUI 和导出格式）是这一设计的基础。

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

## ISO Provider 流程

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
  → 按 options.max_depth 递归展开目录树（ISO9660/Joliet/UDF 统一，默认 1 层）
  → 虚拟条目经 VirtualTreeWriter 写入（is_virtual=1 + FTS5）
```

递归深度由 CLI `--depth` → ScanOptions.max_container_depth →
ContainerOptions.max_depth 传递；UDF 解析器内部另有深度上限 64
防止损坏镜像导致无限递归。

## 错误处理

- 单文件读取失败 → 记录 warning，继续扫描
- Container 损坏 → 记录 error，继续扫描其他文件
- 取消 → 停止后续扫描，事务正确结束，已完成数据保留

## 已知限制与未验证项

与 spec 完成标准的差距，如实记录：

| 条目 | 状态 | 说明 |
|------|------|------|
| Linux / macOS 构建 | 未验证 | 代码无平台依赖（平台逻辑集中在 src/platform/），但仅 Windows/MinGW 实测过，无 CI |
| million-entry 测试 | 未实现 | 无百万级条目规模/性能测试；最大真实验证为单镜像 5k+ 条目 |
| name_encoding / name_confidence 持久化 | 未实现 | 三级容错解析已实现，但解析方式元数据未入库（见 catalog-schema.md 预留字段） |
| available_filesystems 记录 | 未实现 | 多文件系统回退已实现，检测到的文件系统集合未记录（见 catalog-schema.md） |
| fingerprint 预留 API | 未预留 | spec 允许 Phase 1 不实现，当前无任何指纹相关接口 |
| 按格式容器深度（`--depth-format`） | 未实现 | 当前 `--depth` 对所有容器类型全局统一；未来引入多格式容器（ZIP/TAR 等）时按类型覆盖（如 `iso=3,zip=1`）。深度由 scanner 在调用点查 per-format 表决定，Provider 接口无需改动；CLI 语法与 Scan.options JSON 届时一并扩展 |

以上条目不阻塞 Phase 1 核心功能（扫描/搜索/ISO 展开/校验码），属于后续补齐项。
