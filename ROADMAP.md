# HP HTTP Server 路线图

## 当前状态

V1.0主线阶段验收已完成，待用户合并发布；S1/S2独立静态PASS，S3 Builder完整通过及Reviewer006独立PASS，原失败/费用完整保留。V0.6已完成并发布。V0.5.1搁置未完成：S1/S2局部证据保留，S3终止/FAIL、S4停工，S5/S6未开始，不恢复长尾实验。管理停工不能替代验收，RO-002保持开放，历史S3审计EOF根因仍未知；V1.1是可选后续，不自动启动。

## 范围边界

本项目主线为Linux C++ HTTP Server：网络事件、协议、资源生命周期、观测与条件化测量。当前不含HTTP/2/TLS/数据库/Web框架/代理。V1.1仅是可选HTTP层扩展；L4LB、XDP、DPDK另立项目，不混入本仓库。保留include/src布局，不为了展示迁移源码或制造性能数字。

## 推进与完成规则

每阶段设计Approved→Builder实施→Reviewer独立结论→Leader收口；只有PASS或允许关闭的PASS WITH DEBT可完成。后续阶段按前置条件逐项启动，不由当前文档整理自动授权实验或功能。历史报告/raw不删除，公开读者可用结果与说明不依赖本地ignored docs。

## 已交付版本

| 版本 | 交付与边界 |
| --- | --- |
| V0.1 | 最小静态HTTP服务、严格CLI及基础验证 |
| V0.2 | Reactor抽象，事件循环/Channel职责及连接生命周期 |
| V0.3 | 增量HTTP解析、受限framing、Keep-Alive与顺序pipeline |
| V0.4 | 主从Reactor、定时器、连接超时、资源边界及优雅关闭 |
| V0.5 | sendfile、异步日志、Buffer与固定基准；验收不证明性能改善 |
| V0.5.1 | 搁置未完成；TCP_NODELAY局部修复有效，原高并发长尾验收未通过 |
| V0.6 | S1指标/访问记录；S2固定6×3矩阵；S3按Approved R002有限三样本完成，无M6 syscall、函数profile或长尾根因结论 |

详细历史阶段、前置条件及原审批记录见 [旧路线快照](history/documentation/V1.0-S1/ROADMAP.md#版本路线)，变化与风险分别见 [CHANGELOG](CHANGELOG.md)、[技术债](TECH-DEBT-TRACKER.md)。

## 版本路线

### V1.0 简历交付版

状态：主线阶段验收已完成，待用户合并发布；S1 `已完成`（2026-10-09限定文档整理，独立静态PASS）；S2 `已完成`（2026-10-09展示材料独立静态PASS）；S3 `已完成`（2026-10-09 Builder003完整通过及Reviewer006独立PASS，原失败及费用保持）。

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

- `S1 文档整理与入口完善`（`已完成`）：整理 README、ARCHITECTURE、ROADMAP、CHANGELOG 和文档索引；保留现有include/src布局，不改代码或执行最终回归。设计文档：`docs/leader/designs/V1.0/S1-design.md`。
- `S2 展示材料与结果固化`（`已完成`）：固化压测摘要、架构说明、简历亮点和面试问答主线。设计文档：`docs/leader/designs/V1.0/S2-design.md`。
- `S3 最终回归与发布检查`（`已完成`）：Debug/Release各14/14、四smoke/CLI/信号/checker、五sanitizer及九正式样本、公开文档/保护文件后验独立通过。设计文档：`docs/leader/designs/V1.0/S3-design.md`；批准返工R001–R005、Builder003及Reviewer006可追踪，历史失败不覆写。公开证据见[发布检查](documentation/RELEASE-CHECK.md)。

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


本页中的 `docs/` 路径为本地协作编号，不随Git分发，公开使用不以其存在为前提。阶段具体设计仅在相应阶段实际批准后形成实施权威。

<details>
<summary>历史锚点导航</summary>

旧标题对应迁移前历史时点，不作为当前能力或执行授权。

<a id="当前停工状态"></a>
- [当前停工状态](history/documentation/V1.0-S1/ROADMAP.md#当前停工状态)
<a id="当前推进方向"></a>
- [当前推进方向](history/documentation/V1.0-S1/ROADMAP.md#当前推进方向)
<a id="当前独立重构进度"></a>
- [当前独立重构进度](history/documentation/V1.0-S1/ROADMAP.md#当前独立重构进度)
<a id="项目概览"></a>
- [项目概览](history/documentation/V1.0-S1/ROADMAP.md#项目概览)
<a id="长期覆盖范围"></a>
- [长期覆盖范围](history/documentation/V1.0-S1/ROADMAP.md#长期覆盖范围)
<a id="近期不做范围"></a>
- [近期不做范围](history/documentation/V1.0-S1/ROADMAP.md#近期不做范围)
<a id="不得随意删除或覆盖的内容"></a>
- [不得随意删除或覆盖的内容](history/documentation/V1.0-S1/ROADMAP.md#不得随意删除或覆盖的内容)
<a id="v01-最小可运行-http-server"></a>
- [V0.1 最小可运行 HTTP Server](history/documentation/V1.0-S1/ROADMAP.md#v01-最小可运行-http-server)
<a id="v02-reactor-抽象重构"></a>
- [V0.2 Reactor 抽象重构](history/documentation/V1.0-S1/ROADMAP.md#v02-reactor-抽象重构)
<a id="v03-http-状态机与连接复用"></a>
- [V0.3 HTTP 状态机与连接复用](history/documentation/V1.0-S1/ROADMAP.md#v03-http-状态机与连接复用)
<a id="v04-并发模型与资源治理"></a>
- [V0.4 并发模型与资源治理](history/documentation/V1.0-S1/ROADMAP.md#v04-并发模型与资源治理)
<a id="v05-性能优化与静态文件传输增强"></a>
- [V0.5 性能优化与静态文件传输增强](history/documentation/V1.0-S1/ROADMAP.md#v05-性能优化与静态文件传输增强)
<a id="v051-性能回退诊断与修复"></a>
- [V0.5.1 性能回退诊断与修复](history/documentation/V1.0-S1/ROADMAP.md#v051-性能回退诊断与修复)
<a id="v06-可观测性与性能分析"></a>
- [V0.6 可观测性与性能分析](history/documentation/V1.0-S1/ROADMAP.md#v06-可观测性与性能分析)
<a id="阶段设计摘要"></a>
- [阶段设计摘要](history/documentation/V1.0-S1/ROADMAP.md#阶段设计摘要)
<a id="v01-阶段摘要"></a>
- [V0.1 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v01-阶段摘要)
<a id="v02-阶段摘要"></a>
- [V0.2 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v02-阶段摘要)
<a id="v03-阶段摘要"></a>
- [V0.3 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v03-阶段摘要)
<a id="v04-阶段摘要"></a>
- [V0.4 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v04-阶段摘要)
<a id="v05-阶段摘要"></a>
- [V0.5 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v05-阶段摘要)
<a id="v051-阶段摘要"></a>
- [V0.5.1 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v051-阶段摘要)
<a id="v06-阶段摘要"></a>
- [V0.6 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v06-阶段摘要)
<a id="v10-阶段摘要"></a>
- [V1.0 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v10-阶段摘要)
<a id="v11-阶段摘要"></a>
- [V1.1 阶段摘要](history/documentation/V1.0-S1/ROADMAP.md#v11-阶段摘要)
<a id="变更记录"></a>
- [变更记录](history/documentation/V1.0-S1/ROADMAP.md#变更记录)

</details>
