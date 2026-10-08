# Changelog

本文档记录 HP HTTP Server 已经完成并得到适当验证的重要变化。未来计划写入 `ROADMAP.md`，长期架构写入 `ARCHITECTURE.md`，技术债写入 `TECH-DEBT-TRACKER.md`，实现与审查证据写入对应报告。

## Unreleased

### 2026-10-08 — V0.6/S2 固定场景矩阵

- 新增独立Release构建/身份校验与6场景×3轮矩阵，覆盖大小、连接模式和worker/连接组合；两个角色各自18有效样本，五measurement errors均零，HTTP审计及自有进程回收通过。
- 保存单样本CPU原始ticks/wait4/身份/包围clock供独立复算；延迟为wrk校正分布，精确人口明确未采，三轮分位数中位不等于合并P99。首工具超时、CPU证据缺口及两轮返工保留，Reviewer002 PASS。
- [独立结果](benchmark/results/V0.6/S2-reviewer-001.md)限定WSL2同机loopback、热缓存、closed-loop；不宣称性能恢复/多核线性扩展/物理机容量。M3长尾风险保留。S2已完成，V0.6整体未完成，S3未开始；旧风险/延期不关闭。

### 2026-10-08 — V0.6/S1 指标与访问记录

- 增加请求、状态码、错误、连接和服务端响应延迟的固定容量原子统计；ResponseResult显式传递状态/计划正文长度。completed表示kernel接收排空，非客户端已接收。
- 增加默认关闭的 `--access-log` 与 `--metrics-on-exit`：有界JSON访问payload去除query/fragment；受控退出在worker/连接销毁与日志排空后向stdout导出文本，不自动创建文件或新增HTTP路由。日志异常不改变业务响应。
- 修复本阶段首审发现：优雅drain真实排空保留完成通知、记completed；强制截断记aborted，drain不推进pipeline后缀。历史FAIL保留，范围内返工1/2后Reviewer002 PASS，独立12/12 CTest、ASan+UBSan/LSan2/2及18文件格式检查通过。
- 仅S1完成，V0.6整体未完成，S2/S3未开始；不证明性能恢复或长尾修复。V0.5.1搁置未完成和RO-002/TD-001/TD-006保持。

### 2026-10-08 — V0.5.1已搁置（未完成）

- 管理停工并保存当前调查资料，不是整体验收或发布新版本。已合并TCP_NODELAY局部修复及原S1/S2证据保持；此前acda3f9日志批量优化有功能/专项验证，但S3高并发P3 E/C P99=3.2446，未达≤0.25门槛。
- S4固定C/D构建与短HTTP冒烟、R033两方离线工具合同有限PASS；正式高并发诊断仍未完成，R034未封包/未动态执行，当前候选入口批准SHA过期，不能直接运行。
- 高并发长尾尝试修复/定位未果，RO-002仍开放。S5/S6未开始；V0.6未启动。归档工具不代表已验收生产功能。详[停工记录](benchmark/results/V0.5.1/SHELVED.md)。

### V0.5.1/S2 小响应发送修复

- 接收连接交付前启用TCP_NODELAY；失败只关闭当前连接。保留sendfile、背压与关闭流程，独立Reviewer001 PASS。
- 独立8项CTest、专项ASan/UBSan/LSan、40工具测试、12配对样本及默认客户端两组机制通过；1KiB吞吐51.918倍、P99 48.399→2.024ms，大文件保护门槛通过。结果限定本次WSL2负载，小文件CPU显著上升；S3及版本整体尚未完成。
- 修复新验证工具运行账本覆盖缺口，补独立反例；保留测试fixture失败和构建路线纠正记录。旧测试、历史基准和用户注释保持。


### V0.5.1/S1 性能诊断

- 交付独立A/B/C诊断入口、身份/预算/审计与失败回收验证，保留历史基准；38项工具测试、独立18正式样本、中间版本三轮及六组机制实验通过，Reviewer002 PASS。
- 当前1KiB独立中位吞吐715.070 QPS、每轮P99中位数48.392ms，退化仍在；证据支持拆分发送与ACK等待交互，异常在v0.5-s1已出现。生产修复尚未实施，不宣称性能改善或V0.5.1整体完成。
- 修复新诊断工具的ASLR身份误判与中间测量尾部身份校验缺失；保留首套invalid及Reviewer001 FAIL。未改生产源码、冻结旧测试或用户注释。


### 重构

- 2026-09-26：R7 完成语义命名与原有英文注释中文化：38 个 C++ 文件、494 个标识符 token，66 项函数映射；创建工作线程、执行循环、指定 worker 投递及等待退出明确区分，保留运行行为。160 条保护注释逐字保留，93 条原有英文说明译中文。独立 Reviewer001 PASS WITH DEBT、Debug 构建、6/6 CTest、HTTP smoke 及词法/字面量核查通过；31 个历史测试及配置保持，仅延续 TD-006。12 个文件既有格式差异按批准例外保留，不宣称全文件格式通过。本轮未提交或推送。

- 2026-09-21：R6 完成最新个人 C++ 规范迁移：46 个生产文件中45个改为 PascalCase，普通函数/变量采用 camelCase；事件回调使用短转发 lambda、具名目标及头内纯保存 setter，受保护注册入口保留原检查时机。EventLoopThread 使用已批准的共享任务槽，入队前归一所有权，失败捕获在解锁后释放；新增一次槽分配，不作性能改善声明。
- R6 独立 Reviewer001 结论 PASS WITH DEBT，Leader002 收口；当前 CTest 6/6、ASan/UBSan/LSan 专项1/1、HTTP smoke、47文件格式检查及任务所有权负向验证通过。31个旧测试路径和内容不变，保留5个兼容CTest，23个旧接口目标按用户要求暂退构建与注册；新增1项专项测试。唯一延期 TD-006 为旧测试按需恢复，历史28/28不代表当前覆盖。此记录不代表 R6 已合并或发布标签。

- 2026-09-19：获批全测试清理完成，审查全部29个测试文件、实际修改28个及两份应用测试观察接口文件；普通命名、真实具名异步目标和格式统一，保留原断言、注入、捕获、门控、时限及负向场景。独立Reviewer final-test-cleanup002 PASS关闭FS-01–04/FTC-01：75/75格式、完整Debug28/28（22.74秒）、ASan/UBSan/LSan3/3（0.52秒）。两轮纠正2/2成功，最初编译错误与四处规范问题证据保留。生产差异仅测试观察接口声明/命名；不新增运行逻辑、性能或全行为等价声明。与R5一起尚未提交推送。

- 2026-09-19：R5完成14文件日志操作/等级/常量/调用迁移，consumer与六处受影响测试异步入口显式绑定具名目标；保留队列、sink、计数、复制及Stop生命周期。Builder一次28/28；Reviewer独立Debug4/4、ASan/UBSan/LSan1/1，生产/规定测试一致性PASS。一次LSan沙箱路线纠正1/2，原失败保留、选项未弱化；源码返工0。R1–R5批准的渐进重构范围关闭，未改目标的历史fixture及同步谓词/deleter等例外明确保留；R5未提交推送，无新性能声明。

- 2026-09-19：R4完成CLI/startup/SignalWatcher入口命名及三个数字验证函数提取，共四个C++文件；保留参数规则、诊断、退出码及信号生命周期，直接测试调用同步。Builder一次28/28，独立Reviewer001 PASS：首批三项通过、server_integration_tests因缺失构建目标NOT RUN，补建后仅该项1/1通过。两次证据纠正（格式参数与构建清单），源码返工0；原记录保留，不声称首批4/4。R4后续已提交fdc1b14、PR #21合并5eeb859并发布refactor-r4；R5结果见上文。

- 2026-09-19：R3完成ConnectionIo全部操作、常量和SendFileWithoutSigpipe辅助入口改名，同步全部直接调用；三处读写结果显式初始化。七个C++文件只包含批准的符号替换与等价初始化，不改算法、回调、所有权、测试断言或参数。Builder一次完整28/28；独立Reviewer001六项Debug6/6、ASan/UBSan/LSan2/2，规范/语义检查PASS，返工0/2。R3后续已提交ffb20a6、PR #20合并9a49667并发布refactor-r3；R4结果见上文。

- 2026-09-19：R2按R006完成Reactor/线程/定时器及TCP→HTTP回调装配重构：组件槽提供可导航的具名注册入口，HTTP工厂/消息及调度载体显式绑定具名方法，移除迁移范围内自定义operator()回调入口。服务器在外部装配后于Run启用接收；保留惰性Session、原复制/线程/所有权、协议与生产缓冲策略。
- 独立Reviewer005 PASS，F-C01六处测试绑定违规已关闭：Debug11/11、真实背压两目标各三次、ASan/UBSan/LSan6/6通过。Builder先前完整回归27/28及授权单项修复1/1分别保留，另有threads=0 3/3、专项sanitizer7/7和六处修复涉及的两目标Debug/sanitizer各2/2；不声称新跑完整28/28。R2第2/2轮停工、单问题修复及六处修复的逐次用户授权均保留，不重置额度。无新性能声明；R2后续已提交5317298、PR #19合并d7693da并发布refactor-r2，整体重构未完成。

- 2026-09-17：R1统一基础/HTTP/Socket/Epoller操作、枚举和常量名称并同步全部调用者；parser多步ASCII比较/空白裁剪提取具名方法，业务结果显式表达状态与文件归属。保留协议、资源、线程和借用行为，不引入新功能或性能承诺。
- formatter新增显式文件清单，提交hook仅检查暂存C++，保留索引/部分暂存/配置/路径/版本保护和不自动暂存；使用用户原样Google覆盖配置。独立Reviewer001 PASS：新基线与候选28/28、双smoke、0专项、五项ASan/UBSan、13项hook及两类负向反证通过。R1当时按仅本地提交授权收口；后续已合并并发布refactor-r1，R2结果见上文。

### 新增

- V0.5/S4交付固定版本wrk脚本、环境与完整公开数据、26项快测；修复响应尾字节审计漏检。Reviewer002独立12/12测量、24阶段错误0、120次审计及进程/端口回收通过；按指纹继承独立Debug28/28、旧3/双curl及S3 sanitizer证据。Leader005核对五项条件关闭S4与V0.5，未合并或标签发布。
- 基准有效性PASS不表示性能改善：独立1KiB中位QPS A21373.88/B713.56，B/A0.033385（观测下降约96.66%，A noisy）；1MiB B/A1.019197且双方noisy，根因未确定。保留失败及不利样本，不宣称稳定降幅或普适收益。

- V0.5/S3交付连续游标Buffer与ConnectionIo直接recv，减少consume/尾空间足够时追加的后缀搬移，复用小header；完全排空后>64KiB输出容量释放、≤64KiB保留。生产16KiB输入、9MiB逻辑输出含file remaining、sendfile及HTTP背压/超时不变；重复大输出有重新分配取舍，不宣称吞吐收益。Reviewer001独立27/27、三TSan/五ASan及五反证通过；Reviewer002核实唯一两换行格式修复后最终PASS，Leader003关闭S3及TD-005本检查点。S4未开始，V0.5未完成，Unreleased不代表已发布。

- V0.5/S2交付有界异步日志：1024槽、1024字节正文上限及截断，所有等级满队列丢新并计数；单消费者写stderr/flush，拥有消息，生产RAII及并发stop安全回收。健康sink排空；阻塞stderr可能拖延最终join，HTTP shutdown_timeout不保证整个进程退出上限。原接口/诊断及HTTP行为保持，没有吞吐增益承诺。独立Reviewer001 PASS、Debug25/25及三TSan/三ASan、四反证通过，Leader003关闭S2及TD-002；V0.5未完成，Unreleased不表示已发布。

- V0.5/S1交付生产小内存头+拥有型fd/sendfile正文，保持8MiB文件/9MiB逻辑输出及安全路径、HTTP、超时和排空。正文不进入用户输出vector；unsupported/发送错误关闭，不自动read降级、不追加第二响应。要求稳定文件内容，不据机制证据宣称性能提升。
- 2026-09-10经S1 Reviewer002 PASS、Leader003收口：独立23/23、旧3/3、双curl、三TSan/三ASan与三配置probe、13精确反证通过。P2-01已关闭，首FAIL及返工历史保留；仅S1完成，V0.5未完成，本条不代表合并/tag发布。

- V0.4/S4交付9MiB每连接输出边界、压缩及1024每loop普通任务上限；固定控制通知不受普通队列满阻挡。SIGINT/SIGTERM触发停止接收并排空已有输出，统一默认5000ms截止、0立即、重复信号可强关；关闭后不推进pipeline，不补额外响应。
- 2026-09-09经S4 Reviewer002独立PASS与Leader003关闭S4及V0.4：Debug零告警、22/22、threads0旧3/3、双curl、四TSan/三ASan与三worker九连接及负对照通过。Reviewer001 FAIL和Builder返工记录保留，两项P2已关闭，无新增债务。仍为Unreleased，不代表合并或标签发布。

- V0.4/S3交付owner单调TimerQueue、EventLoop最近截止调度、一连接一timer及实际recv/send进展续期；生产idle默认30000ms、keep-alive等待15000ms，各0禁用、组合取早。到期静默关闭，可截断响应而不发送408；不包含最低速率、总请求时限、阻塞抢占或S4高水位/优雅关闭。
- 2026-09-09经S3 Reviewer001独立PASS、Leader003收口：零告警、CTest20/20（14.20秒）、threads0服务3/3、双curl、四TSan与三ASan/UBSan/LSan通过；独立三worker六连接、真实截断前缀、CLI与RST/stop探针通过。S3完成，V0.4未完成，本条不代表提交或发布。

- V0.4/S2交付固定EventLoopThreadPool与生产main/sub Reactor：默认2个worker、`--threads 0`单Reactor兼容，连接轮转后终生owner固定，各ConnectionRegistry回调后回收；Session/parser在owner创建，静态文件服务安全共享。每worker池入口固定1024个未结束任务，满时关闭交接连接；提供立即停止、部分启动失败与worker异常时全部回收，不承诺响应排空或完整资源治理。
- V0.4/S2于2026-09-09经独立Reviewer001 PASS、Leader003收口：Debug零告警、默认CTest18/18（11.57秒）、显式0旧服务回归3/3、双curl、三TSan及三ASan/UBSan/LSan目标通过；另有3worker/9连接、8×257停止交错与真实provider计数探针。仅S2完成，V0.4尚未完成，本条不代表提交或发布。

- V0.4/S1 交付 EventLoop owner 约束、异步任务队列、eventfd 唤醒、停止排空与异常取消，以及 EventLoopThread 启动握手、清理、join 异常回传；注册 token 原子分配并锁存耗尽。生产 HTTP 保持单线程，任务队列仅受控有限投递，未交付线程池、容量治理或进程优雅关闭。
- V0.4/S1 新增线程专项并保留原 15 个 CTest 身份；独立 Debug 零告警、CTest16/16（9.69 秒）、curl、TSan、ASan/UBSan/LSan 及 Reviewer 独立任务/捕获重入探针通过。2026-09-09 经 Reviewer001 PASS、Leader003 关闭 S1；V0.4 整体尚未完成，本记录不代表发布。

- V0.3/S3完成既有协议异常回归：66具名短语料、9长边界及6629调度，首/第二请求14真实拒绝、52逐前缀FIN、两类RST隔离与fd回收；保留原15个CTest身份。仅扩充测试及README，无生产协议或接口变化。
- V0.3/S3独立Debug告警0、CTest15/15（4.88秒）、额外4语料/463调度、curl及两项ASan/UBSan/LSan通过，Reviewer001唯一PASS，Leader004核对版本条件并完成V0.3收口。完成日期2026-09-09，当前仍记Unreleased，不代表发布。

- V0.3/S2交付HTTP/1.1无请求体GET默认保活、显式close优先与有界串行响应；新增通用pause/resume/write-complete，响应排空后推进缓存后缀并正确处理EOF。结构化ResponseResult确保服务400的Header和实际终止一致；旧handle默认close兼容。
- V0.3/S2受限framing矩阵只接受无body或唯一CL零；重复/列表/非零或非法CL、任意TE/Expect统一400关闭。保留原13测试并新增组件/生产复用专项，独立Debug告警0、CTest15/15、双专项ASan/UBSan/LSan通过；Builder001、Reviewer001 PASS及Leader004记录交付。该S2记录不代表版本发布；V0.3最终收口见本节S3记录。

- V0.3/S1交付每连接增量RequestParser，状态/新字节消费/首请求边界/reset/自有结果，新增状态专项，CTest由12增为13；Builder001、Reviewer001及Leader003保留交付证据。

- V0.2/S3 新增TcpConnection消息/发送/消费/排空关闭接口、app HTTP适配器与每连接工厂；新增消息专项，CTest由11增至12。Builder001、Reviewer001、Leader003保留实现、PASS与版本关闭证据。

- V0.2/S2 交付生产 Acceptor/TcpConnection 和独立 ConnectionIo 文件，新增组件动态专项，CTest 由 10 项增为 11 项；Builder 001、Reviewer 001 与 Leader 003 保留实现、PASS 与收口证据。

- V0.2/S1 新增单线程 EventLoop 与非 fd owner Channel，生产入口实际接入；新增事件核心动态专项，CTest 总数由 9 增至 10。
- V0.2/S1 Builder 001、Reviewer 001 和 Leader 003 记录实现、独立 `PASS` 与阶段关闭。

- 新增并批准 `docs/leader/designs/V0.1/S1-design.md`，形成 Builder 可执行的 S1 基线。
- 新增并批准 `docs/reviewer/reviews/V0.1/S1-review.md`，形成 Reviewer 的 REQ/AC/RV 审查基线。
- 新增 `docs/leader/reports/V0.1/S1-report-001.md`，保留初始设计过程。
- 新增 `docs/leader/reports/V0.1/S1-report-002.md`，记录文档完整性整改、决策和验证结果。
- 新增 S1 的 CMake/C++20 工程骨架、同步日志、Socket fd RAII、基础设置操作、最小 CLI 和 3 个 CTest 测试目标。
- 新增 Builder 实现报告 001/002、Reviewer 审查报告 001 和 Leader 阶段关闭报告 003，保留 S1 实现、独立验收与关闭证据。
- 新增 S2 的 Epoller RAII、Socket 监听扩展、集中式单线程 TCP echo 服务器、连接 IO 状态和 3 个网络测试目标；CTest 总数由 3 增至 6。
- 新增 S2 Builder 报告 001/002、Reviewer 报告 001/002 和 Leader 关闭报告 004，保留首轮 `FAIL`、范围内返工、最终 `PASS` 与收口证据。
- 新增 S3 的严格有界 HTTP 请求解析、响应构造、fd-relative 静态文件服务、`--root` CLI、示例首页及 parser/static/真实服务器/curl 测试；CTest 总数由 6 增至 9。
- 新增 S3 Builder 报告 001、Reviewer 报告 001 和 Leader 关闭报告 003，保留 Approved 基线、实现、独立验收与收口证据。

### 变更

- V0.3/S1生产feed新字节后立即消费accepted，NeedMore不重扫前缀；恰好4096请求行内容前缀等待CRLF，4097拒绝，总头部16384限额保持。服务仍单响应关闭；S1已完成，V0.3整体进行中，S2/S3未开始。

- V0.2/S3生产HTTP改走TcpConnection消息回调和app适配器，ConnectionIo移除应用策略；每连接独立done保留单响应与原HTTP行为。S3及整个V0.2已完成，V0.3未开始。

- V0.2/S2 将监听、单连接事件/interest/关闭迁出 TcpServer；输入输出仍归 ConnectionIo，应用契约不变，关闭按稳定 identity 校验并在回调后回收。S2 已完成，V0.2 进行中，S3 未开始。

- V0.2/S1 将 wait、interest 与完整事件分发抽入 EventLoop/Channel，使用稳定 token 隔离 stale/fd reuse，并在回调返回后释放已移除连接；保留 V0.1 HTTP 行为。S1 已完成，V0.2 整体进行中、S2/S3 未开始。

- 统一仓库级文档权威顺序、阶段生命周期、返工规则、角色交接和 Reviewer 唯一结论。
- `ARCHITECTURE.md` 区分当前实现状态与长期目标，补充 metrics 依赖、事件循环线程池和信号关闭边界。
- `ROADMAP.md` 补充版本前置条件、阶段完成门槛，并明确 V1.1 是 V1.0 之后的可选扩展。
- S1 设计固定工具链基线、CLI 范围、Socket 所有权、错误策略、Builder 里程碑和验收追溯。
- V0.1 的 S1、S2、S3 状态均同步为`已完成`；下一步由 Leader 设计 `V0.2 / S1 EventLoop 与 Channel`，新基线批准前不直接实现。
- CLI 从 S1 骨架行为升级为严格的 `--port <0-65535>` 入口；运行时明确标识临时 S2 TCP echo 与非 HTTP 边界。
- S2 返工以最小生产 `ConnectionIo::handle_event` seam 补齐真实组合事件与 `SO_ERROR` 证据，没有改变 Approved 范围或架构。
- 运行入口由临时 TCP echo 升级为每连接单个 `GET` 的最小 HTTP/1.1 静态文件服务，使用严格大小边界、`Connection: close`、root-fd 锚定的逐组件 `openat`/no-follow 访问及受控的 `200/400/403/404/405/500` 响应。
- 技术债跟踪器改为只记录跨阶段债务、批准延期和持续风险，并移除不属于技术债范围的本地文档发布议题。

### 验证

- V0.3/S1独立Debug告警0、CTest13/13、全部REQ/AC/RV PASS；19类/708split/六边界、真实双连接6feed/56字节精确消费、旧Reactor/HTTP/curl和两项ASan/UBSan/LSan通过。P3-01与当前TD检查点已收口，无新债务。

- V0.2/S3独立Debug告警0、CTest12/12，旧11项逐场景映射、全部REQ/AC/RV和版本完成标准PASS；临时响应524390字节完整排空、pipeline第二响应0、新消息ERRIN后1053字节、HTTP/curl/S1/S2与新专项sanitizer通过；无新增债务，P3-01/P3-02已收口。

- V0.2/S2 Reviewer 全新 Debug 告警 0、CTest 11/11，全部 REQ/AC/RV PASS；单次 drain8、65536 字节恢复、旧 identity/fd reuse、回调后销毁、真实 ERR/IN→SO_ERROR→recv1053、HTTP/curl 与新专项 ASan/UBSan 通过，无新增债务，P3-01 已收口。

- V0.2/S1 Reviewer 全新 Debug 构建告警 0、CTest `10/10`、REQ-01..08/AC-01..10/RV-01..10 全通过；真实 stale/fd reuse、ERR|IN → SO_ERROR → recv 1053 字节、HTTP EAGAIN/路径安全/隔离/fd 稳态与 curl 均通过，事件专项 ASan/UBSan 无报告。唯一结论 `PASS`，无新增债务，P3-01 已由 Leader 收口关闭。

- Builder 已完成配置、构建、CTest 和三种 CLI 验证，CTest `3/3` 通过。
- Reviewer 使用全新的 `build-review/` 独立完成 Debug/Ninja 配置与构建，RV-01 至 RV-07 全部通过，CTest `3/3` 通过，唯一结论为 `PASS`。
- Reviewer 确认 P0、P1、P2 均无；唯一 P3 顶层状态同步已由 Leader 阶段关闭处理。
- Reviewer 报告：`docs/reviewer/reports/V0.1/S1-report-001.md`。
- S2 Reviewer 首轮因 P2-01 组合事件动态证据缺失给出 `FAIL`；Builder 范围内返工后，Reviewer 在全新 `build-review-s2-r2/` 中独立完成 CTest `6/6`、专项 `100/100`、全量 `60/60`，REQ-01..07、AC-01..08、RV-01..08 全部通过，最终唯一结论 `PASS`。
- S2 P0/P1/P2、无法验证项和新增技术债均无；P3-01 根状态漂移由 Leader 阶段收口关闭。
- S3 Reviewer 在全新 `build-review-s3/` 中完成 Debug 构建且告警为 0、CTest `9/9`，REQ-01..08、AC-01..10、RV-01..10 全部通过，唯一结论 `PASS`。
- S3 真实集成 `10/10`，状态计数 `{200:35,400:6,403:6,404:1,405:1}`，生产写 EAGAIN 非零、secret 泄露为 0、隔离 `20/20`、服务 fd `6->6`；静态文件成功路径 `openat/close 530/530`，S2 组合事件与 `SO_ERROR=ECONNRESET` 回归仍动态成立。
- S3 无 P0/P1/P2、无法验证项或新增技术债；P3-01 根状态漂移由 Leader 阶段收口关闭。

## V0.0.0 - 2026-05-22

版本摘要：

初始化项目准备阶段文档基线。该版本尚不包含可运行服务器代码，主要用于固定项目定位、长期架构、版本路线、技术债跟踪方式和 README 入口说明，为后续 `V0.1` 代码实现做准备。

### 新增

- 新增 `ARCHITECTURE.md`，明确项目长期架构、系统分层、模块职责、数据流、依赖方向、安全边界、测试架构和架构变更流程。
- 新增 `ROADMAP.md`，规划从 `V0.1` 最小可运行 HTTP Server 到 `V1.1` 轻量 L7 Gateway 的版本路线、阶段划分、禁止范围和完成标准。
- 新增 `TECH-DEBT-TRACKER.md`，记录准备阶段状态，建立技术债 ID、风险观察、下一阶段检查点和关闭规则。
- 新增 `README.md`，作为项目入口说明当前状态、项目定位、环境要求、规划能力、文档索引、开发流程和已知限制。

### 变更

- 将项目定位明确为面向高性能网络岗的 Linux C++ HTTP/1.1 服务器项目。
- 明确核心技术主线为 C++20、Linux socket、非阻塞 IO、`epoll`、Reactor、HTTP/1.1、连接管理、资源治理和性能分析。
- 明确轻量 L7 Reverse Proxy / Gateway 是 HTTP Server 内核稳定后的扩展方向，而不是当前阶段默认实现范围。
- 明确当前仓库处于准备阶段，尚未进入代码实现，README 不提供虚假的构建、运行或压测结果。

### 兼容性

- 当前版本尚未提供运行时接口、配置格式、命令行参数或数据文件格式，因此不存在用户侧迁移要求。
- 后续一旦出现可运行命令、配置格式或用户可见行为变化，需要在本文件追加对应版本记录。

### 安全

- 架构文档明确静态文件服务必须限制在配置根目录内，后续路径解析需要防止路径穿越。
- 架构文档明确生产代码不得执行用户传入的系统命令，日志不得泄露敏感信息。
- 当前版本尚未实现网络服务，因此没有运行时安全修复。

### 验证

- 本阶段为文档准备阶段，未运行代码测试。
- 已检查路线图、技术债、README 的标题结构和当前状态声明。

### 相关文档

- `ARCHITECTURE.md`
- `ROADMAP.md`
- `TECH-DEBT-TRACKER.md`
- `README.md`
- `docs/references/`
