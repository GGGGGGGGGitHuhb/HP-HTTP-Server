# 停工前历史检查点（截至2026-10-08）

以下原文记录历史时点，不代表当前状态。docs链接为本地协作资料，不随远程发布。

# HP HTTP Server

## 当前状态

2026-10-08：固定C/D独立导出、构建与短HTTP冒烟已通过R032有限验收，执行证据见[Leader044](../../docs/leader/reports/V0.5.1/S4-report-044.md)。S4仍返工中，RO-002高并发长尾风险未关闭，S5/S6未开始；S3已终止。正式性能诊断/采集需另行批准，已消费命令不可重跑。工具入口与边界见[benchmark](../../benchmark/README.md)，版本路线见[ROADMAP](../../ROADMAP.md)。以下保留历史性能结果。

S3 **返工中**（2026-10-05，独立Reviewer009结论 **FAIL**，R005完整C/E矩阵未达P3延迟门槛）：R002完整30样本已执行，60个预热/测量阶段错误均零，但单连接P1/D吞吐跨度53.91%超过20%，128连接P3/D P99中位667.923ms，对基线49.136ms比值13.593，超过0.25门槛。见[本次结果](../../benchmark/results/V0.5.1/S3/S3-builder-007.md)及[独立复核](../../docs/reviewer/reports/V0.5.1/S3-report-006.md)。本次未复现历史超时/计时异常，历史根因仍未知；[原失败证据](../../benchmark/results/V0.5.1/S3/S3-builder-004.md)保持。已按R002失败即停，未启动Reviewer完整矩阵；[R003/R004有限定位总结](../../docs/leader/reports/V0.5.1/S3-report-013.md)：增加压测线程未稳定改善；日志file/null/null/file对照P99为121/32/57/133ms，支持日志输出成本参与长尾，但仍未达到原门槛，具体机制未完全定位。以上为R004诊断结束时状态；当前R005日志批量修复已实现，Builder9/9及ASan/UBSan/LSan通过；唯一C/E完整矩阵有效，但P3 P99为158.912ms/C48.977ms，比值3.2446仍超过0.25。全部QPS跨度合规，其余场景达门槛；已停止动态，Reviewer已独立复算确认FAIL，S3不关闭。见[R005结果](../../benchmark/results/V0.5.1/S3/S3-builder-010.md)及[独立复核](../../benchmark/results/V0.5.1/S3/S3-reviewer-009.md)。

2026-09-28：**V0.5.1/S2 已完成，独立 Reviewer001 PASS**。新接收TCP连接在交付前启用TCP_NODELAY，设置失败只关闭该连接，保留sendfile和原背压/关闭流程。S1已发布标签v0.5.1-s1；S2经PR #26合并69424e6并发布标签v0.5.1-s2。S3阻塞，V0.5.1整体未完成。

独立固定负载下1KiB吞吐 **716.522→37200.425 QPS（51.918倍）**，每轮P99中位 **48.399→2.024ms**；默认客户端正文等待降至0.27–0.28ms。1MiB吞吐比1.003842、P99比1.064837，通过保护门槛。小文件server CPU中位由6.823%升至188.920%（单核100%），不能只报收益；结果限定于本机WSL2/loopback/热缓存。[独立结果](../../benchmark/results/V0.5.1/S2/S2-reviewer-001.md)。

S2验收时CTest为 **8项（原6+新2）**；独立专项ASan/UBSan/LSan、40项工具测试、12正式样本及两组机制验证通过。31个旧测试继续冻结，23个停用目标未恢复。历史失败和一次Reviewer构建路径纠正保留，不冒称所有首轮通过。

验证入口：[S3回归工具与限制](../../benchmark/regression/README.md)、[S2修复验证](../../benchmark/repair/README.md)、[S1诊断](../../benchmark/diagnosis/README.md)。历史基准保持不变；S2当前接口专项补足本轮故障隔离、慢读EAGAIN和关闭覆盖。RO-002继续等待S3最终验收。[路线图](../../ROADMAP.md)及[问题跟踪](../../TECH-DEBT-TRACKER.md)。

下方R7及更早状态均为历史时点记录。

R7 语义命名与原有英文注释中文化已完成（2026-09-26），独立 Reviewer 为 PASS WITH DEBT，仅延续 TD-006。工作线程入口现为 `createWorkerThread()` / `runWorkerEventLoop()`，通用循环为 `runEventLoop()`，线程池按指定索引投递使用 `postTaskToWorkerAtIndex()`。本轮 38 个 C++ 文件同步名称，160 条保护注释保持原文、93 条原有英文注释翻译中文；独立 Debug、6/6 CTest、HTTP smoke 通过。12 个文件既有格式差异按批准例外保留，不代表全文件格式通过。31 个历史测试继续冻结，未恢复 23 个停用目标；本轮未提交或推送。下方 R6 及更早验证数据属于历史记录。

R6 最新个人规范迁移已完成，独立 Reviewer 结论为 PASS WITH DEBT（仅 TD-006 旧测试按需迁移延期）：生产文件改为 PascalCase，普通函数和变量为 camelCase，回调使用事件类型、短转发及头内纯保存 setter。具有注册时机约束的入口先检查再调用私有 setter。旧测试源码和路径冻结；当前默认验证为 5 项兼容旧测试与 1 项新增专项测试。独立 Debug 6/6、ASan/UBSan/LSan 专项 1/1、HTTP smoke 与 47 文件格式检查通过；31 个旧测试路径和内容不变。以下 R1–R5 测试数字均为历史记录，不代表 R6 当前覆盖。

- 当前阶段：V0.5/S4可复现压测基线已完成，Reviewer002唯一PASS、Leader005按五项版本条件关闭V0.5；S1–S4全部完成，本地基线`ecddb98`已包含PR #17合并，未新增版本标签。R1基础接口重构已完成（独立Reviewer001 PASS）：接口和调用者同步迁移、parser具名helper及显式结果、局部格式化与暂存范围hook已交付；独立28/28、五项ASan/UBSan、13项hook回归和双smoke通过。R1已合并并发布`refactor-r1`。R2按Approved R006完成独立验收（Reviewer005 PASS、Leader012）：组件回调注册可定位、持久目标显式具名绑定，通用任务容器保存已绑定任务；历史失败、停工及用户逐次授权的窄范围修复均保留；R2已提交5317298、经PR #19合并至d7693da并发布`refactor-r2`。R3 ConnectionIo命名及显式结果重构已完成（独立Reviewer001 PASS、Leader003，返工0/2），R3已提交ffb20a6、PR #20合并9a49667并发布`refactor-r3`。R4 CLI/信号辅助已完成（Reviewer001 PASS、Leader003），源码返工0、证据纠正2/2；R4已提交fdc1b14、PR #21合并5eeb859并发布`refactor-r4`。R5日志及限定一致性检查已完成（Reviewer001 PASS、Leader003），源码返工0、执行路线纠正1/2；`codex/refactor-r5`已提交 f7c1bc5，并经 PR #22 合并至 d465995。R1–R5及随后获批的全测试清理已完成：全部29个测试文件接受规范审查，实际修改28个测试文件及两处应用测试观察接口文件；Reviewer final-test-cleanup002 PASS，75/75格式、独立完整28/28和sanitizer3/3通过。FS-01–04及FTC-01关闭，两轮纠正2/2历史保留。保留规范允许的短同步谓词、直接异常检查、getter/外部ABI/override及标准deleter，不再豁免历史测试fixture；不表示全仓消除lambda。R5和测试清理已随 PR #22 合并。运行命令、环境与完整数据见 [压测说明](../../benchmark/README.md) 和 [独立结果](../../benchmark/results/V0.5-S4-reviewer-002.md)。1KiB实测A/B中位QPS为21373.88/713.56，B/A=0.033385；A组noisy，不能宣称性能改善、稳定降幅或根因。1MiB双方noisy，同样无稳定收益结论。

- S2交付：V0.3/S2 Keep-Alive 连接复用已完成 / Completed；原Approved revision1及Approved S2-rework-001已实现，独立Reviewer唯一PASS。批准见 `docs/leader/reports/V0.3/S2-report-002.md`，补充见 `docs/leader/reworks/V0.3/S2-rework-001.md`。

- V0.3/S3 历史验收：独立全新Debug告警0、CTest15/15（4.88秒）、6REQ/8AC/8RV全部通过，parser状态与keep-alive组件ASan/UBSan/LSan无诊断。交付：`docs/builder/reports/V0.3/S3-report-001.md`、`docs/reviewer/reports/V0.3/S3-report-001.md`、`docs/leader/reports/V0.3/S3-report-004.md`。

- 历史版本：`V0.3 HTTP 状态机与连接复用`已完成，S1/S2/S3均已完成；V0.1/V0.2已完成，V0.4已完成，S1已完成，S2已完成，S3已完成，S4已完成（Approved）。S1已合并至main并发布标签 `v0.4-s1`；S2已独立验收、收口并经PR #11合并至main，标签 `v0.4-s2` 已推送。
- 前置版本状态：`V0.1 最小可运行 HTTP Server` 已完成；S1、S2、S3 均有 Approved 基线、Builder 实现证据与 Reviewer `PASS`。
- V0.5/S3历史验收：`V0.5/S3 Buffer与背压优化`；Reviewer001独立Debug27/27（28.73秒）、旧3/3、双curl、三TSan/五ASan、M0/五反证、真实0/1/2及300轮回收通过。Reviewer002以精确两换行修复及指纹不变关闭唯一格式P2-01，最终8REQ/8AC/8RV通过；本轮复审未重复动态测试。P3-01及TD-005本检查点完成，TD-005持续Open。

- V0.4/S4历史验收：`V0.4/S4 资源上限与优雅关闭`；独立Debug零告警、22/22（15.70秒）、threads0旧3/3（6.00秒）、双curl、四TSan/三ASan及独立正负探针通过。Reviewer002关闭两项P2，Leader003按ROADMAP六项条件关闭V0.4；S4已合并并发布v0.4-s4；V0.5/S1已完成sendfile验收。报告：`docs/reviewer/reports/V0.4/S4-report-002.md`、`docs/leader/reports/V0.4/S4-report-003.md`。

- V0.4/S3历史验收：`V0.4/S3 定时器与连接超时`；独立Debug零告警、CTest20/20（14.20秒）、threads0服务3/3（4.31秒）、双curl、四TSan及三ASan/UBSan/LSan通过。8REQ/12AC/RV与8条生命周期全部通过；审查：`docs/reviewer/reports/V0.4/S3-report-001.md`，收口：`docs/leader/reports/V0.4/S3-report-003.md`。

- V0.4/S2历史验收：`V0.4/S2 主从 Reactor`；独立Debug告警0、默认CTest18/18（11.57秒）、显式0服务回归3/3、双模式curl及六项sanitizer通过，8REQ/12AC/12RV与11条生命周期契约全部通过。交付：`docs/builder/reports/V0.4/S2-report-001.md`、`docs/reviewer/reports/V0.4/S2-report-001.md`、`docs/leader/reports/V0.4/S2-report-003.md`。
- 真实运行入口现为主从 Reactor 的受限 HTTP/1.1 静态文件服务（默认两个 worker，`--threads 0` 回退单 Reactor）：严格要求 `--port` 与 `--root`，支持同连接连续无请求体 `GET`，默认返回 `Connection: keep-alive`，显式 close 优先；响应按请求顺序逐个排空。
- 已实现严格 CRLF/Host/request-line/Header 解析、16 KiB 请求上限、4 KiB 请求行上限、8 MiB 文件上限，以及 `200/400/403/404/405/500`。
- 静态文件访问以启动时打开的 root fd 为锚点，逐组件使用 `openat`、`O_NOFOLLOW|O_CLOEXEC`，中间目录另用 `O_DIRECTORY`；拒绝 raw/encoded traversal、反斜杠、歧义组件与 symlink escape。
- S2 的短写/EAGAIN 续传、半关闭、`EPOLLERR/SO_ERROR` 同批读取、稳定 identity guard（保留原 generation 防复用语义）、先 epoll DEL 后释放 fd 和连接错误隔离仍由回归测试保护。
- V0.1/S3历史验收：Reviewer 已在全新的 `build-review-s3/` 中完成 Debug 独立构建且告警为 0，CTest `9/9`；REQ-01..08、AC-01..10、RV-01..10 全部通过，唯一结论为 `PASS`。真实集成摘要为 `200:35, 400:6, 403:6, 404:1, 405:1`，分段 `NeedMore=1`、生产写 EAGAIN `=1`、accept-drain `8/8`、reset 后续连接 `20/20`、secret 泄露 `0`。
- V0.2/S1 验收：全新 `build-review-v0.2-s1/` Debug 构建告警 0，CTest `10/10`，REQ-01..08、AC-01..10、RV-01..10 全部通过；事件专项 ASan/UBSan 无报告，真实 HTTP 回归保持原行为。
- V0.2/S2 已完成；其批准、实现、独立验收与关闭证据见 `docs/leader/reports/V0.2/S2-report-002.md`、`docs/builder/reports/V0.2/S2-report-001.md`、`docs/reviewer/reports/V0.2/S2-report-001.md`、`docs/leader/reports/V0.2/S2-report-003.md`。
- 当前生产 listener/connection 通过非 fd owner 的 Channel 注册，由 EventLoop wait、按 registration token 分发完整 mask；Acceptor 独占监听与 accept-drain，TcpConnection 独占 ConnectionIo、Channel、事件诊断、interest 与本地关闭；各 owner 的 ConnectionRegistry 持有连接集合，在本 owner 回调返回后按稳定 identity 回收；TcpServer 在 main 管理监听、固定线程池和轮转交接。
- V0.2/S2 独立验收：全新 Debug 构建告警 0、CTest `11/11`、REQ-01..08/AC-01..10/RV-01..10 全通过；新增组件专项 ASan/UBSan 无诊断。
- V0.2/S3 历史链路（S2会话已替代done）：TcpConnection 发布累计未消费输入/EOF；app 层 HTTP 适配器每连接独立 done，通过 send/consume/close_after_flush 发送并排空；ConnectionIo 仅管理字节读写和缓冲。实现报告见 `docs/builder/reports/V0.2/S3-report-001.md`。
- V0.3/S1 实现：每连接独立 `RequestParser` 接收新字节，NeedMore 时立即消费已接收字节；RequestLine/Headers/Complete/Error 状态保留跨块 CR。`ParseRequest` 使用同一算法。请求行内容恰好 4096 字节的未结束前缀为 NeedMore，4097 内容字节为错误；完整请求头上限 16384 字节（含 CRLF）。
- 增量 parser 的终态保持到显式 reset，并在首个请求头结尾精确停止。生产在响应排空后 reset，先处理 transport 中缓存的后缀，最多保留一个未排空响应。待输出时暂停读取，正常空闲 EOF 静默关闭；初始空 EOF 或部分请求 EOF 返回400后关闭。
- V0.1/S3 历史 Approved 权威包：
  - `docs/leader/designs/V0.1/S3-design.md`
  - `docs/reviewer/reviews/V0.1/S3-review.md`
- S3 Builder 报告：
  - `docs/builder/reports/V0.1/S3-report-001.md`
- S3 最终 Reviewer 证据：
  - `docs/reviewer/reports/V0.1/S3-report-001.md`
- S3 Leader 关闭报告：
  - `docs/leader/reports/V0.1/S3-report-003.md`
- V0.2/S1 Approved 权威包：
  - `docs/leader/designs/V0.2/S1-design.md`
  - `docs/reviewer/reviews/V0.2/S1-review.md`
  - `docs/leader/reports/V0.2/S1-report-001.md`
- S2 最终 Reviewer 证据：
  - `docs/reviewer/reports/V0.1/S2-report-002.md`

规划能力与当前已实现能力必须区分。具体版本顺序、完成门槛和禁止范围以 `ROADMAP.md` 为准。

2026-10-05：R005日志批量修复候选已实现，当前CTest为9项（原8+日志批量专项）；Builder9/9与ASan/UBSan/LSan通过，TSan环境映射限制保留，完整性能矩阵P3延迟门槛失败，独立Reviewer009确认FAIL。候选与预算入口见[日志批量候选说明](../../benchmark/regression/BATCH-CANDIDATE.md)。

