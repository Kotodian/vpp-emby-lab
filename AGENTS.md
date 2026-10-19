<!-- CODEGRAPH_START -->
## CodeGraph

In repositories indexed by CodeGraph (a `.codegraph/` directory exists at the repo root), reach for it BEFORE grep/find or reading files when you need to understand or locate code:

- **MCP tool** (when available): `codegraph_explore` answers most code questions in one call — the relevant symbols' verbatim source plus the call paths between them, including dynamic-dispatch hops grep can't follow. Name a file or symbol in the query to read its current line-numbered source. If it's listed but deferred, load it by name via tool search.
- **Shell** (always works): `codegraph explore "<symbol names or question>"` prints the same output.

If there is no `.codegraph/` directory, skip CodeGraph entirely — indexing is the user's decision.
<!-- CODEGRAPH_END -->

# VPP 构建系统指南

本仓库的构建入口是顶层 `Makefile`，真正的编译由 CMake 生成 Ninja 工程完成。修改构建相关文件前先读本文件，优先复用既有宏，不要绕过既有层级。

## 构建链路

```text
make build / make build-release
  └─ build-root/Makefile
       ├─ 根据 PLATFORM/TAG 确定 build-*/install-* 目录
       └─ build-data/packages/vpp.mk
            └─ cmake -G Ninja -S src -B <build dir>
                 └─ src/CMakeLists.txt (+ src/cmake/*.cmake)
                      └─ ninja：编译、链接、安装
```

`configure` 是绕过 make 包装层的等价入口，直接执行 `cmake -G Ninja -S src -B <build-dir>`，适合单目录、纯 CMake 工作流。

## 关键文件

- `Makefile`：用户命令入口（build/test/pkg/checkstyle 等），定义 `WS_ROOT`、`BR=build-root`、依赖安装和运行目标。
- `build-root/Makefile`：按 `PLATFORM`/`TAG` 组织构建与安装目录；`TAG` 为 `vpp`、`vpp_debug`、`vpp_gcov`、`vpp_coverity` 等。
- `build-data/packages/vpp.mk`：把 make 变量翻译成 `vpp_cmake_args` 并调用 CMake/Ninja；`VPP_EXTRA_CMAKE_ARGS` 在此原样透传。
- `build-data/platforms/vpp.mk`：定义各 TAG 的 `CMAKE_BUILD_TYPE`（`vpp_debug`→debug、`vpp`→release、`vpp_gcov`→gcov、`vpp_coverity`→coverity）。
- `src/CMakeLists.txt`：CMake 项目根；编译器检查、全局编译选项、`SUBDIRS` 顺序、构建类型和安装布局。
- `src/cmake/*.cmake`：`add_vpp_library`、`add_vpp_plugin`、`add_vpp_driver`、`add_vpp_crypto_engine`、`add_vpp_executable`、`add_vpp_tool`、`add_vpp_test` 等宏，是新增目标的首选入口。
- `src/plugins/CMakeLists.txt`、`src/drivers/CMakeLists.txt`、`src/crypto_engines/CMakeLists.txt`：用 glob 发现子目录，并应用 `VPP_PLUGINS` / `VPP_DRIVERS` / `VPP_CRYPTO_ENGINES` 过滤。
- `src/scripts/version`、`src/scripts/generate_version_h`：生成版本号和 `vpp/app/version.h`。
- `test/`：Python 功能测试框架；`test/hs-test/`：host stack / Go 测试。

## 构建目录与产物

默认 debug 构建（`make build`）：

- CMake 构建目录：`build-root/build-vpp_debug-native/vpp`
- 安装前缀：`build-root/install-vpp_debug-native/vpp`
- 二进制：`.../bin/vpp`、`.../bin/vppctl`
- 库：`<安装前缀>/lib/<arch>`（Debian 多架构）或 `<安装前缀>/lib64`（常见 RPM 发行版）
- 插件、驱动、加密引擎：`<libdir>/vpp_plugins`、`<libdir>/vpp_drivers`、`<libdir>/vpp_crypto_engines`

Release 构建（`make build-release`）使用 `build-vpp-native` 与 `install-vpp-native`。`build-root/.ccache` 是默认 ccache 目录。

常用命令：

```bash
make install-dep              # 安装系统构建依赖
make install-ext-deps         # 安装 DPDK/RDMA 等外部依赖（vpp-ext-deps）
make build                    # debug 构建 + 安装
make build-release            # release 构建 + 安装
make rebuild                  # wipe 后 debug 重建
make run                      # 运行 debug 版 vpp
make debug                    # 在 gdb 中运行 debug 版 vpp
make wipe                     # 清理 debug 构建产物（保留 startup.*）
make wipe-release             # 清理 release 构建产物
```

外部依赖查找路径：`/opt/vpp/external/<arch>`、`/opt/vpp/optional/<arch>`，以及构建树内的 `.../external` 安装前缀；CMake 通过 `CMAKE_PREFIX_PATH` 查找头文件和库。未安装系统级 ext-deps 时，`vpp-install` 依赖的 `external-install` 会在本地构建树内构建外部依赖。

外部 DPDK 默认跳过（`VPP_EXTERNAL_DPDK=no`）；需要时用 `make build VPP_EXTERNAL_DPDK=yes` 或 `make install-ext-deps VPP_EXTERNAL_DPDK=yes` 恢复。没有可用 DPDK 头文件时 `dpdk` 插件会自动禁用，也可显式加 `VPP_EXCLUDED_PLUGINS=dpdk`。

## 构建类型与常用 CMake 选项

- `CMAKE_BUILD_TYPE`：`release`（`-O3`，默认启用 LTO）、`debug`（`-O0`，定义 `CLIB_DEBUG`）、`gcov`、`coverity`。设置构建类型后会统一追加 `-g -Werror -Wall`，因此编译警告会导致构建失败。
- 构建类型切换：在构建目录执行 `ninja set-build-type-debug` 等，或重新配置。
- 透传 CMake 选项：`make build VPP_EXTRA_CMAKE_ARGS="-DVPP_...=..."`。
- CPU 平台：`VPP_PLATFORM=<name>`（如 `icelake-server`、`sapphirerapids`、`neoverse-n1`，见 `src/cmake/platform/*.cmake`）会切换为平台专用 `-march/-mcpu`；不设置时按多架构方式构建。
- `VPP_BUILD_NATIVE_ONLY=ON`：只编译本机 CPU 变体，等价于 `./configure -n`。
- 目标筛选（逗号分隔）：`VPP_PLUGINS`、`VPP_EXCLUDED_PLUGINS`、`VPP_DRIVERS`、`VPP_CRYPTO_ENGINES`、`VPP_TOOLS`、`VPP_TESTS`。`none` 表示全部禁用；请求的名称不存在时配置阶段会报错。`VPP_PLUGINS` 与 `VPP_EXCLUDED_PLUGINS` 取其一，同时设置时前者优先。
- 其他常用开关：`VPP_USE_CCACHE`、`VPP_USE_LTO`、`VPP_LIBVNET_HIDDEN_VISIBILITY`、`VPP_ENABLE_SANITIZE_ADDR`、`VPP_ENABLE_TRAJECTORY_TRACE`、`VPP_BUILD_TESTS_WITH_COVERAGE`、`VPP_API_TEST_BUILTIN`、`VPP_BUILD_VPPINFRA_TESTS`、`VPP_HOST_TOOLS_ONLY`。
- `./configure` 快捷参数：`-t/--build-type`、`-b/--build-dir`、`-i/--install-dir`、`-P/--plugins`、`-D/--drivers`、`-T/--tools`、`-S/--tests`、`-E/--crypto-engines`、`-p/--platform`、`-a/--arch`（交叉编译）、`-m/--minimal`、`-n/--native-only`、`-s mem`（ASan）。

## 新增或修改构建目标

### 插件

`src/plugins/` 下包含 `CMakeLists.txt` 的目录会被自动发现，不需要修改 `SUBDIRS`。推荐模板：

```cmake
add_vpp_plugin(foo
  SOURCES
  foo.c
  foo_node.c

  API_FILES
  foo.api

  INSTALL_HEADERS
  foo_all_api_h.h
  foo_msg_enum.h

  API_TEST_SOURCES
  foo_test.c

  MULTIARCH_SOURCES
  foo_simd.c

  LINK_LIBRARIES
  vnet vppinfra
)
```

`API_FILES` 由 `vppapigen` 在构建目录生成 `*.h`、`*_enum.h`、`*_types.h`、`*.json` 等文件，并自动加入插件依赖；不要手写或提交这些生成物。插件产物为 `<libdir>/vpp_plugins/<name>_plugin.so`（无 `lib` 前缀、无 SONAME）。`API_TEST_SOURCES` 会生成 `vpp_api_test` 使用的测试插件。

### 驱动、加密引擎、库、可执行文件

- 驱动：`src/drivers/<name>/CMakeLists.txt` 使用 `add_vpp_driver(<name> SOURCES ... LINK_LIBRARIES ... MULTIARCH_SOURCES ...)`。
- 加密引擎：`src/crypto_engines/<name>/CMakeLists.txt` 使用 `add_vpp_crypto_engine(<name> SOURCES ... LINK_LIBRARIES ...)`。
- 核心库：`add_vpp_library(<name> SOURCES ... MULTIARCH_SOURCES ... API_FILES ... INSTALL_HEADERS ... LINK_LIBRARIES ...)`；`HIDDEN` 控制符号可见性，`LTO` 控制 LTO。
- 可执行文件/工具/测试：分别使用 `add_vpp_executable`、`add_vpp_tool`、`add_vpp_test`；工具和测试可被 `VPP_TOOLS`/`VPP_TESTS` 过滤。
- 只有新增顶层子目录时，才需要把目录加入 `src/CMakeLists.txt` 的 `SUBDIRS`；该列表顺序有意义（库先于使用者）。

### API 文件

在组件目录新增 `*.api` 并加入 `API_FILES`；`vpp_add_api_files`/`add_vpp_plugin` 会生成 C 头文件、枚举、JSON 和测试桩。API 的源文件是 `.api`，生成物只存在于构建目录；修改后重新构建即可，不要编辑 `build-root/` 下的副本。

## 多架构（multiarch）

默认构建会为核心库生成多个 `-march` 变体并在运行时按 CPU 选择：

- 只有需要在特定 CPU 变体中编译的源文件才放进 `MULTIARCH_SOURCES`；普通源文件放 `SOURCES`。
- 变体定义在 `src/cmake/cpu.cmake`（x86：`x86_64_v3`、`x86_64_v4`；ARM：`neoversen1`、`neoversev2` 等）。
- 可用 `-DVPP_MARCH_VARIANT_<NAME>=ON/OFF` 控制单个变体；插件/驱动的 `MULTIARCH_FORCE_ON` 可强制某个默认关闭的变体参与编译。
- 设置 `VPP_BUILD_NATIVE_ONLY=ON` 或 `VPP_PLATFORM` 时不做多架构分发。

## 测试

```bash
make test                          # 构建并运行基础功能测试
make test TEST=test_ip4            # 按文件/类/用例过滤
make test TEST=ip4,ip6 TEST_JOBS=4 # 多过滤 + 并行
make test-debug TEST=ip4 DEBUG=gdb # debug 镜像并在 gdb 下调试
make retest                        # 不重新构建，重跑测试
make test-list TEST=ip4            # 只显示选中的测试，不执行
make test-all                      # 包含 extended tests
make test-help                     # 查看全部测试参数
```

测试框架位于 `test/`（Python，基于 `vpp_papi`），`test/hs-test/` 是 host stack/Go 测试。`TEST_JOBS` 控制并行度，`V=1` 增加日志。C 单元测试可通过 `VPP_BUILD_VPPINFRA_TESTS=ON` 等选项启用。

## 风格与提交检查

```bash
make checkstyle          # C 代码 clang-format 检查（同时校验 FEATURE.yaml）
make fixstyle            # 自动修复 C 代码风格
make checkstyle-python   # black 检查
make fixstyle-python
make checkstyle-commit   # 检查 commit message 模板
make checkstyle-go
```

构建默认带 `-Werror`；提交前至少保证相关目标能编译通过，并运行受影响的测试。

## 常见坑

- 顶层 `PLATFORM` 与 CMake 的 `VPP_PLATFORM` 不是一回事：`PLATFORM` 选择构建平台（通常为 `vpp`），`VPP_PLATFORM` 选择 CPU 微架构优化目标。
- 不要直接修改 `build-root/build-*`、`build-root/install-*` 中的文件，它们由构建过程生成。清理用 `make wipe` / `make wipe-release`，不要手工删除整棵构建树。
- `make build` 是 debug 构建，`make build-release` 才是 release；两者使用不同的构建和安装目录，切换后不要混用产物。
- 外部依赖未安装时先执行 `make install-dep` 和 `make install-ext-deps`；缺少 DPDK 等依赖时对应插件/驱动会在配置日志中给出 warning 并被跳过。
- `libvnet` 默认隐藏符号。插件、驱动或加密引擎引用 `libvnet` 符号时，该符号必须用 `__clib_export` 标注；每次构建都会运行 `check-vnet-exports` 校验，缺失会直接失败。`-DVPP_LIBVNET_HIDDEN_VISIBILITY=OFF` 只是兼容开关，不是修复手段。
- 本仓库没有名为 `tuntap` 的插件：TUN/TAP 接口由 `tap` 插件提供（`TAP_API_FLAG_TUN`），内核 tuntap 注入/punt 路径位于核心 `src/vnet/unix/tuntap.c`；IPv4/IPv6 punt 同样属于 `vnet` 核心（`ip4-punt`、`ip6-punt`、`punt.api`）。
- `af_xdp` 插件依赖 `libxdp.a` 和 `libbpf.a`，且两者必须是 PIC。优先使用 `build/external` 构建的 xdp-tools（已通过补丁为 bundled libbpf 加上 `-fPIC`）；直接链接系统非 PIC 的 `libbpf.a` 会报 `relocation R_X86_64_PC32 ... recompile with -fPIC`。
- 构建要求 CMake >= 3.19、Ninja、bash，以及 clang 或 gcc >= 9；`V=1` 可查看完整编译命令。
- 版本号来自 `git describe` 和 `src/scripts/version`；非 git 源码树需要提供 `src/scripts/.version`，否则版本生成会失败。

## 延伸阅读

- `docs/developer/corearchitecture/buildsystem/`：Makefile、build-root、CMake/Ninja 的设计说明。
- `docs/developer/build-run-debug/building.rst`：完整构建、打包与可复现构建流程。
- `docs/developer/build-run-debug/testing_vpp.rst`：测试框架与 `TEST=` 过滤语法。
- `docs/developer/build-run-debug/code_coverage.rst`：gcov/lcov 覆盖率流程；文档本身可用 `make docs` 构建。
