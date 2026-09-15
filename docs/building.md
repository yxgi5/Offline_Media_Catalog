# 手动构建 / Building from Source

本文件汇总各环境**实测过**的手动构建命令：MSVC 三条路线、MinGW-w64（Strawberry）、
MSYS2（两个 shell）与 Linux。README 的[构建](../README.md#构建--building)一节
是通用快速流程，本文是其展开版，用于手动编译与排查构建问题。

This document collects the *verified* manual build commands for every environment:
the three MSVC routes, MinGW-w64 (Strawberry), MSYS2 (both shells) and Linux. The
[Building](../README.md#构建--building) section of the README is the common quick
flow; this is its long form, meant for hands-on builds and build troubleshooting.

## 已验证环境 / Verified Environments

| 环境 / Environment | 工具链 / Toolchain | 生成器 / Generator | 验证 / Verified |
|------|------|------|------|
| MSVC（Windows） | VS 2022 Community，MSVC 19.44（x64） | Visual Studio 17 2022 / Ninja / NMake | 本机实测 / local |
| MinGW-w64（Strawberry Perl） | `D:\Strawberry\c\bin\gcc.exe`，GCC 13.1.0 | Ninja 1.11.1 | 本机实测 / local |
| MSYS2 MINGW64 shell | `/mingw64/bin/g++`，GCC 15.2.0 | Unix Makefiles | 本机实测 / local |
| MSYS2 MSYS shell | `/mingw64/bin/g++`，GCC 15.2.0 | Unix Makefiles（附加参数见下） | 本机实测 / local |
| Linux | GCC（ubuntu-22.04，glibc 2.35+） | Unix Makefiles | CI 验证 / CI |

下文命令中的 `cmake` 指 PATH 中的 CMake（本机为 `D:\Program Files\CMake\bin\cmake.exe`，
MSYS2 下为 `/usr/bin/cmake`）。CMake 最低版本要求见 CMakeLists.txt（3.16）。

`cmake` below means the one on PATH (locally `D:\Program Files\CMake\bin\cmake.exe`,
inside MSYS2 `/usr/bin/cmake`). The minimum CMake version is declared at the top of
CMakeLists.txt (3.16).

## 通用步骤 / Common Steps

```bash
# 配置 / configure（单配置生成器用 CMAKE_BUILD_TYPE，多配置生成器用 --config）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 编译 / build
cmake --build build -j

# 测试 / test（需 OFFCAT_BUILD_TESTS=ON，默认开启 / needs tests enabled, the default）
ctest --test-dir build
```

产物 / Artifacts：

| 目标 / Target | 路径 / Path |
|------|------|
| CLI | `build/offcat`（Windows / Windows: `build/offcat.exe`） |
| 测试 / Tests | `build/offcat_tests`（Windows / Windows: `build/offcat_tests.exe`） |

常用配置项 / Common options：

| 选项 / Option | 说明 / Description |
|------|------|
| `-DOFFCAT_BUILD_TESTS=ON\|OFF` | 是否构建测试套件（默认 ON）/ build the test suite (default ON) |
| `-DOFFCAT_USE_OPENSSL=ON` | 使用 OpenSSL 计算 SHA-256/MD5（默认内置实现）/ OpenSSL for SHA-256/MD5 instead of the built-in code |
| `-DOFFCAT_WERROR=ON` | 警告视为错误（GCC/Clang；CI 开启）/ warnings as errors (GCC/Clang; used by CI) |
| `-DOFFCAT_SANITIZE=ON` | ASan + UBSan（GCC/Clang；CI 开启）/ ASan + UBSan (GCC/Clang; used by CI) |
| `-DGOOGLETEST_SOURCE_DIR=<dir>` | 复用本地 googletest 源码目录，省去解压 / reuse a local googletest checkout |
| `-DOFFCAT_VERSION=x.y.z` | 覆盖版本号（默认取最近 git tag）/ override the version (default: latest git tag) |

## Windows：MSVC 三条路线 / Windows: Three MSVC Routes

MSVC 没有 `make`：Visual Studio 生成器产出的是 `.sln`/`.vcxproj`（由 MSBuild 驱动，
目录里没有 Makefile），Ninja 与 NMake 则是单配置生成器。三者都用 `cmake --build` 驱动；
只想编主程序时加 `-DOFFCAT_BUILD_TESTS=OFF`（更快，也不需要 googletest）。

MSVC ships no `make`: the Visual Studio generator emits `.sln`/`.vcxproj` driven by
MSBuild (no Makefile in the build directory), while Ninja and NMake are single-config
generators. All three are driven through `cmake --build`; add `-DOFFCAT_BUILD_TESTS=OFF`
for a CLI-only build (faster, no googletest needed).

### 加载 VS 开发者环境 / Loading the VS Developer Environment

Ninja 与 NMake 要求 `cl.exe` / `nmake.exe` 已在 PATH 中（VS 生成器不需要，CMake 能自行
定位 MSBuild）。三种方式任选：

Ninja and NMake require `cl.exe` / `nmake.exe` to be on PATH (the VS generator does
not — CMake locates MSBuild itself). Pick one of three ways:

1. 开始菜单打开 "x64 Native Tools Command Prompt for VS 2022"，之后命令直接可用 / open
   "x64 Native Tools Command Prompt for VS 2022" from the Start menu.
2. 普通 cmd / plain cmd：
   ```bat
   call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
   ```
3. PowerShell：
   ```powershell
   Import-Module "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
   Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\2022\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
   ```

未加载环境时会在配置阶段直接失败，例如 `Running 'nmake' '-?' failed with: no such file or directory`。

Without the environment, configuration fails immediately, e.g. `Running 'nmake' '-?'
failed with: no such file or directory`.

### 路线 A：Visual Studio 生成器（推荐，无需开发者环境）/ Route A: Visual Studio generator

多配置生成器：Debug/Release 在**构建时**用 `--config` 选择，省略时默认 Debug。

Multi-config generator: Debug/Release are picked at *build* time via `--config`,
defaulting to Debug when omitted.

```bat
cmake -S . -B build5 -G "Visual Studio 17 2022" -A x64
cmake --build build5 --config Release                    :: 全部目标 / all targets
cmake --build build5 --config Release --target offcat     :: 只编主程序 / CLI only
ctest --test-dir build5 -C Release                        :: 多配置生成器的测试需 -C / -C is required for multi-config tests
```

产物在 `build5\Release\offcat.exe`（多配置的输出带 `Release\` 子目录），也可以直接打开
`build5\offcat.sln` 用 Visual Studio 编译。

Artifacts land in `build5\Release\offcat.exe` (multi-config output has a `Release\`
subdirectory); opening `build5\offcat.sln` in Visual Studio works too.

### 路线 B：Ninja（推荐用于日常迭代）/ Route B: Ninja

单配置、增量最快；需要先加载开发者环境（见上）。

Single-config, fastest incremental builds; load the developer environment first.

```bat
cmake -S . -B build6 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build6
:: 或 / or: ninja -C build6
ctest --test-dir build6
```

### 路线 C：NMake（字面意义的 "make"）/ Route C: NMake

```bat
cmake -S . -B build7 -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build7
:: 或 / or: cd build7 && nmake
ctest --test-dir build7
```

串行构建，比 Ninja 慢；适合习惯 make 流程的场景。

Serial builds, slower than Ninja; handy when a make-style flow is preferred.

### 三条路线对比 / Route Comparison

| 路线 / Route | 生成器 / Generator | 构建命令 / Build command | 需开发者环境 / Dev env | 特点 / Notes |
|------|------|------|------|------|
| A | `"Visual Studio 17 2022"` | `cmake --build <dir> --config Release` | 否 / no | 多配置，可用 VS 打开工程 / multi-config, opens in VS |
| B | `Ninja` | `cmake --build <dir>` | 是 / yes | 单配置，增量最快 / single-config, fastest incremental |
| C | `"NMake Makefiles"` | `cmake --build <dir>` 或 `nmake` | 是 / yes | 单配置，串行 / single-config, serial |

## Windows：MinGW-w64（Strawberry）/ Windows: MinGW-w64 (Strawberry)

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

若 `gcc` / `g++` / `ninja` 不在 PATH（Strawberry 的 `c\bin` 未加入），显式指定即可：

If `gcc` / `g++` / `ninja` are not on PATH (Strawberry's `c\bin` not added), pass them
explicitly:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=D:/Strawberry/c/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=D:/Strawberry/c/bin/c++.exe ^
  -DCMAKE_MAKE_PROGRAM=D:/Strawberry/c/bin/ninja.exe
cmake --build build
```

（`^` 是 cmd 的续行符。/ `^` is cmd's line continuation character.）

## Windows：MSYS2 / Windows: MSYS2

MSYS2 有 MINGW64 与 MSYS 两个 shell，对本项目影响很大：

MSYS2 has two shells, MINGW64 and MSYS, and the difference matters here:

- **MINGW64 shell**：CMake 判定 `WIN32=TRUE`，平台分支正常 / CMake reports `WIN32=TRUE`,
  all platform branches behave normally.
- **MSYS shell**：CMake 把平台识别为 MSYS、`WIN32=FALSE`，产物仍是原生 Windows 程序。
  项目已用 `OFFCAT_WINDOWS`（`WIN32 OR CYGWIN OR MSYS`）谓词统一判定 Windows 专属配置
  （`ws2_32`、`_WIN32_WINNT`/`NOMINMAX`、静态链接选项），因此 `-lws2_32` 在这两个 shell
  下都会进入链接行；在 Linux 上该谓词为 FALSE，保证 `ws2_32` 不会污染链接。
  / CMake reports the platform as MSYS with `WIN32=FALSE` while the compiler still
  produces native Windows binaries. The project routes every Windows-only setting
  (`ws2_32`, `_WIN32_WINNT`/`NOMINMAX`, static-link flags) through an `OFFCAT_WINDOWS`
  (`WIN32 OR CYGWIN OR MSYS`) predicate, so `-lws2_32` reaches the link line in both
  shells and stays off it on Linux.

### MINGW64 shell

```bash
cmake -S . -B build-win -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j 8
ctest --test-dir build-win
```

MSYS2 默认不带 Ninja（需要时 `pacman -S mingw-w64-x86_64-ninja`，之后可 `-G Ninja`）。

Ninja is not installed by default in MSYS2 (`pacman -S mingw-w64-x86_64-ninja` if you
want `-G Ninja`).

### MSYS shell

```bash
cmake -S . -B build3 -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_DEPENDS_USE_COMPILER=FALSE
cmake --build build3 -j 8
ctest --test-dir build3
```

> **必须带 `-DCMAKE_DEPENDS_USE_COMPILER=FALSE`**：CMake 4.3 在 MSYS shell 下生成的依赖
> 文件 `compiler_depend.make` 会出现"相对前缀 + Windows 绝对路径"的畸形条目（如
> `_deps/googletest-build/googletest/C:/Users/.../gtest.h`），路径里的 `C:` 被 GNU Make
> 当作规则分隔符，构建时报 `compiler_depend.make:4: *** 目标不匹配`。该选项让 CMake
> 自行扫描依赖而不读取编译器的 `-MD` 输出，畸形文件不再产生；Linux 与原生 MinGW 不受影响。
> **每新建一个 MSYS shell 的构建目录都要带上它。**
>
> **The flag is mandatory in the MSYS shell**: with CMake 4.3 the generated
> `compiler_depend.make` contains malformed entries ("relative prefix + Windows
> absolute path", e.g. `_deps/googletest-build/googletest/C:/Users/.../gtest.h`); GNU
> Make reads the `C:` as a rule separator and aborts with an error pointing at
> `compiler_depend.make`. The flag makes CMake scan dependencies itself instead of
> consuming the compiler's `-MD` output, so the malformed file is never produced;
> Linux and native MinGW are unaffected. Pass it for **every** new MSYS-shell build
> directory.

## Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

CI 用同一套命令在 ubuntu-22.04 上构建（保留 glibc 2.35 兼容性），`-DOFFCAT_SANITIZE=ON`
（ASan + UBSan）亦由 CI 覆盖。Linux 下 `OFFCAT_WINDOWS` 为 FALSE，`ws2_32` 不会进入链接行。

CI builds with the same commands on ubuntu-22.04 (keeping glibc 2.35 compatibility),
including a `-DOFFCAT_SANITIZE=ON` (ASan + UBSan) job. On Linux `OFFCAT_WINDOWS` is
FALSE, so `ws2_32` never reaches the link line.

## 静态链接与编码 / Static Linking and Encoding

Release 默认产出**全静态**单文件二进制，无需额外参数；相关配置都在 CMakeLists.txt 顶部：

Release builds are **fully static** single-file binaries by default, with no extra
flags required; the relevant settings live at the top of CMakeLists.txt:

| 配置 / Setting | 条件 / Condition | 作用 / Effect |
|------|------|------|
| `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>` | `if(MSVC)` | Release `/MT`、Debug `/MTd`，VC++ 运行库静态链接 / static VC++ runtime (`/MT`, `/MTd` in Debug) |
| `-static-libgcc -static-libstdc++ -static` | `if(MINGW OR MSYS)` | MinGW 运行库静态链接，避免误用 PATH 中其它程序的 libstdc++ DLL / static MinGW runtime, avoiding a mismatched libstdc++ DLL from PATH |
| `add_compile_options(/utf-8)` | `if(MSVC)` | 源码是无 BOM 的 UTF-8（`web/index.html` 含中文，经 `configure_file` 嵌入 `web_resources.h`），MSVC 默认按系统代码页解析会报 C2059/C2017；GCC/Clang 默认 UTF-8，无需开关 / sources are UTF-8 without BOM; MSVC would otherwise use the system code page and fail with C2059/C2017 on the embedded resources. GCC/Clang default to UTF-8 and need nothing |

对应关系：MinGW 的 `-static-libgcc`/`-static-libstdc++` 与 MSVC 的 `/MT` 是同一个取舍——
把运行库编进二进制，换取"拷贝即用"；`/utf-8` 则与 GCC/Clang 的默认编码对应。

The mapping: MinGW's `-static-libgcc`/`-static-libstdc++` and MSVC's `/MT` are the same
trade-off — the runtime is compiled into the binary so it runs anywhere; `/utf-8` is
simply what GCC/Clang do by default.

## 验证产物 / Verifying the Artifact

用 `objdump -p` 查看导入表，确认运行库确实内嵌、依赖只剩系统 DLL：

Inspect the import table with `objdump -p` to confirm the runtime is embedded and only
system DLLs remain:

```bash
objdump -p build/offcat.exe | grep 'DLL Name'           # MinGW / MSYS2
objdump -p build5/Release/offcat.exe | grep 'DLL Name'  # MSVC（VS 生成器输出带 Release\ 子目录）
```

`objdump` 可从 Strawberry（`D:\Strawberry\c\bin\objdump.exe`）或 MSYS2
（`/mingw64/bin/objdump.exe`）取用；MSVC 环境里也可用 `dumpbin /dependents <exe>`。

`objdump` comes with Strawberry (`D:\Strawberry\c\bin\objdump.exe`) or MSYS2
(`/mingw64/bin/objdump.exe`); inside a VS developer prompt, `dumpbin /dependents <exe>`
does the same.

预期结果 / Expected:

| 构建 / Build | 导入的 DLL / Imported DLLs |
|------|------|
| MSVC Release（`/MT`） | `WS2_32.dll`、`KERNEL32.dll`、`SHELL32.dll` —— 不含 `vcruntime140.dll` / `msvcp140.dll` |
| MinGW / MSYS2 Release（`-static*`） | `WS2_32.dll`、`KERNEL32.dll`、`SHELL32.dll`、`msvcrt.dll`（Windows 自带 / ships with Windows） |

## 常见问题 / Troubleshooting

| 现象 / Symptom | 原因与处理 / Cause and fix |
|------|------|
| `make : 无法将"make"项识别为...` / `make` not recognized | VS 生成器没有 Makefile，用 `cmake --build <dir> --config Release`（或打开 `<dir>\offcat.sln`）/ the VS generator has no Makefile: use `cmake --build` or open the `.sln` |
| `Running 'nmake' '-?' failed` / `no such file or directory` | Ninja/NMake 需要先加载 VS 开发者环境（见上文三种方式）/ load the VS developer environment first |
| MinGW 链接报 `undefined reference to __imp_setsockopt` 等 / `undefined reference to __imp_*` | 平台判定问题：MSYS shell 下 `WIN32=FALSE` 会让 `if(WIN32)` 静默失效。项目已用 `OFFCAT_WINDOWS` 谓词处理；新增 Windows 专属配置时请沿用该谓词，**不要**把 `ws2_32` 无条件写进 `target_link_libraries`（那会破坏 Linux 链接）/ MSYS-shell platform detection: `if(WIN32)` silently fails there. Use the `OFFCAT_WINDOWS` predicate and never link `ws2_32` unconditionally |
| `compiler_depend.make:4: *** 目标不匹配`（MSYS shell） | 加 `-DCMAKE_DEPENDS_USE_COMPILER=FALSE` 重新配置该构建目录 / reconfigure that build directory with the flag |
| 链接时 `cannot open output file offcat.exe: Permission denied` | 有正在运行的 `offcat serve` 占用了产物文件，先停止它再重新链接 / a running `offcat serve` holds the file: stop it and relink |
| MSVC 报 `web_resources.h` 的 C2059/C2017 | 源文件是 UTF-8 无 BOM；项目已在 `if(MSVC)` 下加 `/utf-8`，若自建工程需同样设置 / keep `/utf-8` for MSVC |
| 版本号不是预期的 tag | 版本来自最近 git tag；源码包/无 `.git` 时用 `-DOFFCAT_VERSION=x.y.z` 覆盖 / the version comes from the latest git tag; override with `-DOFFCAT_VERSION=x.y.z` |

## 依赖与离线构建 / Dependencies and Offline Builds

- SQLite3 amalgamation 与 GoogleTest 的 zip 随仓库维护在 `third_party/`，由 FetchContent
  解压使用，**无需联网即可构建**。
  The SQLite3 amalgamation and GoogleTest zips are vendored in `third_party/` and
  extracted by FetchContent, so builds work fully offline.
- 若 `third_party/googletest-1.14.0/` 已解压存在，或显式传入
  `-DGOOGLETEST_SOURCE_DIR=<dir>`，则直接复用本地源码，跳过解压。
  If `third_party/googletest-1.14.0/` already exists, or `-DGOOGLETEST_SOURCE_DIR=<dir>`
  is given, the local checkout is reused and the extraction step is skipped.
