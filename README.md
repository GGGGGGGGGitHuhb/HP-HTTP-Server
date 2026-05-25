# HP HTTP Server

HP HTTP Server 是一个面向高性能网络岗秋招展示的 Linux C++ HTTP/1.1 服务器项目。项目目标是从零构建一个可运行、可压测、可解释、可扩展的高性能 HTTP Server，用于展示 Linux 系统编程、网络编程、Reactor 模型、连接管理、协议解析、资源治理和性能分析能力。

当前仓库处于项目准备阶段，已完成核心文档规划，尚未进入代码实现。后续实现会严格按照 `ROADMAP.md` 和阶段设计文档推进。

## 当前状态

- 当前阶段：准备阶段。
- 当前状态：文档规划中，暂不可运行。
- 最近完成：`ARCHITECTURE.md`、`ROADMAP.md`、`TECH-DEBT-TRACKER.md` 初始化。
- 下一阶段：`V0.1 / S1 项目骨架与基础资源封装`。

当前已明确的项目方向：

- 使用 C++20 实现。
- 面向 Linux / WSL2 环境。
- 核心网络模型基于 Linux socket、非阻塞 IO、`epoll` 和 Reactor。
- 优先实现 HTTP/1.1 静态资源服务，再逐步加入 keep-alive、主从 Reactor、线程池、定时器、`sendfile`、异步日志、压测分析和轻量 L7 Gateway。

## 功能概览

当前仓库尚未提供可运行服务器。规划中的核心能力包括：

- HTTP/1.1 静态资源服务器。
- 非阻塞 TCP 连接管理。
- 单线程 Reactor 到主从 Reactor 的演进。
- HTTP 请求解析状态机和 keep-alive。
- 定时器、空闲连接超时和慢连接保护。
- `sendfile` 零拷贝静态文件传输。
- 异步日志、基础指标和 wrk/perf 性能分析。
- 轻量 L7 Reverse Proxy / Gateway 扩展。

具体版本顺序和阶段边界以 `ROADMAP.md` 为准。

## 环境要求

当前准备阶段只需要文本编辑和 Git。进入代码实现后，推荐环境如下：

- 操作系统：Linux 或 WSL2。
- 编译器：支持 C++20 的 `clang++` 或 `g++`。
- 构建工具：CMake。
- 可选生成器：Ninja 或 Make。
- 功能验证工具：curl。
- 后续压测工具：wrk。
- 后续性能分析工具：perf 或等价工具。

项目核心运行时不依赖数据库、外部服务、Web 框架或大型网络库。

## 快速开始

当前仓库尚未进入 `V0.1` 代码实现阶段，因此暂时没有可执行程序。

运行目录：

```bash
/home/power/projects/HP-HTTP-Server
```

当前建议先阅读项目文档：

```bash
sed -n '1,200p' README.md
sed -n '1,220p' ROADMAP.md
sed -n '1,220p' ARCHITECTURE.md
sed -n '1,220p' TECH-DEBT-TRACKER.md
```

进入 `V0.1` 后，README 会补充实际构建、运行和测试命令。

## 常用命令

当前阶段可用命令：

```bash
git status --short
```

后续 `V0.1` 预计使用的命令形式如下，具体以实现后的 README 更新为准：

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/hp_http_server --port 8080 --root public
```

在这些命令真正可用前，Builder 不应在报告中声称项目已经可以构建或运行。

## 配置说明

当前尚未实现配置系统。

规划中的配置入口包括：

- 命令行参数：监听端口、静态资源根目录、线程数、日志级别等。
- 配置文件：后续阶段根据需要引入。
- 默认值：应集中定义，避免散落在多个模块中。

配置格式、默认值和覆盖方式会在对应阶段设计和 README 更新中明确。

## 项目结构

当前仓库主要是文档骨架：

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

规划中的代码目录包括：

```text
.
├── app/          # 可执行程序入口
├── include/      # 公开头文件
├── src/          # 实现文件
├── tests/        # 单元测试、集成测试和 smoke test
├── benchmark/    # wrk、perf 和性能报告
├── public/       # 静态资源示例
└── config/       # 配置文件示例
```

实际目录创建以阶段设计和 Builder 实现为准。

## 测试与验证

当前尚无代码测试可运行。

后续测试体系规划如下：

- 单元测试：HTTP 解析、响应构造、路径解析、Buffer、定时器等纯逻辑。
- 集成测试：启动服务器并通过真实 TCP/HTTP 链路访问。
- smoke test：覆盖 `/`、存在文件、不存在文件、非法方法、路径穿越等最小主流程。
- 压测：使用 wrk 记录 QPS、平均延迟、P95/P99、错误率和环境信息。
- 性能分析：使用 perf 或等价工具记录热点和优化方向。

测试命令会在对应功能实现后补充。

## 文档索引

- `AGENTS.md`：仓库级 Agent 工作规则。
- `ARCHITECTURE.md`：长期架构、模块职责、数据流和依赖边界。
- `ROADMAP.md`：版本路线、阶段范围、禁止范围和完成标准。
- `TECH-DEBT-TRACKER.md`：跨版本技术债、风险、延期事项和阶段检查点。
- `CHANGELOG.md`：已完成版本变化记录。
- `docs/references/`：各类文档撰写规范。
- `docs/leader/GUIDE.md`：Leader 工作指南。
- `docs/builder/GUIDE.md`：Builder 工作指南。
- `docs/reviewer/GUIDE.md`：Reviewer 工作指南。
- `docs/leader/designs/`：阶段详细设计文档。
- `docs/builder/reports/`：Builder 实现报告。
- `docs/reviewer/reports/`：Reviewer 审查报告。

## 开发流程

项目采用文档驱动的阶段推进方式：

1. Leader 根据 `ROADMAP.md` 和 `ARCHITECTURE.md` 编写阶段设计。
2. Builder 只按当前阶段设计实现，不主动扩大范围。
3. Builder 完成后运行约定测试并生成实现报告。
4. Reviewer 根据阶段设计、架构约束和测试结果独立审查。
5. Leader 根据 Builder 报告和 Reviewer 报告更新技术债、变更记录和后续计划。

任何阶段如果改变运行方式、测试命令、项目结构或用户可见能力，都应同步更新 README。

## 已知限制

- 当前尚无可运行代码。
- 当前没有构建命令、测试命令或压测数据。
- 当前性能目标尚未量化。
- 当前只承诺 Linux / WSL2 方向，不承诺跨平台。
- HTTP/2、HTTPS/TLS、完整 Web 框架、数据库 ORM、复杂前端、L4LB、XDP、DPDK 均不属于近期范围。
- WSL2 可用于开发和初步验证，但最终性能结论需要记录环境，必要时在原生 Linux 复测。

更详细的风险和技术债见 `TECH-DEBT-TRACKER.md`。

## 许可证

暂未指定。
