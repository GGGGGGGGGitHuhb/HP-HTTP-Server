# HP HTTP Server 路线图

## 当前状态

V1.0主线阶段验收已完成，待用户合并发布；S1/S2独立静态PASS，S3 Builder完整通过及Reviewer006独立PASS，原失败/费用完整保留。V0.6已完成并发布。V0.5.1搁置未完成：S1/S2局部证据保留，S3终止/FAIL、S4停工，S5/S6未开始，不恢复长尾实验。管理停工不能替代验收，RO-002保持开放，历史S3审计EOF根因仍未知；后续V1.1–V1.4改为服务器完善路线，V1.1/S1已完成独立验收并合并，S2已完成独立验收，S3已完成独立验收，V1.1验收完成且S3已合并（标签发布未核实），V1.2/S1/S2已完成独立验收且S2已合并；S3已完成（Approved revision1、Reviewer002 PASS、Leader004收口），V1.2阶段验收全部完成，S3已合并origin/main（PR40），标签发布未核实。

发布状态补充：2026-10-29已fetch核实origin/main为cc4058c，包含v1.0/v1.0-s3标签所指c0eb380；与Leader S3-report-014一致。上段“待合并发布”为阶段提交时旧快照。V1.1/S1从当前origin/main建立开发分支，现有工作区改动不视为已验收版本。

## 范围边界

本项目主线为Linux C++ HTTP Server：网络事件、协议、资源生命周期、观测与条件化测量。当前不含HTTP/2/TLS/数据库/Web框架/代理。后续聚焦HTTP正文、路由、动态处理与运行保障，数据库暂不纳入；L7 Gateway、L4LB、XDP、DPDK另立项目，不混入本仓库。保留include/src布局，不为了展示迁移源码或制造性能数字。

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

### 后续路线与共同门槛

2026-10-29 用户明确要求取消本仓库网关版本，暂不加入数据库，把 Server 做得更扎实完整。以下为本次制定的版本规划；V1.1/S1已于2026-10-29按Approved revision 1完成，Builder001及Reviewer001 PASS、Leader003收口；S2于2026-10-29设计/审查revision 1及两文件冻结例外获批，按Builder001及Reviewer001独立PASS完成，Leader003收口，状态 `已完成`，S3现为 `已完成`（Approved revision 1、Reviewer001 PASS、Leader003收口），V1.1验收完成且S3已合并（标签未核实），V1.2/S1已独立验收，V1.2/S2 `已完成`（Approved revision1及补充、Reviewer001 PASS、Leader005收口），S3为`已完成`（Approved revision1、Reviewer002 PASS、Leader004收口），其后阶段`未开始`。后续按阶段形成设计、审查计划及批准记录，不一次展开所有实现细节。

- 顺序：V1.1 → V1.2 → V1.3 → V1.4。每阶段依赖前一阶段完成，各版本依赖前一版本验收；V1.1启动前核实V1.0发布基线与现有工作区改动归属。
- 每阶段必须给出支持/拒绝行为矩阵、当前接口测试、真实HTTP集成验证及静态服务回归；涉及生命周期、跨线程或资源变化时增加相应专项验证。
- TD-006按受影响模块补足当前接口覆盖；冻结旧测试是否迁移须在阶段设计中明确，不批量改写受保护历史文件。测试总数不替代覆盖说明。
- 新正文、任务队列、动态数据和连接均须在引入时有上限及失败释放路径；不能把基本资源安全推迟到V1.4。
- 每阶段只有独立Reviewer给出PASS或允许关闭的PASS WITH DEBT后才可标为已完成；同步README、架构及已验证CHANGELOG。性能测量须单独定义环境与目标，不恢复V0.5.1旧实验。
- 阶段设计及审查计划在对应 `docs/leader/designs/Vx/Sx-design.md`、`docs/reviewer/reviews/Vx/Sx-review.md` 创建；这些是未来路径约定，当前未生成文件不构成实施基线。

### V1.1 请求正文与基础动态处理

状态：`已完成`（S1–S3均已独立验收，S3 Reviewer001 PASS、Leader003收口；版本验收完成不表示已经合并发布）。目标：接收有界非空请求正文，按方法和路径处理请求，并正确维持连接消息边界。

- **S1 协议与回归基线**（`已完成`，2026-10-29，Reviewer001 PASS、Leader003收口）：盘点当前解析、保活、输出及错误路径，建立现有接口支持矩阵和关键回归覆盖。完成标准：静态GET、错误请求、顺序pipeline与连接关闭行为有可执行证据，明确TD-006本轮覆盖与保留缺口。已交付当前接口parser/真实HTTP两套测试及[支持矩阵](documentation/HTTP-SUPPORT.md)，独立Debug/Release各16/16、新套件sanitizer 2/2；产品与冻结旧源码不变，累计返工0/2，无新债。
- **S2 Content-Length正文与读取期限**（`已完成`，2026-10-29，Approved revision 1、Reviewer001 PASS、Leader003收口）：增量接收固定长度正文；请求头与正文分别限额，限制完整请求读取时间。完成标准：分段到达、正文与下一请求粘连、零/非零长度、提前FIN、长度溢出、超限、慢速发送均有确定行为；拒绝歧义CL及TE/CL组合，未支持的Transfer-Encoding与Expect不得误处理。已交付1MiB正文、16KiB独立头限额、默认10秒非滑动读取期限，非法framing 400、合法超限413并关闭；独立Debug/Release各18/18、规定sanitizer 6/6，累计返工0/2，无新债。S2验收时GET消费正文但不回显、POST/HEAD仍405；方法处理现已由下述S3扩展。
- **S3 方法路由与动态响应**（`已完成`，2026-10-29 Approved revision 1、Reviewer001 PASS、Leader003收口）：区分合法方法解析与资源允许方法；支持GET、HEAD及POST处理器，按方法和路径匹配，保留静态服务。交付有界 `POST /api/echo` 原始正文回显示例，不依赖JSON解析或数据库。完成标准：回显字节准确，HEAD不发送正文但有正确表示元数据，404/405及Allow明确，处理器失败受控，保活与响应顺序正确。已独立验证Debug/Release各20/20、规定sanitizer 8/8、负对照及清理，累计返工0/2，无新债；版本同连接正文与静态GET标准满足。

版本完成标准：同一连接上可连续发送带正文请求和静态GET，正文不串入下一请求；所有S1–S3验收通过。保留当前sendfile路径，不将动态正文错误套用文件长度约束。

禁止范围：chunked、multipart、数据库、业务线程池、代理、通用Web框架；不为方法齐全加入CONNECT/TRACE。

### V1.2 HTTP兼容与静态资源语义

状态：`已完成`（S1已完成、Approved revision1、Reviewer001 PASS，S2已完成且合并（Approved revision1及补充、Reviewer001 PASS）、S3已完成（Approved revision1、Reviewer002 PASS、Leader004收口），V1.2阶段验收全部完成，S3已合并origin/main（PR40），标签发布未核实；前置V1.1/S3已合并origin/main，未核实标签发布）。目标：扩展明确的HTTP/1.1子集，支持常见编码请求与浏览器缓存/下载场景，不宣称完整协议实现。

- **S1 chunked请求与Expect**（`已完成`，2026-10-29 Approved revision1、Reviewer001 PASS、Leader003收口）：有界增量chunk解码、终止块及trailer规则，支持受控100-continue；对块大小、解码总量、元数据与读取期限分别设边界。完成标准：任意拆分、非法块、截断、TE/CL冲突、过大正文和最终边界可验证；不会误发100后无限等待，不允许trailer覆盖framing/路由字段。已独立验证22/22 Debug、22/22 Release、10/10 sanitizer，真实100背压/期限/失败清理通过，累计返工0/2，无新债。
- **S2 请求目标与参数**（`已完成`，2026-10-29 Approved revision1及补充、Reviewer001 PASS、Leader005收口）：分离path/query，定义一次性percent解码、无效编码与编码分隔符策略，提供有界查询参数解析。完成标准：有效编码可用，双重编码、NUL、路径穿越及symlink不能越过静态根；路由与文件访问对同一请求目标解释一致。
- **S3 条件请求与单区间下载**（`已完成`，2026-10-29 Approved revision1、Reviewer002 PASS、Leader004收口）：GET/HEAD的ETag与If-None-Match、Last-Modified与If-Modified-Since，明确优先级；单区间Range及If-Range。完成标准：200/206/304/416、正文长度、HEAD和文件偏移正确，多区间有明确策略，静态文件sendfile及大小上限不回退。

版本完成标准：chunked POST、带编码路径/参数请求、条件GET和单区间下载可通过端到端用例复现；现有固定长度正文及保活矩阵不回退。

禁止范围：HTTP/2、HTTP/3、压缩、multipart上传、多区间响应、无限文件传输；本版本不新增业务数据存储。

### V1.3 无数据库的动态接口与业务执行

状态：`实现中`（S1已完成，Approved revision1及两补充、Reviewer001 PASS；S2已完成、S3未开始，V1.3整体未完成）。目标：用一个小型任务记录API串起输入、路由、业务执行、动态响应和失败处理；数据仅驻留内存，重启清空。

- **S1 JSON与资源接口**（`已完成`，2026-10-29 Approved revision1及两补充、Reviewer001 PASS、Leader007收口）：选择并评估JSON组件，固定大小/深度限制和错误格式；实现 `POST /api/tasks`、`GET /api/tasks`、`GET/PUT/DELETE /api/tasks/{id}`，PUT为完整替换，DELETE仅操作示例记录。限制记录数量、字段长度和列表响应大小；明确多worker访问一致性。完成标准：创建/查询/替换/删除、无效输入、未知ID及容量耗尽均有测试；静态目录不可通过API修改。
- **S2 有界异步业务执行**（`已完成`；Approved revision1，Reviewer002 PASS，Leader003收口）：为可能阻塞的处理器提供独立有界执行队列、结果回送、执行期限与过载拒绝。完成标准：慢处理不阻塞同owner其他连接，客户端断开/超时/停机后的迟到结果不触碰失效连接；排队和运行任务释放恰好一次。不能宣称可强制抢占任意C++处理器，合作取消和停机等待边界必须明确。
- **S3 综合应用验收**（`未开始`）：混合静态文件、JSON操作、慢处理器、并发修改及断开/停机故障。完成标准：资源有界、响应不乱序、无数据竞争；明确超时并不自动回滚已经发生的业务副作用；给出curl示例和可重复演示。

版本完成标准：无需数据库即可运行完整动态接口闭环，业务层与HTTP/net分离；数据易失性及错误、并发语义在使用文档明确。

禁止范围：数据库/ORM、持久化文件存储、账号登录、权限体系、PATCH、表单/multipart上传、通用插件或中间件框架。JSON依赖选型及引入在S1设计中固定，不在规划时下载或安装。

### V1.4 运行保障与交付

状态：`未开始`。目标：在受控部署中管理容量、观察运行状态、复现运行与故障行为；不据此承诺公网生产级服务。

- **S1 全局资源预算与过载**（`未开始`）：整合连接数、输入/输出、动态数据、业务队列等逻辑预算，定义拒绝及恢复策略。完成标准：多worker下准入不越界，持续过载及恢复不泄漏资源；区分逻辑预算与进程RSS，不能把前者宣称为硬内存上限。
- **S2 配置与运行观测**（`未开始`）：配置文件与CLI覆盖规则、启动校验、健康/就绪检查、运行中指标及日志输出/轮转策略。完成标准：错误配置启动失败，限额可解释，指标端点有独立暴露边界，慢日志sink与关闭期限有明确且验证过的契约；沿用既有统计定义或记录迁移。
- **S3 部署与版本回归**（`未开始`）：提供非特权运行、systemd示例、静态根权限和HTTPS外置终止说明；验证干净构建与完整协议/资源/故障矩阵。性能只做获批条件下的代表性基线。完成标准：按文档可启动、停止、重启和诊断；保留WSL2及长尾限制，不把新功能验收当作旧性能问题已解决。

版本完成标准：上述S1–S3通过独立审查，受支持功能和已知限制、部署说明、当前测试入口一致。

禁止范围：进程内TLS、热更新配置、分布式配额、多租户鉴权、代理、自动重试及生产SLA承诺。

## 长期演进方向

以下仅为候选，不进入V1.1–V1.4实施范围：数据库与持久化、业务鉴权、PATCH、表单/文件上传、压缩、进程内TLS、更高HTTP版本，以及原生Linux性能专项。待当前应用与资源边界稳定、用户提出具体场景后，再独立规划；不为“完整”预先引入所有功能。

本仓库长期聚焦HTTP服务端。L7转发/模型路由由独立AI Inference L7 Gateway项目承担，L4连接/流分配由独立L4LB承担；三者可在未来单独批准联调，但Server运行、构建与验收不依赖其他项目。

## 路线变更记录

- 2026-10-29：V1.1/S1按用户批准的revision 1完成实现及独立PASS，状态已完成；S2/S3未启动，V1.1未整体完成。
- 2026-10-29：依据用户“暂时不加数据库”“原来的网关那个版本就不要了”，取消原V1.1轻量L7扩展，将V1.1–V1.4用于HTTP正文、协议语义、无数据库动态接口和运行保障。授权范围是文档规划；阶段设计尚未批准，未启动实现。旧V1.1只有路线条目，未发现对应阶段设计/审查计划文件，故保留下方历史文本，不制造Approved/Superseded阶段文件。

<details>
<summary>已取消的旧V1.1网关规划（历史，不是当前要求）</summary>

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

本节是2026-10-29替换前的历史原文，其中“计划中”、路径及完成标准均已失效，不再用于启动Server阶段。

</details>

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
