# Web Viewer API 契约 / Web Viewer API Contract

`offcat serve` 是**只读**的目录浏览服务。前端（内置 HTML、VVV 或任何
静态站点）与核心之间只通过本文档描述的 HTTP JSON API 交互——前端
不需要了解 C++/SQLite，核心也不需要了解前端。

`offcat serve` is a **read-only** directory-browsing service. The frontend
(built-in HTML, VVV or any static site) talks to the core only through the
HTTP JSON API described here — the frontend does not need to know C++/SQLite,
and the core does not need to know the frontend.

## 启动 / Starting

```
offcat serve [--port <N>] [--web-root <dir>] <catalog.db>
```

| 参数 / Argument | 默认 / Default | 说明 / Description |
|------|------|------|
| `--port` | 8080 | 监听 127.0.0.1 的 TCP 端口 / TCP port listening on 127.0.0.1 |
| `--web-root <dir>` | 无（内嵌前端）/ none (embedded frontend) | 前端静态资源目录；提供后 `/` 和静态文件从该目录读取 / frontend static-asset directory; when provided, `/` and static files are served from it |
| `<catalog.db>` | — | 目录数据库（**只读**打开，任何接口都无法修改数据）/ the catalog database (opened **read-only**; no endpoint can modify data) |

- 未指定 `--web-root`：服务内嵌的 `index.html`（单文件部署，零依赖）。
  Without `--web-root`: the embedded `index.html` is served (single-file deployment, zero dependencies).
- 指定 `--web-root`：该目录下的任意静态资源都会被托管（HTML/CSS/JS/图片
  等），`index.html` 优先于内嵌版本。路径穿越（`..`）被拒绝。
  With `--web-root`: any static assets in that directory are hosted
  (HTML/CSS/JS/images etc.), and `index.html` takes precedence over the
  embedded one. Path traversal (`..`) is rejected.
- 所有响应 `Cache-Control: no-store`；仅支持 `GET`。
  All responses carry `Cache-Control: no-store`; only `GET` is supported.

## 通用约定 / General Conventions

- 编码：UTF-8（请求 URL 百分号编码，响应 JSON 原生 UTF-8）。
  Encoding: UTF-8 (request URLs are percent-encoded; response JSON is native UTF-8).
- 文本类型响应带 `charset=utf-8`。
  Text responses include `charset=utf-8`.
- 出错时 JSON API 返回 `{"error":"..."}`（搜索）或空对象/空数组
  （其他接口）；未知路径返回 `404`。
  On errors the JSON API returns `{"error":"..."}` (search) or an empty
  object/array (other endpoints); unknown paths return `404`.

## 接口一览 / Endpoint Overview

| 端点 / Endpoint | 用途 / Purpose |
|------|------|
| `GET /api/stats` | 库统计 / catalog statistics |
| `GET /api/sources` | Source 列表 / source list |
| `GET /api/tree?parent_id=<N>&source_id=<N>` | 目录树节点 / directory tree nodes |
| `GET /api/entry?id=<N>` | 条目详情 / entry details |
| `GET /api/search?q=<词>&limit=<N>` | FTS5 全文搜索 / FTS5 full-text search |

### `GET /api/stats`

```json
{"sources":1,"entries":8125,"containers":64}
```

### `GET /api/sources`

```json
[{"id":1,"name":"S:\\H8A\\AV","type":"directory","path":"S:\\H8A\\AV\\","entries":8125}]
```

- `type`：`directory` / `file` / `iso`（Source 本身是文件时）。
  `type`: `directory` / `file` / `iso` (when the source itself is a file).

### `GET /api/tree?parent_id=<N>&source_id=<N>`

- `parent_id=0` 时返回指定 `source_id` 的根条目；否则返回
  `parent_id` 的子条目。
  With `parent_id=0`, returns the root entries of the given `source_id`;
  otherwise returns the children of `parent_id`.
- 结果按"目录在前、名称字典序"排序。
  Results are sorted with directories first, then by name.

```json
[{"id":704,"source_id":1,"parent_id":703,"name":"torrent.iso",
  "type":"file","size":4603346944,"mtime":1784128018,
  "is_virtual":false,"is_container":true,"is_dir":false}]
```

字段说明 / Field reference:

| 字段 / Field | 含义 / Meaning |
|------|------|
| `id` / `source_id` / `parent_id` | 条目标识（`parent_id=0` 表示根）/ entry identifiers (`parent_id=0` means root) |
| `name` | 条目名（UTF-8，可能含任意字符）/ entry name (UTF-8, may contain any characters) |
| `type` | `directory` / `file` / `symlink` / `other` |
| `size` / `mtime` | 字节大小 / 修改时间（Unix 秒）/ size in bytes / mtime (Unix seconds) |
| `is_virtual` | 是否容器内虚拟条目（未解压的 ISO 内容）/ whether this is a virtual entry inside a container (unpacked ISO contents) |
| `is_container` | 是否容器（如 ISO），**其内部条目通过 `parent_id=<id>` 展开** / whether this is a container (e.g. an ISO); **its inner entries are expanded via `parent_id=<id>`** |
| `is_dir` | 是否目录 / whether it is a directory |

**注意**：`is_container` 与 `is_dir` 独立——容器（如 `torrent.iso`）是
`is_dir:false, is_container:true`，但仍有子条目。可展开判断应使用
`is_dir || is_container`。

**Note**: `is_container` and `is_dir` are independent — a container (e.g.
`torrent.iso`) is `is_dir:false, is_container:true` yet still has children.
Use `is_dir || is_container` to decide whether an entry can be expanded.

### `GET /api/entry?id=<N>`

```json
{"id":704,"source_id":1,"parent_id":703,"name":"torrent.iso","type":"file",
 "size":4603346944,"mtime":1784128018,"is_virtual":false,"is_container":true,
 "is_dir":false,"path":"torrent/torrent.iso","ctime":0,"atime":0,
 "birthtime":0,"mode":0,"attributes":438,
 "container":{"type":"iso","provider":"iso_provider"},"checksums":[]}
```

- `path`：从 Source 根到该条目的完整相对路径（`/` 分隔）。
  `path`: the full relative path from the source root to this entry (`/`-separated).
- `container`：`null` 或 `{"type":"iso","provider":"..."}`。
  `container`: `null` or `{"type":"iso","provider":"..."}`.
- `checksums`：数组 `{"algorithm":"sha256","value":"<hex>","calculated_at":<unix秒>}`。
  `checksums`: array of `{"algorithm":"sha256","value":"<hex>","calculated_at":<unix seconds>}`.

### `GET /api/search?q=<词>&limit=<N>`

- FTS5 全文搜索（文件名 + 路径 + Source 名），`limit` 默认 200。
  FTS5 full-text search (file name + path + source name); `limit` defaults to 200.
- `q` 为空返回 `[]`；内部错误返回 `{"error":"..."}`。
  An empty `q` returns `[]`; internal errors return `{"error":"..."}`.

```json
[{"entry_id":707,"entry_name":"0b89....torrent",
  "full_path":"torrent/torrent.iso/torrent/0b89....torrent",
  "source_name":"S:\\H8A\\AV","type":"file","size":58640,
  "is_virtual":true,"container_type":""}]
```

## 内嵌前端 / Embedded Frontend

未指定 `--web-root` 时，`/` 返回编译进二进制的 `index.html`
（源码在 `web/index.html`，通过 CMake 嵌入）。该页面本身是
本契约的参考实现：树形浏览 + 搜索 + 详情。

Without `--web-root`, `/` serves the `index.html` compiled into the binary
(source in `web/index.html`, embedded via CMake). That page is a reference
implementation of this contract: tree browsing + search + details.

## 只读保证 / Read-Only Guarantee

数据库以只读方式打开；服务进程内没有任何写路径。验证方式：
扫描前后 `test.db` 文件大小与时间戳不变，仅产生 0 字节 `-wal`
与 `-shm`（SQLite 只读连接的正常副作用）。

The database is opened read-only; the server process has no write path at all.
Verification: the `test.db` file's size and timestamp stay unchanged before
and after, producing only 0-byte `-wal` and `-shm` files (the normal side
effects of a SQLite read-only connection).
