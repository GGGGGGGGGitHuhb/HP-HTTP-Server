# HP HTTP Server

HP HTTP Server 是一个面向高性能网络岗学习与简历展示的 Linux C++20 HTTP/1.1 服务器项目。项目将从最小可运行服务器逐步演进到 Reactor、连接复用、并发资源治理、性能优化和可观测性；轻量 L7 Gateway 是 V1.0 之后的可选扩展。

## 当前状态

- 当前版本：`V0.1 最小可运行 HTTP Server`。
- 最近完成阶段：`V0.1 / S2 单线程 epoll 与连接读写`；Reviewer 复审唯一结论为 `PASS`，阶段状态为 `已完成`。
- S2 已实现显式 `--port` CLI、端口 0 实际端口报告、非阻塞 TCP listener、单线程单 epoll LT、accept drain、连接表、原始字节 echo、输出缓冲与短写续传、半关闭排空、连接错误隔离和 fd RAII 清理。
- Reviewer 在全新的 `build-review-s2-r2/` 中独立完成 Debug 配置、构建和 RV-01 至 RV-08：CTest `6/6`、P2-01 专项 `100/100`、全量稳定性 `60/60` 均通过；首轮 P2-01 已关闭，P0/P1/P2、无法验证项和新增技术债均无。
- 当前运行入口仍是临时 `V0.1 / S2 TCP echo` 验证接口，不解析 HTTP，也不提供 HTTP 响应、静态文件、生产安全或性能承诺。
- 下一步：由 Leader 设计 `V0.1 / S3 最小 HTTP 静态文件服务`；S3 尚无 Approved 基线，不得直接实现。
- S2 已批准基线：
  - `docs/leader/designs/V0.1/S2-design.md`
  - `docs/reviewer/reviews/V0.1/S2-review.md`
- Builder 报告：
  - `docs/builder/reports/V0.1/S2-report-001.md`
  - `docs/builder/reports/V0.1/S2-report-002.md`
- Reviewer 报告：
  - `docs/reviewer/reports/V0.1/S2-report-001.md`（历史 `FAIL`）
  - `docs/reviewer/reports/V0.1/S2-report-002.md`（最终 `PASS`）

规划能力与当前已实现能力必须区分。具体版本顺序、完成门槛和禁止范围以 `ROADMAP.md` 为准。

## 技术主线

项目计划覆盖：

- C++20、面向对象、RAII、智能指针和 STL。
- Linux fd、系统调用、文件 IO、信号和线程。
- TCP/IP、Socket、非阻塞 IO、epoll LT/ET 和连接生命周期。
- Reactor、事件循环线程池、定时器、Buffer、日志与资源治理。
- HTTP/1.1 请求解析、状态码、Header、Keep-Alive 和静态资源响应。
- wrk 压测、延迟与错误率记录、perf 热点分析。

以上是路线图目标，不表示当前已经实现。

## 环境要求

S2 基线：

- Linux 或 WSL2。
- CMake 3.20 或更高。
- Clang 14 或更高，或者 GCC 11 或更高。
- C++20。
- Ninja 推荐但非强制。
- CTest。
- curl 在后续 HTTP smoke test 中使用。

当前工作环境已于 `2026-08-24` 核对：

- CMake / CTest：`3.28.3`
- Clang：`18.1.3`
- GCC：`13.3.0`
- Ninja：`1.11.1`
- curl：`8.5.0`
- wrk、perf：当前未安装；它们分别到 V0.5、V0.6 才成为阶段工具，因此不阻塞 S2

项目核心运行时不依赖数据库、外部服务、Web 框架或替代核心网络模型的大型网络库。

## 快速开始

Shell：Bash

工作目录：

```text
/home/power/projects/HP-HTTP-Server
```

```bash
cmake -S . -B build-s2 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-s2 --verbose
ctest --test-dir build-s2 --output-on-failure
./build-s2/hp_http_server --help
./build-s2/hp_http_server --port 8080
```

预期稳定信号：

```text
100% tests passed, 0 tests failed out of 6
[INFO] HP HTTP Server V0.1 / S2 TCP echo
[INFO] Listening on TCP port 8080.
[INFO] This temporary echo endpoint does not provide HTTP service.
V0.1 / S2 TCP echo listening on port 8080; HTTP service is not available.
```

CLI 验证命令：

```bash
./build-s2/hp_http_server --help
./build-s2/hp_http_server
./build-s2/hp_http_server --port invalid
```

`--help` 退出 `0`；无参数、未知参数、重复参数、缺失值、非十进制值和超范围端口均输出简短用法并以非零状态退出。`--port 0` 允许内核选择临时端口，启动输出会给出实际非零端口。

## 配置说明

当前没有配置文件系统，只接受一个必需的 `--port <0-65535>` 参数。

当前行为：

- `--port 8080`：监听显式端口。
- `--port 0`：由内核分配临时端口，并在启动输出中报告实际端口。
- 当前不接受静态目录、线程数、连接上限、超时或日志级别参数。

每个参数只有在实现、测试和 README 命令同时完成后，才视为可用接口。

## 项目结构

当前仓库保留规划与阶段文档，并已建立 S1 可构建代码：

```text
.
├── AGENTS.md
├── ARCHITECTURE.md
├── CHANGELOG.md
├── README.md
├── ROADMAP.md
├── TECH-DEBT-TRACKER.md
└── docs/
    ├── builder/
    ├── leader/
    ├── references/
    └── reviewer/
```

S2 已实现代码目录：

```text
app/
include/base/
include/net/
src/base/
src/net/
tests/
CMakeLists.txt
```

- `app/`：S2 严格 CLI、服务器启动和顶层异常转换。
- `include/base/`、`src/base/`：不可拷贝约束和同步日志。
- `include/net/`、`src/net/`：Socket fd RAII、Epoller RAII、集中式单线程 TCP echo 服务器和连接 IO 状态。
- `tests/`：S1 回归、网络原语、受控短写/EAGAIN、EINTR、CLI 和真实 loopback 集成测试。
- `docs/`：阶段设计、审查计划和三角色工作证据。

## 测试与验证

S2 的可复现验证命令为：

```bash
cmake -S . -B build-s2 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-s2 --verbose
ctest --test-dir build-s2 --output-on-failure
```

Builder 与 Reviewer 已验证 6 项 CTest：

- `base_tests`、`socket_tests`：保留 S1 基础能力与 fd 所有权回归。
- `network_primitives_tests`：覆盖 epoll LT、add/modify/remove、EINTR、listener/connection close-on-exec、accept drain、注册失败、组合 IN/RDHUP 和 fd 清理。
- `connection_io_tests`：覆盖二进制跨块读取、受控短写/EAGAIN、读写 EINTR、输出游标、半关闭、SIGPIPE，以及真实 RST 下同批 `EPOLLIN|EPOLLERR`、`SO_ERROR=ECONNRESET`、先诊断后读取和同批字节不丢失。
- `cli_tests`：覆盖帮助、严格参数校验和受控 bind 失败。
- `server_integration_tests`：启动真实 `--port 0` 子进程，覆盖文本/二进制/大数据、多连接、半关闭排空、reset 隔离和重复连接。

Reviewer 已在全新的 `build-review-s2-r2/` 中独立复审 P2-01 并全量回归 RV-01 至 RV-08，CTest `6/6`、专项 `100/100`、全量稳定性 `60/60` 均通过，唯一结论为 `PASS`。HTTP smoke test、wrk 压测和 perf 分析属于后续阶段，当前没有相关结果或性能数字。

## 文档索引

- `AGENTS.md`：仓库级 Agent 规则、文档权威和完成门槛。
- `ARCHITECTURE.md`：当前态与长期目标架构、模块职责、数据流和依赖边界。
- `ROADMAP.md`：版本路线、前置条件、阶段范围和完成标准。
- `TECH-DEBT-TRACKER.md`：跨阶段技术债、批准延期和持续风险。
- `CHANGELOG.md`：已完成并得到适当验证的变化。
- `docs/leader/GUIDE.md`：Leader 工作指南。
- `docs/builder/GUIDE.md`：Builder 工作指南。
- `docs/reviewer/GUIDE.md`：Reviewer 工作指南。
- `docs/leader/designs/`：阶段设计。
- `docs/leader/reworks/`：批准后的返工设计。
- `docs/reviewer/reviews/`：审查计划。
- `docs/leader/reports/`、`docs/builder/reports/`、`docs/reviewer/reports/`：历史工作证据。

`docs/` 是本项目的本地协作资料目录；是否纳入版本控制或远程发布不属于 Agent 默认工作范围。

## 开发流程

1. Leader 形成 Draft 设计和审查计划。
2. 用户批准后，文档标记为 `Approved`。
3. Builder 只按最新 Approved 基线实现、测试并创建新报告。
4. Reviewer 按 REQ/AC/RV 独立验证并给出 `PASS`、`PASS WITH DEBT`、`FAIL` 或 `BLOCKED`。
5. 只有允许关闭的 Reviewer 结论出现后，阶段才更新为 `已完成`。
6. Leader 同步路线图、README、CHANGELOG 和技术债，再准备下一阶段。

## 已知限制

- 当前 TCP echo 只是 S2 网络闭环的临时验证接口，不是 HTTP 服务，也不是长期协议。
- 当前不解析 HTTP，不提供静态文件、keep-alive、线程池、定时器、优雅关闭或性能数据。
- 当前输出缓冲没有高水位或慢连接保护，只适用于开发和阶段验收，不作生产安全或规模承诺；该治理按路线图留到 V0.4。
- 只承诺 Linux / WSL2 方向，不承诺跨平台。
- WSL2 可用于开发和初步验证；最终性能结论必须记录环境，必要时在原生 Linux 复测。
- HTTP/2、HTTPS/TLS、完整 Web 框架、数据库 ORM、L4LB、XDP 和 DPDK 不属于当前路线。
- V1.1 L7 Gateway 是可选扩展，不影响 V1.0 简历交付版完成。

## 许可证

仓库当前未包含 `LICENSE` 文件，尚未授予明确的开源复用许可。
