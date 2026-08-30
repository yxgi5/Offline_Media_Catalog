# Web Viewer API 契约

`offcat serve` 是**只读**的目录浏览服务。前端（内置 HTML、VVV 或任何
静态站点）与核心之间只通过本文档描述的 HTTP JSON API 交互——前端
不需要了解 C++/SQLite，核心也不需要了解前端。

## 启动

```
offcat serve [--port <N>] [--web-root <dir>] <catalog.db>
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `--port` | 8080 | 监听 127.0.0.1 的 TCP 端口 |
| `--web-root <dir>` | 无（内嵌前端） | 前端静态资源目录；提供后 `/` 和静态文件从该目录读取 |
| `<catalog.db>` | — | 目录数据库（**只读**打开，任何接口都无法修改数据） |

- 未指定 `--web-root`：服务内嵌的 `index.html`（单文件部署，零依赖）。
- 指定 `--web-root`：该目录下的任意静态资源都会被托管（HTML/CSS/JS/图片
  等），`index.html` 优先于内嵌版本。路径穿越（`..`）被拒绝。
- 所有响应 `Cache-Control: no-store`；仅支持 `GET`。

## 通用约定

- 编码：UTF-8（请求 URL 百分号编码，响应 JSON 原生 UTF-8）。
- 文本类型响应带 `charset=utf-8`。
- 出错时 JSON API 返回 `{"error":"..."}`（搜索）或空对象/空数组
  （其他接口）；未知路径返回 `404`。

## 接口一览

| 端点 | 用途 |
|------|------|
| `GET /api/stats` | 库统计 |
| `GET /api/sources` | Source 列表 |
| `GET /api/tree?parent_id=<N>&source_id=<N>` | 目录树节点 |
| `GET /api/entry?id=<N>` | 条目详情 |
| `GET /api/search?q=<词>&limit=<N>` | FTS5 全文搜索 |

### `GET /api/stats`

```json
{"sources":1,"entries":8125,"containers":64}
```

### `GET /api/sources`

```json
[{"id":1,"name":"S:\\H8A\\AV","type":"directory","path":"S:\\H8A\\AV\\","entries":8125}]
```

- `type`：`directory` / `file` / `iso`（Source 本身是文件时）。

### `GET /api/tree?parent_id=<N>&source_id=<N>`

- `parent_id=0` 时返回指定 `source_id` 的根条目；否则返回
  `parent_id` 的子条目。
- 结果按"目录在前、名称字典序"排序。

```json
[{"id":704,"source_id":1,"parent_id":703,"name":"torrent.iso",
  "type":"file","size":4603346944,"mtime":1784128018,
  "is_virtual":false,"is_container":true,"is_dir":false}]
```

字段说明：

| 字段 | 含义 |
|------|------|
| `id` / `source_id` / `parent_id` | 条目标识（`parent_id=0` 表示根） |
| `name` | 条目名（UTF-8，可能含任意字符） |
| `type` | `directory` / `file` / `symlink` / `other` |
| `size` / `mtime` | 字节大小 / 修改时间（Unix 秒） |
| `is_virtual` | 是否容器内虚拟条目（未解压的 ISO 内容） |
| `is_container` | 是否容器（如 ISO），**其内部条目通过 `parent_id=<id>` 展开** |
| `is_dir` | 是否目录 |

**注意**：`is_container` 与 `is_dir` 独立——容器（如 `torrent.iso`）是
`is_dir:false, is_container:true`，但仍有子条目。可展开判断应使用
`is_dir || is_container`。

### `GET /api/entry?id=<N>`

```json
{"id":704,"source_id":1,"parent_id":703,"name":"torrent.iso","type":"file",
 "size":4603346944,"mtime":1784128018,"is_virtual":false,"is_container":true,
 "is_dir":false,"path":"torrent/torrent.iso","ctime":0,"atime":0,
 "birthtime":0,"mode":0,"attributes":438,
 "container":{"type":"iso","provider":"iso_provider"},"checksums":[]}
```

- `path`：从 Source 根到该条目的完整相对路径（`/` 分隔）。
- `container`：`null` 或 `{"type":"iso","provider":"..."}`。
- `checksums`：数组 `{"algorithm":"sha256","value":"<hex>","calculated_at":<unix秒>}`。

### `GET /api/search?q=<词>&limit=<N>`

- FTS5 全文搜索（文件名 + 路径 + Source 名），`limit` 默认 200。
- `q` 为空返回 `[]`；内部错误返回 `{"error":"..."}`。

```json
[{"entry_id":707,"entry_name":"0b89....torrent",
  "full_path":"torrent/torrent.iso/torrent/0b89....torrent",
  "source_name":"S:\\H8A\\AV","type":"file","size":58640,
  "is_virtual":true,"container_type":""}]
```

## 内嵌前端

未指定 `--web-root` 时，`/` 返回编译进二进制的 `index.html`
（源码在 `web/index.html`，通过 CMake 嵌入）。该页面本身是
本契约的参考实现：树形浏览 + 搜索 + 详情。

## 只读保证

数据库以只读方式打开；服务进程内没有任何写路径。验证方式：
扫描前后 `test.db` 文件大小与时间戳不变，仅产生 0 字节 `-wal`
与 `-shm`（SQLite 只读连接的正常副作用）。
