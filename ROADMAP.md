# HP HTTP Server 路线图

本文档记录 HP HTTP Server 的总体目标、版本路线、阶段边界和长期演进方向。它是 Leader 制定阶段设计、Builder 判断实现范围、Reviewer 判断是否越界的主要依据。

本文档不记录函数级、类级或文件级实现步骤。长期架构写入 `ARCHITECTURE.md`，阶段详细设计写入 `docs/leader/designs/Vx/Sx-design.md`，实现过程写入 Builder 报告，审查结果写入 Reviewer 报告，跨版本技术债写入 `TECH-DEBT-TRACKER.md`。

## 项目概览

HP HTTP Server 是一个面向高性能网络岗秋招展示的 Linux C++ HTTP/1.1 服务器项目。项目目标不是实现完整 Web 框架，而是围绕 Linux 系统编程和网络编程能力，逐步构建一个可运行、可压测、可解释、可扩展的高性能 HTTP Server。

项目最终希望展示以下能力：

- 使用 C++20 和 Linux socket API 构建服务器。
- 使用非阻塞 IO、`epoll` 和 Reactor 模型管理大量连接。
- 正确处理 TCP 连接生命周期、短读短写、半关闭、超时和错误事件。
- 实现 HTTP/1.1 请求解析、响应构造、静态资源服务和 keep-alive。
- 通过线程池、主从 Reactor、定时器和背压机制治理资源。
- 使用 `sendfile`、异步日志、Buffer 优化和压测分析支撑高性能叙事。
- 在 HTTP Server 内核稳定后，扩展轻量 L7 Reverse Proxy / Gateway 能力，减少普通 WebServer 项目的同质化。

当前项目处于准备阶段，优先完成根文档、路线规划和阶段设计，再进入代码实现。近期目标是先完成最小可运行 HTTP Server，再逐步演进为结构清晰、性能可验证、适合简历展示的网络服务器项目。

总体技术方向：

- 语言优先：C++20。
- 平台优先：Linux / WSL2，后续性能数据可在原生 Linux 环境复测。
- 构建优先：CMake，后续可使用 Ninja 或 Make 作为生成器。
- 核心能力自研：socket、epoll、Reactor、HTTP 解析、连接管理、Buffer、定时器、异步日志。
- 验证优先：单元测试、smoke test、wrk 压测、perf 或等价性能分析记录。

## 范围边界

### 长期覆盖范围

项目长期覆盖以下能力：

- HTTP/1.1 静态资源服务器。
- Reactor 网络事件框架。
- 非阻塞 TCP 连接管理。
- HTTP 请求解析状态机。
- keep-alive 与连接复用。
- 定时器与空闲连接超时。
- 主从 Reactor 与线程池。
- 输出 Buffer、高水位和慢连接保护。
- `sendfile` 零拷贝静态文件传输。
- 同步日志到异步日志的演进。
- 基础指标统计、访问日志和性能报告。
- 轻量 L7 Reverse Proxy / Gateway 扩展。

### 近期不做范围

近期版本不实现以下内容：

- HTTP/2。
- HTTPS/TLS。
- 完整 Web 框架或 Servlet 风格容器。
- 数据库、ORM、业务系统和复杂前端页面。
- 跨平台兼容。
- L4LB、NAT、IPVS、XDP、DPDK 或内核旁路能力。
- 基于 Boost.Asio、libevent、libev、muduo、workflow 等库替代核心网络模型。
- 完整生产级网关治理能力，例如复杂服务发现、动态配置中心、灰度发布和分布式限流。

### 不得随意删除或覆盖的内容

- `docs/leader/designs/` 下的阶段设计文档。
- `docs/builder/reports/` 下的实现报告。
- `docs/reviewer/reports/` 下的审查报告。
- `TECH-DEBT-TRACKER.md` 中已记录的技术债和关闭依据。
- `benchmark/` 中已经用于性能对比的压测命令、环境说明和结果记录。
- `CHANGELOG.md` 中已经记录的版本变化。

历史文档即使过时，也应通过追加新文档、补充说明或标记状态处理，不应直接删除来“清理历史”。

## 推进与完成规则

- `V0.1 / S3` 已完成，`V0.1` 三个阶段均已关闭。当前活动规划为 `V0.2 / S1 EventLoop 与 Channel`，状态`设计中 / Awaiting PM Decision`；Draft revision 1 未批准、未实现，批准前不启动 Builder。
- 阶段状态统一使用：`未开始`、`设计中`、`待实现`、`实现中`、`待审查`、`返工中`、`已完成`、`阻塞`。
- 阶段设计和审查计划必须处于 `Approved`，Builder 才能开始实现。
- 阶段只有在实现证据完整，且 Reviewer 给出 `PASS` 或允许关闭的 `PASS WITH DEBT` 后，才能标记为 `已完成`。
- 后续阶段以前一阶段 `已完成` 为前置条件；后续版本以前一版本全部阶段 `已完成` 为前置条件。
- 版本完成还要求 README、CHANGELOG、技术债和对应报告与实际行为一致。
- `V1.1` 是 `V1.0` 之后的可选扩展，不属于简历交付版的必需完成条件。

## 版本路线

### V0.1 最小可运行 HTTP Server

状态：`已完成`；`S1`、`S2`、`S3` 均已完成。

前置条件：

- 项目准备文档完成。
- `V0.1 / S1` 设计与审查计划已经批准。

目标：

构建项目最小可运行闭环。版本完成后，用户可以在 Linux / WSL2 中编译服务器，指定端口和静态资源目录，通过浏览器或 curl 访问静态文件，并看到基础 HTTP 错误响应。

核心能力：

- CMake 项目骨架和基本目录结构。
- TCP listen socket、bind、listen、accept。
- fd RAII 封装和非阻塞设置。
- 单线程 `epoll` LT 事件循环。
- 最小 HTTP 请求行和 Header 解析。
- `GET` 静态文件响应。
- 基础状态码：`200`、`400`、`403`、`404`、`405`、`500`。
- 输出缓冲处理非阻塞短写。
- 基础单元测试和 curl smoke test。

禁止范围：

- 不实现 keep-alive。
- 不实现线程池。
- 不实现主从 Reactor。
- 不实现完整 HTTP 状态机。
- 不实现 `sendfile`。
- 不实现异步日志。
- 不实现 L7 代理。
- 不做 wrk 性能达标承诺。

阶段划分：

- `S1 项目骨架与基础资源封装`（`已完成`）：建立 CMake、目录结构、基础类型、日志、socket RAII 和最小启动入口。设计文档：`docs/leader/designs/V0.1/S1-design.md`。
- `S2 单线程 epoll 与连接读写`（`已完成`）：实现单线程 epoll LT、非阻塞连接读写、临时 TCP echo、短写续传、半关闭和 fd 生命周期；Reviewer 复审 CTest `6/6`，唯一结论 `PASS`。设计文档：`docs/leader/designs/V0.1/S2-design.md`。
- `S3 最小 HTTP 静态文件服务`（`已完成`）：design/review revision 1 于 `2026-09-03` 获 PM 批准；Builder 按基线实现，Reviewer 在全新 `build-review-s3/` 中完成 CTest `9/9` 及 RV-01..10，唯一结论 `PASS`。已交付有界最小 HTTP 解析、fd-relative 路径安全、静态文件响应、错误码和真实 smoke test。设计文档：`docs/leader/designs/V0.1/S3-design.md`。

完成标准：

- 可以通过 CMake 完成配置和构建。
- 服务器可以监听指定端口并响应静态文件请求。
- curl 验证 `/`、存在文件、不存在文件、非法方法、路径穿越均有预期状态码。
- 单元测试和 smoke test 通过。
- `README.md` 包含最小构建、运行和验证命令。
- Builder 报告和 Reviewer 审查报告已生成。
- `V0.1 / S3` Reviewer 给出 `PASS` 或允许关闭的 `PASS WITH DEBT`。

相关文档：

- 设计文档：`docs/leader/designs/V0.1/`
- 实现报告：`docs/builder/reports/V0.1/`
- 审查报告：`docs/reviewer/reports/V0.1/`

### V0.2 Reactor 抽象重构

状态：进行中；`S1 设计中 / Awaiting PM Decision`，`S2/S3 未开始`。

前置条件：

- `V0.1` 已完成。

目标：

把 V0.1 中集中在服务器主循环里的 socket、epoll、连接、回调和缓冲职责拆分为清晰的 Reactor 组件。版本完成后，项目应具备可讲解的 `EventLoop`、`Channel`、`Acceptor`、`TcpConnection` 协作模型。

核心能力：

- `EventLoop` 封装单个事件循环。
- `Channel` 绑定 fd、关注事件和事件回调。
- `Acceptor` 专门负责监听 fd 和新连接接收。
- `TcpConnection` 管理单个连接的读写 Buffer、关闭状态和回调。
- 网络层与 HTTP 层解耦。
- 重构后保持 V0.1 用户可见行为不回退。

禁止范围：

- 不引入多线程。
- 不实现 keep-alive。
- 不引入复杂生命周期框架。
- 不改变 HTTP 功能范围。
- 不为了抽象完整性提前实现未来版本能力。

阶段划分：

- `S1 EventLoop 与 Channel`（`设计中 / Awaiting PM Decision`）：抽象事件循环和 fd 事件分发，保持单线程模型；Draft revision 1 尚未批准或实现。设计文档：`docs/leader/designs/V0.2/S1-design.md`。
- `S2 Acceptor 与 TcpConnection`（`未开始`）：拆分监听连接和普通连接生命周期，明确输入输出 Buffer 边界；不得提前进入 S1。设计文档：`docs/leader/designs/V0.2/S2-design.md`。
- `S3 HTTP 链路重接与回归验证`（`未开始`）：将 HTTP 处理接入新的连接回调模型，补充回归测试；不得提前进入 S1。设计文档：`docs/leader/designs/V0.2/S3-design.md`。

完成标准：

- `net` 模块不再由单个服务器类同时承担所有事件职责。
- HTTP 模块不依赖 `epoll` 或连接 fd。
- V0.1 smoke test 全部通过。
- 新增 Reactor 关键组件的单元或集成验证。
- 架构文档中对应模块边界仍然成立。
- Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.2/`
- 实现报告：`docs/builder/reports/V0.2/`
- 审查报告：`docs/reviewer/reports/V0.2/`

### V0.3 HTTP 状态机与连接复用

状态：计划中。

前置条件：

- `V0.2` 已完成。

目标：

让服务器从“读取一次请求并关闭连接”升级为支持 HTTP/1.1 请求边界和 keep-alive 的连接复用服务器。版本完成后，服务器能够在同一 TCP 连接上处理多个顺序请求，并正确处理半包、粘包、Header 上限和非法请求。

核心能力：

- HTTP 解析状态机。
- 请求行、Header、请求边界和错误状态管理。
- Header 总大小限制。
- keep-alive 与 `Connection: close` 语义。
- 同连接多请求顺序处理。
- 连接复用下的 Buffer 消费和响应队列策略。
- 更完整的 HTTP 错误响应。

禁止范围：

- 不实现 HTTP request body 大规模处理。
- 不实现 POST 业务接口。
- 不实现 chunked body。
- 不实现 pipelining 并发乱序响应。
- 不实现线程池和主从 Reactor。
- 不实现代理转发。

阶段划分：

- `S1 HTTP Parser 状态机`：建立请求解析状态机，支持半包、粘包和 Header 上限。设计文档：`docs/leader/designs/V0.3/S1-design.md`。
- `S2 Keep-Alive 连接复用`：实现连接复用、请求消费、响应后保活或关闭决策。设计文档：`docs/leader/designs/V0.3/S2-design.md`。
- `S3 协议边界与异常用例`：补充非法请求、超大 Header、异常关闭和回归测试。设计文档：`docs/leader/designs/V0.3/S3-design.md`。

完成标准：

- 单连接连续多个 GET 请求可以得到正确响应。
- 非法请求不会破坏连接状态或导致进程异常退出。
- Header 超限、方法不支持、路径非法均有明确行为。
- V0.1、V0.2 行为不回退。
- Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.3/`
- 实现报告：`docs/builder/reports/V0.3/`
- 审查报告：`docs/reviewer/reports/V0.3/`

### V0.4 并发模型与资源治理

状态：计划中。

前置条件：

- `V0.3` 已完成。

目标：

引入多线程并发模型和连接资源治理。版本完成后，服务器应具备主从 Reactor、事件循环线程池、定时器、空闲连接超时、优雅关闭和基础背压能力，可以解释高并发连接下如何避免资源失控。

核心能力：

- 主从 Reactor 模型。
- `EventLoopThread` 或等价事件循环线程封装。
- 固定大小的 `EventLoopThreadPool` 和跨线程投递边界。
- 定时器与空闲连接超时。
- 响应 `SIGINT`、`SIGTERM` 的优雅关闭流程。
- 输出 Buffer 高水位和慢连接保护。
- 跨线程任务投递和唤醒机制。

禁止范围：

- 不承诺极限性能指标。
- 不实现复杂动态扩缩容线程池。
- 不引入协程框架。
- 不实现代理转发。
- 不实现复杂限流系统。
- 不把业务处理和 IO 线程边界混在一起。

阶段划分：

- `S1 EventLoop 线程化`：建立事件循环线程封装和跨线程唤醒机制。设计文档：`docs/leader/designs/V0.4/S1-design.md`。
- `S2 主从 Reactor`：实现由 `EventLoopThreadPool` 持有 sub reactor，main reactor 接收连接并按固定策略分配。设计文档：`docs/leader/designs/V0.4/S2-design.md`。
- `S3 定时器与连接超时`：实现空闲连接超时、keep-alive 超时和定时清理。设计文档：`docs/leader/designs/V0.4/S3-design.md`。
- `S4 资源上限与优雅关闭`：加入输出高水位、跨线程任务上限、信号停止通知和关闭流程验证。设计文档：`docs/leader/designs/V0.4/S4-design.md`。

完成标准：

- 服务器可以使用多个事件循环线程处理连接。
- 空闲连接会按策略关闭。
- 慢连接不会无限占用输出缓冲内存。
- 关闭流程不会遗留 fd、悬空回调或重复关闭。
- 并发相关测试和 smoke test 通过。
- Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.4/`
- 实现报告：`docs/builder/reports/V0.4/`
- 审查报告：`docs/reviewer/reports/V0.4/`

### V0.5 性能优化与静态文件传输增强

状态：计划中。

前置条件：

- `V0.4` 已完成。

目标：

围绕静态文件服务性能建立优化闭环。版本完成后，服务器应支持 `sendfile` 零拷贝传输、异步日志、Buffer 优化和基础压测基线，让“高性能”具备可验证证据。

核心能力：

- `sendfile` 零拷贝静态文件传输。
- 文件 fd 生命周期管理。
- 大文件短写和续写处理。
- 异步日志或明确日志方案升级。
- Buffer 减少拷贝和内存占用治理。
- wrk 基础压测脚本和结果记录。
- 性能优化前后对比记录。

禁止范围：

- 不引入生产级 CDN 缓存系统。
- 不实现复杂 HTTP 缓存协商全集。
- 不为了压测数字牺牲协议正确性。
- 不用不可复现环境中的单次结果作为最终性能结论。
- 不将性能测试替代功能测试。

阶段划分：

- `S1 sendfile 文件传输`：实现静态文件零拷贝传输和短写续传。设计文档：`docs/leader/designs/V0.5/S1-design.md`。
- `S2 异步日志与 IO 路径减负`：实现或选定日志升级方案，避免热路径同步阻塞。设计文档：`docs/leader/designs/V0.5/S2-design.md`。
- `S3 Buffer 与背压优化`：优化输入输出 Buffer、高水位和慢连接策略。设计文档：`docs/leader/designs/V0.5/S3-design.md`。
- `S4 压测基线`：建立 wrk 压测脚本、环境记录和基础对比结果。设计文档：`docs/leader/designs/V0.5/S4-design.md`。

完成标准：

- 静态文件传输路径支持 `sendfile` 或有明确降级策略。
- 日志不会在高并发请求热路径上造成明显阻塞。
- wrk 压测命令、环境、参数和结果可复现。
- 性能优化不会破坏 V0.1 到 V0.4 的正确性测试。
- Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.5/`
- 实现报告：`docs/builder/reports/V0.5/`
- 审查报告：`docs/reviewer/reports/V0.5/`
- 压测记录：`benchmark/`

### V0.6 可观测性与性能分析

状态：计划中。

前置条件：

- `V0.5` 已完成。

目标：

补齐项目作为简历项目所需的可观测和性能分析材料。版本完成后，项目不仅能运行和压测，还能解释吞吐、延迟、错误、连接数、CPU 热点和优化方向。

核心能力：

- 结构化访问日志。
- 基础指标统计。
- 内部指标接口或指标导出文本。
- wrk 多场景压测报告。
- perf 或等价工具的热点分析记录。
- 不同线程数、连接数、文件大小下的结果对比。
- 性能结论与技术债记录。

禁止范围：

- 不搭建完整 Prometheus / Grafana 系统。
- 不为了展示而引入复杂监控平台。
- 不编造性能数据。
- 不把 WSL2 数据直接包装成生产服务器性能结论。
- 不在未记录环境的情况下比较性能结果。

阶段划分：

- `S1 指标统计与访问日志`：记录请求数、状态码、连接数、延迟和错误计数。设计文档：`docs/leader/designs/V0.6/S1-design.md`。
- `S2 压测场景矩阵`：整理小文件、大文件、短连接、keep-alive、多线程等压测场景。设计文档：`docs/leader/designs/V0.6/S2-design.md`。
- `S3 性能分析报告`：记录热点、瓶颈、优化结论和后续技术债。设计文档：`docs/leader/designs/V0.6/S3-design.md`。

完成标准：

- 每组压测结果包含环境、命令、参数、QPS、平均延迟、P95/P99 和错误率。
- 至少形成一份可放入 README 或简历讲解的性能结论。
- 可观测能力不破坏核心 IO 路径。
- 技术债和风险已同步到 `TECH-DEBT-TRACKER.md`。
- Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.6/`
- 实现报告：`docs/builder/reports/V0.6/`
- 审查报告：`docs/reviewer/reports/V0.6/`
- 压测记录：`benchmark/`

### V1.0 简历交付版

状态：计划中。

前置条件：

- `V0.6` 已完成。

目标：

将项目整理为可展示、可运行、可讲解的秋招简历项目。版本完成后，面试官可以通过 README 快速理解项目价值，通过架构文档深入查看设计，通过压测报告验证性能叙事。

核心能力：

- README 项目介绍、快速开始、核心亮点和压测摘要。
- 架构图或文字化模块关系说明。
- 完整构建、运行、测试、压测命令。
- 关键设计取舍说明。
- 已知限制和技术债说明。
- 简历表述建议和面试讲解主线。

禁止范围：

- 不在 V1.0 临时加入大功能。
- 不为了展示修改未经验证的性能数字。
- 不删除历史报告。
- 不把未完成能力写成已完成能力。

阶段划分：

- `S1 文档整理与入口完善`：整理 README、ARCHITECTURE、ROADMAP、CHANGELOG 和文档索引。设计文档：`docs/leader/designs/V1.0/S1-design.md`。
- `S2 展示材料与结果固化`：固化压测摘要、架构说明、简历亮点和面试问答主线。设计文档：`docs/leader/designs/V1.0/S2-design.md`。
- `S3 最终回归与发布检查`：执行完整测试、smoke test、关键压测和文档一致性审查。设计文档：`docs/leader/designs/V1.0/S3-design.md`。

完成标准：

- 新用户可以按 README 在干净环境中构建、运行和验证。
- README 明确展示项目亮点、技术栈、能力范围和压测摘要。
- 所有已完成版本的设计、报告和审查文档可追踪。
- `TECH-DEBT-TRACKER.md` 记录未完成能力和已接受风险。
- 最终 Reviewer 审查报告确认项目达到简历展示标准。

相关文档：

- 设计文档：`docs/leader/designs/V1.0/`
- 实现报告：`docs/builder/reports/V1.0/`
- 审查报告：`docs/reviewer/reports/V1.0/`

### V1.1 轻量 L7 Gateway 扩展

状态：计划中。

前置条件：

- `V1.0` 已完成。
- 用户明确批准继续扩展代理能力。

目标：

在 HTTP Server 内核稳定后，加入轻量 L7 Reverse Proxy / Gateway 能力。该版本用于降低项目同质化，让项目从“静态 HTTP Server”扩展为“具备应用层转发能力的高性能 HTTP 服务端内核”。

核心能力：

- 基于路径前缀或 Host 的路由规则。
- upstream 配置与选择。
- 简单负载均衡策略。
- upstream 连接建立、复用或明确关闭策略。
- 转发超时、失败响应和错误日志。
- 代理访问日志和基础指标。

禁止范围：

- 不实现 L4LB。
- 不实现服务发现系统。
- 不实现复杂动态配置中心。
- 不实现完整 API Gateway 治理能力。
- 不实现 TLS 终止。
- 不实现分布式限流、熔断和灰度发布全集。

阶段划分：

- `S1 代理配置与路由规则`：定义 upstream 和路由匹配边界。设计文档：`docs/leader/designs/V1.1/S1-design.md`。
- `S2 HTTP 转发链路`：实现请求转发、响应回传、超时和错误处理。设计文档：`docs/leader/designs/V1.1/S2-design.md`。
- `S3 代理压测与观测`：验证代理链路性能、日志和指标。设计文档：`docs/leader/designs/V1.1/S3-design.md`。

完成标准：

- 可以将指定路径请求转发到配置的 upstream。
- upstream 不可用、超时或响应异常时有明确错误响应。
- 代理能力复用现有 `net`、`http`、`timer`、`metrics` 模块边界。
- 静态文件服务能力不回退。
- 代理场景有 smoke test 和基础压测记录。
- Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V1.1/`
- 实现报告：`docs/builder/reports/V1.1/`
- 审查报告：`docs/reviewer/reports/V1.1/`
- 压测记录：`benchmark/`

## 阶段设计摘要

### V0.1 阶段摘要
- `S1 项目骨架与基础资源封装`（`已完成`）：产出可构建项目、基础 RAII 工具、socket 封装和最小启动入口；Reviewer 独立 CTest `3/3` 通过，结论为 `PASS`；涉及 `app`、`base`、`net`。
- `S2 单线程 epoll 与连接读写`（`已完成`）：已产出非阻塞监听、单线程 epoll LT、连接读写、输出缓冲和临时 echo；Reviewer 复审 `PASS`；涉及 `net`、`tests`。
- `S3 最小 HTTP 静态文件服务`（`已完成`）：已产出 GET 静态文件、错误响应、路径安全和 smoke test；Reviewer 独立 CTest `9/9`、RV-01..10 全部通过，唯一结论 `PASS`；涉及 `http`、`net`、`app`、`tests`。

### V0.2 阶段摘要

- `S1 EventLoop 与 Channel`（`设计中 / Awaiting PM Decision`）：计划产出生产路径实际使用的单线程事件循环和非 fd owner Channel；涉及 `net`、`tests`。
- `S2 Acceptor 与 TcpConnection`（`未开始`）：计划产出监听连接和普通连接生命周期抽象；涉及 `net`、`base`。
- `S3 HTTP 链路重接与回归验证`（`未开始`）：计划产出重构后的 HTTP 服务链路和回归测试；涉及 `net`、`http`、`tests`。

### V0.3 阶段摘要

- `S1 HTTP Parser 状态机`：产出可处理半包、粘包和 Header 上限的解析器；涉及 `http`。
- `S2 Keep-Alive 连接复用`：产出同连接多请求顺序处理能力；涉及 `http`、`net`。
- `S3 协议边界与异常用例`：产出异常输入测试和错误语义验证；涉及 `http`、`tests`。

### V0.4 阶段摘要

- `S1 EventLoop 线程化`：产出事件循环线程和跨线程唤醒机制；涉及 `net`、`base`.
- `S2 主从 Reactor`：产出由固定 `EventLoopThreadPool` 持有 sub reactor 的连接分发模型；涉及 `net`。
- `S3 定时器与连接超时`：产出空闲连接清理和超时策略；涉及 `timer`、`net`。
- `S4 资源上限与优雅关闭`：产出高水位、跨线程任务上限、信号停止通知和关闭流程验证；涉及 `net`、`base`。

### V0.5 阶段摘要

- `S1 sendfile 文件传输`：产出零拷贝静态文件路径；涉及 `http`、`net`。
- `S2 异步日志与 IO 路径减负`：产出日志升级方案；涉及 `base`。
- `S3 Buffer 与背压优化`：产出缓冲区和慢连接治理能力；涉及 `base`、`net`。
- `S4 压测基线`：产出 wrk 脚本和基础性能记录；涉及 `benchmark`。

### V0.6 阶段摘要

- `S1 指标统计与访问日志`：产出可观测基础数据；涉及 `metrics`、`base`、`http`。
- `S2 压测场景矩阵`：产出多场景压测命令和结果记录；涉及 `benchmark`。
- `S3 性能分析报告`：产出热点分析、瓶颈判断和技术债记录；涉及 `benchmark`、`docs`。

### V1.0 阶段摘要

- `S1 文档整理与入口完善`：产出完整 README 和文档索引；涉及 `docs`。
- `S2 展示材料与结果固化`：产出压测摘要、架构说明和简历讲解主线；涉及 `docs`、`benchmark`。
- `S3 最终回归与发布检查`：产出最终审查报告和发布检查结果；涉及 `tests`、`docs`。

### V1.1 阶段摘要

- `S1 代理配置与路由规则`：产出 upstream 和路由规则边界；涉及 `proxy`、`config`。
- `S2 HTTP 转发链路`：产出代理请求转发和响应回传；涉及 `proxy`、`http`、`net`。
- `S3 代理压测与观测`：产出代理场景测试和性能记录；涉及 `proxy`、`metrics`、`benchmark`。

## 长期演进方向

以下方向属于远期候选，不代表当前版本必须实现：

- 更完整的 L7 Gateway 能力，例如健康检查、连接池、基础熔断和限流。
- 更细的 HTTP 协议兼容能力，例如 Range、ETag、If-Modified-Since、chunked body。
- 在原生 Linux 环境中复测 WSL2 开发阶段的关键性能结论。
- 更完善的异步日志和日志落盘策略。
- 更系统的 perf、火焰图和系统调用热点分析。
- 可选部署能力，例如 systemd 服务文件、默认配置示例和运行目录约定。

进入更远期版本前，必须先满足以下条件：

- V1.0 已经形成可运行、可测试、可压测、可讲解的简历交付版。
- 已完成能力的测试和文档闭环稳定。
- `TECH-DEBT-TRACKER.md` 中不存在阻塞后续演进的高风险技术债。
- 用户明确希望继续扩展，而不是优先准备简历材料或面试讲解。

## 变更记录

- `2026-09-03`：形成 V0.2/S1 Draft revision 1，活动规划进入`设计中 / Awaiting PM Decision`；保持既有 S1/S2/S3 边界，批准前不实现，并修正 V0.1/S3 阶段摘要的状态漂移。
- `2026-09-03`：依据 S3 Builder 报告 001 与 Reviewer 报告 001 的唯一 `PASS`，将 S3 与 V0.1 标记为`已完成`；下一步由 Leader 设计 V0.2/S1，不直接实现。
- `2026-09-03`：PM 明确批准 S3 Draft revision 1；design/review 同步为 `Approved`，活动阶段推进到 `待实现 / Ready for Builder`，尚未开始实现。
- `2026-09-03`：形成 S3 Draft design/review revision 1 与 PM 决策包；活动阶段同步为 `设计中 / Awaiting PM Decision`，批准前不启动 Builder/Reviewer。
- `2026-09-01`：依据 Builder 报告 001/002 与 Reviewer 复审报告 002 的 `PASS`，将 S2 标记为 `已完成`；下一步由 Leader 设计 S3，S3 批准前不实现。
- `2026-08-27`：S2 Draft 设计与审查计划已形成；将活动阶段同步为 S2 `设计中` / `Awaiting PM Decision`，批准前不交给 Builder。
- `2026-08-25`：依据 S1 Builder 报告 002 和 Reviewer 报告 001 的 `PASS` 结论，将 S1 标记为已完成，并明确下一步由 Leader 设计 S2。
- `2026-08-24`：补充版本前置条件、统一阶段完成门槛，明确 `V0.4` 的事件循环线程池边界及 `V1.1` 的可选扩展属性。
- `2026-05-21`：初始化项目路线图，确定从最小 HTTP Server 到 Reactor、HTTP 状态机、并发治理、性能优化、可观测性、简历交付版和 L7 Gateway 的版本路线。
