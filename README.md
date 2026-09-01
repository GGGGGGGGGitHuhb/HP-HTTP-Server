# HP HTTP Server

HP HTTP Server 是一个面向高性能网络岗学习与简历展示的 Linux C++20 HTTP/1.1 服务器项目。项目将从最小可运行服务器逐步演进到 Reactor、连接复用、并发资源治理、性能优化和可观测性；轻量 L7 Gateway 是 V1.0 之后的可选扩展。

## 当前状态

- 当前版本：`V0.1 最小可运行 HTTP Server`。
- 最近完成阶段：`S1 项目骨架与基础资源封装`。
- 阶段状态：`已完成`。
- 已批准基线：
  - `docs/leader/designs/V0.1/S1-design.md`
  - `docs/reviewer/reviews/V0.1/S1-review.md`
- 当前已实现 CMake/C++20 构建、同步日志、Socket fd RAII、非阻塞与地址复用设置，以及最小 CLI；程序不监听端口，也不提供 HTTP 服务。
- Reviewer 已在全新的 `build-review/` 中独立配置、构建并完成 RV-01 至 RV-07，CTest `3/3` 通过，唯一结论为 `PASS`。
- 验收报告：`docs/reviewer/reports/V0.1/S1-report-001.md`。
- 下一步：由 Leader 设计 `V0.1 / S2 单线程 epoll 与连接读写`；S2 设计批准前不开始实现。

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

S1 基线：

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
- wrk、perf：当前未安装；它们分别到 V0.5、V0.6 才成为阶段工具，因此不阻塞 S1

项目核心运行时不依赖数据库、外部服务、Web 框架或替代核心网络模型的大型网络库。

## 快速开始

Shell：Bash

工作目录：

```text
/home/power/projects/HP-HTTP-Server
```

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/hp_http_server
```

预期稳定信号：

```text
100% tests passed, 0 tests failed out of 3
[INFO] HP HTTP Server V0.1 / S1 skeleton
[INFO] This stage does not provide HTTP service.
```

三种 CLI 验证命令：

```bash
./build/hp_http_server --help
./build/hp_http_server
./build/hp_http_server --unknown-option
```

`--help` 和无参数运行退出 `0`；未知参数输出错误与用法，并以非零状态退出。

## 配置说明

当前未实现配置系统，也不接受端口、静态目录或线程数等运行参数。

按路线图逐步引入：

- S1：只提供 `--help`、无参数骨架输出和未知参数错误。
- V0.1 后续阶段：监听端口和静态资源根目录。
- 后续版本：线程数、连接上限、超时和日志级别。

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

S1 已实现代码目录：

```text
app/
include/base/
include/net/
src/base/
src/net/
tests/
CMakeLists.txt
```

- `app/`：S1 命令行入口和顶层异常转换。
- `include/base/`、`src/base/`：不可拷贝约束和同步日志。
- `include/net/`、`src/net/`：Socket fd 所有权与基础设置操作。
- `tests/`：base、Socket 真实 fd 和 CLI 进程测试。
- `docs/`：阶段设计、审查计划和三角色工作证据。

## 测试与验证

S1 的可复现验证命令为：

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Builder 自测为 `3/3` CTest 通过。Reviewer 随后使用全新的 `build-review/` 独立配置和构建，复跑 RV-01 至 RV-07，CTest 同样为 `3/3` 通过，并确认：

- 同步日志调用、不可拷贝约束和 Socket 真实 fd 所有权语义符合设计。
- `release/reset`、非阻塞与地址复用的成功/失败路径通过。
- 三种 CLI 行为和 S1 禁止范围符合 Approved 基线。

HTTP smoke test、wrk 压测和 perf 分析属于后续阶段，当前没有相关结果或性能数字。

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

- 当前可执行程序只是 S1 骨架，不监听端口、不提供 HTTP 服务，也没有性能数据。
- 只承诺 Linux / WSL2 方向，不承诺跨平台。
- WSL2 可用于开发和初步验证；最终性能结论必须记录环境，必要时在原生 Linux 复测。
- HTTP/2、HTTPS/TLS、完整 Web 框架、数据库 ORM、L4LB、XDP 和 DPDK 不属于当前路线。
- V1.1 L7 Gateway 是可选扩展，不影响 V1.0 简历交付版完成。

## 许可证

仓库当前未包含 `LICENSE` 文件，尚未授予明确的开源复用许可。
