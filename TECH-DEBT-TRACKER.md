# HP HTTP Server 技术债跟踪

2026-10-06：按用户明确指令关闭 V0.5.1/S3，保存并推送当前阶段交付；这是终止本阶段工作，不是验收通过。最终 Reviewer019 **FAIL**，正式 R005 P3 E/C P99=3.2446，未达到≤0.25；慢发送根因未确定，RO-002保持Open，移交用户后续新阶段。R019仅为未批准方案，本阶段不再执行。历史“返工中/不关闭”均为该决定前的检查点，未改写历史结论。

本文档只记录跨阶段技术债、已批准延期和持续风险。普通阶段待办、尚未开始的计划功能和一次性实现缺陷不在此跟踪；它们应写入阶段设计、Builder 报告或 Reviewer 报告。

## 当前概况

- S3阻塞：2026-10-05 R002完整30样本结束，Reviewer006 FAIL；P1/D吞吐跨度53.91%超过20%，P3/D P99中位667.923ms/基线49.136ms=13.593，超过0.25。60阶段零错误，历史计时异常本轮未复现但根因仍未知。停止后续动态，RO-002继续开放。

- 2026-09-28：V0.5.1/S2已完成（Approved revision1、Builder001、独立Reviewer001 PASS）。RO-002已有生产修复和局部性能证据，仍待S3最终独立验收，不提前关闭。
- TD-001/005/006继续Open，TD-002保持Closed。用户注释、31冻结测试和23停用目标保持；新增当前接口专项覆盖本次风险，不等同TD-006整体关闭。
- F-S2-01工具预算漏洞已独立反例验证关闭；首轮Builder测试fixture失败及Reviewer外层构建路径失败保留。无新延期债务或风险豁免。

## 类型与状态

类型：

- `Risk`：尚未形成缺陷，但需要在未来阶段验证或控制。
- `Approved Deferral`：已明确允许推迟到指定阶段的工作。
- `Technical Debt`：为完成当前目标而接受的实现折中，需要后续偿还。

状态：

- `Open`：持续跟踪，尚未满足退出标准。
- `Scheduled`：已经分配到明确版本或阶段。
- `Accepted Risk`：风险已被接受，但仍需保留环境或限制说明。
- `Closed`：退出标准已满足并有证据。
- `Superseded`：被更高权威决策或后续条目替代。

每个条目必须有影响范围、当前决定、目标检查点和退出标准。没有退出标准的普通愿望不得登记为技术债。

## 活动条目

### TD-006 R6 旧测试迁移按需延期

- 类型：`Approved Deferral`
- 状态：`Open`
- 影响范围：R6 生产接口和文件命名变更后，与旧接口绑定的历史测试。
- 决策依据：2026-09-21 用户明确要求旧测试源码、文件名、内部调用和 lambda 不修改；不兼容目标取消构建/注册，未来需要使用时再迁移。R6 设计随后获明确批准。
- 当前决定：保留旧源码及历史验证证据，记录实际停用目标；仍兼容的黑盒测试继续执行，R6 新增专项测试覆盖本轮关键生命周期风险。不得把历史通过视作新代码回归通过，也不得因生产行为缺陷停用兼容测试。
- 责任角色：后续阶段 Leader 决定所需覆盖，Builder 按获批范围迁移，Reviewer 独立验证。
- 目标检查点：后续阶段需要相应模块回归覆盖时，或用户主动要求恢复旧测试时；没有强制全量重写日期。
- 退出标准：需恢复的测试完成独立适配、重新注册且通过当前代码验证；全条目关闭须所有停用测试恢复或获得明确永久替代决定。
- R7 检查点（2026-09-26）：Reviewer001 PASS WITH DEBT、Leader003 收口；仍为原 23 个停用目标、31 个冻结文件，6 项活跃 CTest 独立通过，无新增停用或延期。沿用下述原责任角色、目标检查点和退出标准；12 文件既有格式差异属于批准的内容保护例外，不新增债务。
- R6 当前清单：23 个旧接口耦合 CTest 目标暂退，5 个兼容旧 CTest 保留，新增 1 个专项目标；精确名单与 include 依赖原因见 README“测试与验证”。31 个旧测试路径及内容保持冻结，独立 Reviewer R6-report-001 为 PASS WITH DEBT：6/6 CTest、专项 sanitizer 1/1、HTTP smoke 和47文件格式检查通过，31个旧文件逐一校验一致。Leader R6-report-002 接受此已批准延期并关闭 R6，TD-006 保持 Open。

### TD-001 WSL2 性能数据代表性有限

- 类型：`Risk`
- 状态：`Open`
- 影响范围：`V0.5`、`V0.6`、`V1.0`
- 责任角色：Leader、Reviewer
- 目标检查点：`V0.5 / S4` 建立压测基线时

问题：

WSL2 的网络栈、虚拟化层和观测条件与原生 Linux 服务器不同，直接把 WSL2 数据写成普适性能结论会降低可复现性和简历可信度。

当前决定：

V0.1 至 V0.4 可在 WSL2 开发和验证。性能阶段必须记录 CPU、内存、内核、编译参数、连接模型、wrk 参数和结果摘要；关键结论优先在原生 Linux 或等价环境复测。

退出标准：

V1.0 前存在一份环境完整、命令可复现的性能报告；若无法原生 Linux 复测，README 和简历明确限定数据只代表对应 WSL2 环境。

### TD-002 异步日志延期到性能阶段

- 类型：`Approved Deferral`
- 状态：`Closed`
- 影响范围：`V0.1` 至 `V0.5`
- 责任角色：Leader、Builder
- 目标检查点：`V0.5 / S2`

问题：

过早实现异步日志会引入线程、队列、刷盘、丢弃和关闭语义，干扰 Reactor、连接管理和 HTTP 主线。

历史决定（保留形成依据）：

S1 只实现简单同步日志接口；早期热路径避免高频日志。V0.5/S2 根据已有压测和观测证据决定简化自研方案或继续保持同步。

退出标准：

V0.5/S2 形成 Approved 设计并完成 Reviewer 验收，明确队列上限、过载策略、刷盘与关闭语义，或者用数据证明无需异步化并记录决定。

关闭记录（2026-09-10）：Approved revision1明确1024槽、1024字节正文、全等级丢新、逐条flush和健康sink排空；Reviewer001独立8AC/RV、25/25、六sanitizer及四反证通过，Leader003关闭本条。阻塞stderr最终join不保证受HTTP截止限制为PM整体批准边界；flush不承诺fsync。没有采用无数据的“保留同步”替代，也没有宣称吞吐提升。

### TD-003 HTTP Parser 范围膨胀风险

- 类型：`Risk`
- 状态：`Closed`
- 影响范围：`V0.1 / S3`、`V0.3` 及后续
- 责任角色：Leader
- 目标检查点：`V0.1 / S3`、`V0.3 / S1` 已完成；`V0.3 / S2` 复用前 framing 矩阵审批与验收已完成；V0.3/S3及版本退出条件于2026-09-09经独立Reviewer确认满足

问题：

HTTP/1.1 细节包含 body、chunked、pipelining、Range 和缓存协商。项目主线是网络服务器内核，不是完整 Web 框架。

历史决定（保留形成依据）：

S3 已按 Approved revision 1 交付单请求 GET、严格 Header 边界和 `Connection: close`，并保持 body/chunked、pipelining 第二响应、URL decode 和完整状态机在范围外；Reviewer 的 RV-01..10 及非零动态证据全部通过，S3 检查点关闭。V0.3/S1已按Approved矩阵交付增量解析、半包/粘包首边界/reset、上限和非法输入验证，Reviewer确认无越界，当前解析检查点满足既有退出要求。S2已按单独批准的矩阵及接口补充交付keep-alive，并通过全部12AC与独立Reviewer PASS；复用framing检查点完成。持续协议范围风险保留Open，不授权body/chunked或扩大协议范围。其他协议能力必须由新的版本范围明确批准。

退出标准：

V0.3 的 Approved 设计列出支持矩阵与禁止范围，测试覆盖半包、粘包、超限和非法请求，Reviewer 确认没有越界实现。

关闭记录（2026-09-09）：S3 Reviewer001 RV01..08确认V0.3 Approved矩阵、半包/粘包/超限/非法输入测试及无越界实现，满足上述退出标准；Leader004据此关闭本条受控范围内风险，不是接受未解决缺陷。未来范围膨胀的可能性并未消失，后续协议能力仍须独立批准并遵守仓库范围门槛；本次不授权body/chunked或完整HTTP支持。

### TD-004 L7 Gateway 范围膨胀风险

- 类型：`Risk`
- 状态：`Open`
- 影响范围：`V1.1`
- 责任角色：Leader
- 目标检查点：进入 `V1.1` 前

问题：

代理能力容易扩张到服务发现、动态配置、限流、熔断、灰度和 TLS 终止，可能削弱 HTTP Server 内核主线。

当前决定：

V1.1 是 V1.0 之后的可选扩展。只有用户再次批准时才启动，默认仅考虑轻量路由、upstream 选择、HTTP 转发、超时和错误处理。

退出标准：

V1.0 已完成，用户明确批准 V1.1，且 Approved 设计包含禁止范围、代理测试和性能隔离方案。

### TD-005 文档与实现漂移风险

- 类型：`Risk`
- 状态：`Open`
- 影响范围：所有版本和阶段
- 责任角色：Leader、Builder、Reviewer
- 目标检查点：每个阶段验收时

问题：

阶段设计、实现、审查、README、架构和路线图可能随推进出现不一致，导致错误实现或虚假能力描述。

当前决定：

使用 `Draft/Approved/Superseded` 生命周期和 `REQ/AC/RV` 追溯。Builder 报告必须记录设计差异，Reviewer 必须检查文档一致性，阶段通过后由 Leader 同步状态文档。`2026-09-01` 的 S2 与 `2026-09-03` 的 S3 检查点均已按此流程完成。V0.2/S1 revision 1 已建立一致的 8 REQ、10 AC、10 RV 与根状态，并于 `2026-09-07` 登记为 Approved；现有 Builder 001、Reviewer 001 唯一 PASS 及 Leader 003 同步收口证据，本阶段 P3-01 与 TD-005 同步检查点已关闭。V0.2/S2 同样已于 `2026-09-08` 具备 Approved、Builder 001、Reviewer 001 PASS 与 Leader 003 收口证据，S2 同步检查点关闭。V0.2/S3及版本收口也已形成Approved、Builder001、Reviewer001 PASS及Leader003，P3-01/P3-02关闭；TD-005 继续作为跨阶段治理风险保持 `Open`。V0.4/S1 于 2026-09-09 具备 Approved、Builder001、独立 Reviewer001 PASS 与 Leader003，同步检查点及 P3-01 关闭，无新增债务或风险豁免。

退出标准：

这是持续治理风险，不在单一版本永久关闭。若连续完成的阶段均有 Approved 基线、完整报告、唯一 Reviewer 结论和同步状态，可在 V1.0 评估为 `Accepted Risk` 或 `Closed`。

## 风险观察

风险观察用于提示未来设计，不代表已经接受技术债。

### RO-001 初始目录和 CMake 结构验证

- 检查点：`V0.1 / S1`
- 当前结论：已由 Builder 实现并由 Reviewer 在独立构建目录验证；RV-01、RV-02、RV-06 均通过，未升级为技术债。
- 转换规则：S2 设计若需要改变既有模块边界，必须先由 Leader 明确设计并按批准流程处理。

### RO-002 性能目标尚未量化

- 2026-10-06：R017/R018宿主只读查询失败记录保留；独立离线确认Hyper-V-Hypervisor注册和正常清理，但不证明采集权限、目标WSL调度或生产根因。R019同步观测及额度扩展仍Draft；正式FAIL/RO-002开放，无新风险豁免。见[Builder020](benchmark/results/V0.5.1/S3/S3-builder-020.md)与[Reviewer019](benchmark/results/V0.5.1/S3/S3-reviewer-019.md)。

- 2026-10-06：R016的67021对sendto和221条同期内核样本独立核验完整、零丢失；旧240.784ms未复现。本轮7.058ms调用仅近返回一点定位于TCP发送，70.166ms间隙仅一次快照确认epoll_wait，原长尾根因仍未知，不能据此修改Logger或判定宿主暂停。正式P3 FAIL、RO-002开放。见[Builder018](benchmark/results/V0.5.1/S3/S3-builder-018.md)与[Reviewer017](benchmark/results/V0.5.1/S3/S3-reviewer-017.md)。

- 2026-10-06：R014独立确认完整sendto240.784ms（105B成功，未记录完整调度切出）及futex7.868ms/等待地址。事件零丢失且完整排空，定位收窄到调用内部；具体内核/宿主原因和锁对象仍未知，不直接修改Logger或宣称纯CPU。Reviewer015正式FAIL、RO-002开放；见[Builder016](benchmark/results/V0.5.1/S3/S3-builder-016.md)与[Reviewer015](benchmark/results/V0.5.1/S3/S3-reviewer-015.md)，无新增风险豁免。

- 2026-10-06：R013校正准入通过各90000对独立核验，两个短样本已完成，选定两worker时间线零丢失/无残留。server相邻发送间隙137.954ms主要阻塞至唤醒，另123.350ms仅0.057ms offCPU，具体调用/锁地址及运行残差原因仍未知。控制/观测P99差72.44%超扰动阈值，不能外推收益；独立Reviewer014保持正式FAIL。见[Builder015](benchmark/results/V0.5.1/S3/S3-builder-015.md)与[Reviewer014](benchmark/results/V0.5.1/S3/S3-reviewer-014.md)。 RO-002开放，无新增风险豁免。

- 2026-10-06：获批R012精简采集已预审，唯一微型在约60.672ms突发、实际258KiB/CPU缓冲下丢21284事件，按停止条件两个HTTP样本均未启动。小缓冲突发不代表约4MiB的计划真实观测，不能据此判断P3必败或服务器根因。独立Reviewer013维持FAIL；原R005有效30样本及P3未达标保留。见[Builder014](benchmark/results/V0.5.1/S3/S3-builder-014.md)与[Reviewer013](benchmark/results/V0.5.1/S3/S3-reviewer-013.md)。 RO-002保持开放，未新增风险豁免。

- 2026-10-05：R009–R011隔离观测已结束。私有tracefs与实际线程PID映射可用，但两个真实负载观测样本均大量丢事件；修正采集器递归容量检查后，最后高频微型仍丢48185事件，未满足完整观测准入。三次HTTP尝试仅A1有效，原ABBA未成立，A4未执行。服务器具体根因仍未知，正式S3仍返工中/FAIL，无新生产修复或门槛调整。见[Builder013](benchmark/results/V0.5.1/S3/S3-builder-013.md)与[Reviewer012](benchmark/results/V0.5.1/S3/S3-reviewer-012.md)。 RO-002保持开放，无新增风险豁免。

- R006诊断/R007工具纠正检查点（2026-10-05）：12个有效样本证明日志共享锁有长获取区间，但单机制ABBA去除逐响应info仍有长尾，不能将日志归为完整根因。双端实际停顿扩至运输调用/epoll间区间，内核/调度/虚拟化具体触发未确定；原工具初始化失败保留，新工具最小纠正补齐。合计334.522s（含失败与初始化保守15s）≤480，无新增预算；正式S3 FAIL保持，生产候选/门槛不改。见[Builder011](benchmark/results/V0.5.1/S3/S3-builder-011.md)与[Reviewer010](benchmark/results/V0.5.1/S3/S3-reviewer-010.md)。
- 2026-10-05：R008观测能力核验已结束：基本ptrace可用，host调度时间线接口仍缺，未满足准入故未启动ABBA；独立Reviewer011复核，正式S3仍FAIL。见[Builder012](benchmark/results/V0.5.1/S3/S3-builder-012.md)与[Reviewer011](benchmark/results/V0.5.1/S3/S3-reviewer-011.md)。 RO-002保持未关闭，未新增延期或风险豁免。
- R005修复检查点（2026-10-05）：用户批准日志批量输出及每角色新增1800s/2GiB预算，旧累计/失败保留。候选已实现，Builder9/9及ASan/UBSan/LSan通过，TSan初始化mapping限制仍无法验证；唯一C/E矩阵30样本有效，但P3 P99 E/C=3.2446未达0.25，其他场景及跨度合规；动态停止、独立Reviewer009确认FAIL，详Leader016。新增使用778.879s与661,538,066B，历史保留。风险与S3/版本保持未关闭，原门槛不变。以下为历史检查点。

- 定位补充（2026-10-05，R003/R004）：压测线程2→4未稳定改善，未见worker长期闲置；日志file/null/null/file P99约121/32/57/133ms，总CPU下降集中于创建顺序推定的logger线程，支持sink成本参与。不能据此认定具体锁/磁盘机制，null仍未达原门槛；P1两样本未复现波动不等于修复。全部动态停止，生产未改、风险未关闭，详见Leader013和Reviewer007/008。

- R002检查点（2026-10-05）：R002完整运行形成新的数值失败证据，P3高并发长尾和P1吞吐波动原因尚未定位；不能由CPU数值直接归因。见Builder007、Reviewer006及Leader011。Reviewer独立完整套未执行，Builder剩余384.726s不足900s整套预留；后续须形成有界定位方案及预算授权，不重置历史账本或降低门槛。S2局部结果保持，S3和版本不关闭。

- S3历史检查点（2026-09-28）：Builder001/002正式套分别只有13/6个完整样本，64KiB/C测量均timeout32，raw时长54.488s/31.247s而配置20s；UTC与monotonic差约34.383s/11.199s。两套invalid，不能判定五场景门槛或具体原因；Reviewer004确认阶段BLOCKED，F-S3-01/02工具缺陷已关闭但不替代性能验收。停止第三套，原预算/门槛保持，R001有限计时诊断R001已批准，返工中。S2局部修复证据不撤销，但不能代替S3整体验收。

- 2026-09-28 S2最新结论：TCP_NODELAY修复独立PASS。1KiB QPS 716.522→37200.425、P99 48.399→2.024ms；大文件吞吐比1.003842/P99比1.064837，全部组跨度≤20%，默认客户端正文等待0.27–0.28ms。CPU成本与环境边界保留。状态仍未关闭，下一检查点为V0.5.1/S3扩展性能验收与版本收口。下方S1/准备记录保留其原时点含义。


- S2准备（2026-09-28）：TCP_NODELAY最小修复与6项验收为Draft；局部目标为小文件吞吐≥基线10倍、P99≤25%，大文件吞吐≥90%/P99≤125%，默认客户端正文等待中位<5ms，须批准后执行。异常保持未关闭，S3仍负责最终独立验收。

- 2026-09-28 最新检查点：V0.5.1/S1完成（Approved revision1、Builder002、Reviewer002 PASS）。独立18正式样本确认C小文件715.070 QPS、每轮P99中位数48.392ms；v0.5-s1中间版本711.703 QPS。默认正文等待约42–43ms，客户端QUICKACK单因素后约0.25–0.47ms，sendfile本身快速返回；证据支持头部/正文拆分发送与ACK等待交互，未直接观测内核Nagle状态。
- 下一检查点：V0.5.1/S2修复设计与S3独立性能验收。RO-002保持未关闭，不能将诊断PASS视为性能修复或门槛已批准。首次工具ASLR误判与Reviewer001 F-01 FAIL保留，最终身份核验缺陷已独立关闭；无新增风险豁免。

- 检查点：`V0.5 / S4`
- 当前结论：S4固定基准已独立有效复现。1KiB A/B中位QPS21373.88/713.56，B/A0.033385（约下降96.66%，A跨度46.11% noisy）；Builder003也观测下降96.92%。1MiB B/A1.019197、双方noisy。明显小响应下降及未知根因保持观察，不能从PASS推导性能改善或豁免调查。
- 当前检查点（2026-09-28 用户授权调整）：V0.5.1/S1 优先完成历史 A/B 与当前版本复现及有限机制诊断，取代原先等待 V0.6 的安排；V0.5/S4 历史验收不变。
- 转换规则：先验证复现、等待/热点与机制，再确定 S2 修复及 S3 数值门槛；不得直接归因 sendfile/logger/Buffer。无法复现保留观察，不自动关闭；若确认缺陷或需要延期，由新阶段设计明确处理，不追改历史验收。

### RO-003 L7 Gateway 与独立 L4LB 边界

- 检查点：进入 `V1.1` 前
- 当前结论：本仓库只考虑 HTTP 层转发；独立 L4LB、XDP、DPDK 不进入本项目。
- 转换规则：若用户目标转向 L4，应另立项目或路线，不在本仓库混合实现。

## 阶段边界

S1 关闭条件已经满足：

- S1 设计为 `Approved`。
- S1 审查计划为 `Approved`。
- Builder 报告 002 提供完整实现与自测证据。
- Reviewer 报告 001 独立完成 RV-01 至 RV-07，CTest `3/3` 通过，唯一结论为 `PASS`。
- P0、P1、P2 均无；唯一 P3 顶层状态同步已在本次 Leader 关闭工作中处理。
- S1 没有新增技术债或影响关闭的遗留项。

S2 关闭条件已经满足：

- S2 design/review revision 2 为 `Approved`。
- Builder 报告 001 完成实现，报告 002 闭环首轮 Reviewer P2-01。
- Reviewer 报告 001 的 `FAIL` 作为历史保留；Reviewer 报告 002 全量复审 RV-01 至 RV-08，唯一结论为 `PASS`。
- CTest `6/6`、P2-01 专项 `100/100`、全量稳定性 `60/60` 通过；P0/P1/P2、无法验证项和新增技术债均无。
- P3-01 根状态漂移已由本次 Leader Closing 同步关闭。

S3 关闭条件已经满足：

- S3 design/review revision 1 为 `Approved`，批准日期 `2026-09-03`。
- Builder 报告 001 提供完整实现与自测证据。
- Reviewer 报告 001 在全新 `build-review-s3/` 中完成 RV-01 至 RV-10，CTest `9/9`，唯一结论为 `PASS`。
- 真实 HTTP 集成 `10/10`，状态 `{200:35,400:6,403:6,404:1,405:1}`，生产写 EAGAIN 非零、secret 泄露为 0、隔离 `20/20`、fd `6->6`；静态文件 `openat/close 530/530`，S2 关键组合事件回归仍成立。
- P0/P1/P2、无法验证项和新增技术债均无；P3-01 已由本次 Leader Closing 同步关闭。
- TD-003 的 S3 检查点已完成但条目保持 `Open` 至 V0.3/S1；TD-005 的 S3 同步检查点已完成但作为持续治理风险保持 `Open`。
- 既有慢读、无全局高水位和无超时治理边界保持不变，不阻塞 V0.1 关闭。

V0.2/S1 关闭证据（2026-09-07）：

- Approved revision 1 的批准、Builder 001 实现与 Reviewer 001 独立 `PASS` 均齐备；S1 已完成，V0.2 进行中，S2/S3 未开始。
- 独立 Debug CTest `10/10`、告警 0；真实 stale/fd reuse 隔离、Channel ERR|IN → SO_ERROR → recv 1053 字节、HTTP EAGAIN/隔离/fd 稳态与事件专项 sanitizer 均通过。
- P0/P1/P2、必需未验证项、返工与新增债务：无。P3-01 由 Leader report-003 根文档同步关闭；TD-005 仅关闭本阶段检查点，条目继续 `Open`。

V0.2/S2 关闭证据（2026-09-08）：

- Approved revision 1、Builder 001 四里程碑、Reviewer 001 全部 REQ-01..08/AC-01..10/RV-01..10 PASS 已齐备。
- 独立 CTest 11/11、告警 0；内核队列 8 单轮 drain、65536 字节 EAGAIN 恢复、回调后销毁、真实 fd/旧 identity 隔离、新连接 ERR/IN→SO_ERROR→recv1053、HTTP fd6→6 与专项 sanitizer 均通过。
- P3-01 与 TD-005 本阶段同步检查点由 Leader 003 关闭；无新债务，不关闭既有持续风险。S3 未开始。

V0.2/S3与版本关闭证据（2026-09-08）：

- Approved基线、Builder四里程碑、Reviewer全部REQ/AC/RV及ROADMAP版本标准PASS、Leader003收口齐备。
- 独立CTest12/12、告警0；新消息生产响应7、交错唯一响应2、524390字节EAGAIN完整恢复、第二响应0、ERRIN后消息1053字节、原S1/S2/HTTP/curl全回归与专项sanitizer通过。
- P3-01/P3-02及TD-005当前检查点关闭，无新增债务；静态文件root fd保留。V0.2已完成，V0.3未开始。

## V0.3/S1 关闭检查点

- 2026-09-08：Approved支持矩阵19类、708split、六长度边界、粘包/reset/非法请求与生产单响应验证通过；TD-003当前解析检查点完成，无越界。S2启用复用前仍须单独批准framing支持/拒绝矩阵，CL/TE/Connection仅语法检查不表示支持body。
- TD-005：Approved、Builder001、Reviewer001 PASS、Leader003及根状态同步齐备，本阶段检查点完成，持续治理风险Open。
- 无新债务、必需未验证项或阻塞；S1已完成，V0.3整体进行中，S2/S3未开始。

## V0.3/S2 关闭检查点

- 2026-09-08：依据Approved design/review revision1及rework001、Builder001、Reviewer001唯一PASS、Leader004，TD-003复用framing检查点完成；矩阵拒绝与真实复用、close/EOF/内存有界全部验证。
- TD-005：批准、接口补充、实现/独立审查及根状态同步齐备，本阶段检查点与Reviewer P3-01关闭。两个条目保持Open，无新债务或延期。
- S2已完成，V0.3整体进行中，S3未开始；没有发布操作或版本完成声明。

## 关闭与更新规则

- 技术债只有在退出标准满足且存在 Builder/Reviewer 证据后才能关闭。
- `PASS WITH DEBT` 中的条目必须给出负责人、目标阶段和退出标准。
- 阶段未实现的计划功能不创建技术债。
- Reviewer 发现的当前阶段必需缺陷优先返工，不得直接登记技术债规避验收。
- 条目状态变化时追加更新记录，不覆盖形成决策时的原因。

## V0.3/S3 检查点（已完成）

- 2026-09-08：基于main 93f305a/v0.3-s2形成S3 Draft设计与审查计划；协议异常矩阵和独立证据方案已准备，尚未实现或验收。
- TD-003：批准后按S3矩阵验证协议范围，版本退出条件需验收后决定；TD-005：批准、实现、独立审查及Leader收口待后续完成。两项保持Open，无新增债务或风险接受。

- 2026-09-09完成：Approved、Builder001、Reviewer001 PASS及Leader004齐备，P3-01与TD-005阶段检查点关闭；TD-003按实际退出条件Closed，TD-005持续Open。上列2026-09-08为准备时历史快照。

## 变更记录

- `2026-09-09`：准备 V0.4/S2 Draft revision 1，解释首次线程池的固定投递界限与S4一般治理边界；TD-005持续Open，S1检查点保留完成，无新增债务。

- `2026-09-09`：准备 V0.4/S1 Draft revision 1，TD-005 持续 Open；未新增债务或批准延期，任务容量治理保留 S4 范围。

- `2026-09-09`：依据S3 Builder001、独立Reviewer001 PASS及Leader004关闭S3、V0.3与P3-01；TD-003按退出条件Closed，TD-005当前检查点完成并持续Open；V0.4未开始。

- `2026-09-09`：依据PM“批准，开始工作”及Leader V0.3/S3-report-002登记S3 revision1 Approved、待实现；范围及既有架构不变，未新增验收或债务关闭声明。

- `2026-09-08`：准备V0.3/S3 Draft revision1设计、审查与Leader001；S3设计中待批准，S1/S2已完成，V0.3尚未完成。

- `2026-09-08`：依据V0.3/S2 Builder001、Reviewer001 PASS和Leader004关闭S2及P3-01，TD-003/005当前检查点完成但持续Open；V0.3进行中、S3未开始。

- `2026-09-08`：依据PM原话“批准，进行开发”及Leader V0.3/S2-report-002，将S2 design/review revision1登记Approved，当前待实现 / Ready for Builder；无功能验收或新增债务。

- `2026-09-08`：依据Leader V0.3/S2-report-001登记S2 Draft矩阵与规划同步；TD-003/005仍Open，无新增债务或本阶段关闭结论。

- `2026-09-08`：依据V0.3/S1 Builder001、Reviewer001 PASS及Leader003关闭S1/P3-01和TD003/005当前检查点；V0.3整体进行中，S2/S3未开始，无新债务。

- `2026-09-08`：依据PM批准及Leader V0.3/S1-report-002，design/review revision1登记Approved，当前待实现；V0.3/S2/S3未开始，无新增债务。

- `2026-09-08`：准备 V0.3/S1 Draft revision 1，当前阶段设计中等待批准；V0.2 已完成，功能代码未变。

- `2026-09-08`：依据S3 Builder001、Reviewer001唯一PASS与Leader003，关闭S3和整个V0.2、P3-01/P3-02及TD-005检查点；V0.3未开始，无新增债务。

- `2026-09-08`：依据 PM 当前批准和 Leader S3-report-002，V0.2/S3 revision1 登记 Approved，当前待实现；V0.2整体进行中，无新增债务。

- `2026-09-08`：依据 S2 Builder 001、Reviewer 001 唯一 PASS 与 Leader 003，关闭 V0.2/S2、P3-01 和 TD-005 阶段检查点；V0.2 进行中，S3 未开始，无新增债务。

- `2026-09-08`：依据 PM 明确批准与 Leader S2-report-002，V0.2/S2 revision 1 登记 Approved，当前待实现；S1 已完成，S3 未开始，无新增债务。

- `2026-09-07`：依据 V0.2/S1 Builder 001 与 Reviewer 001 唯一 `PASS`，Leader report-003 关闭 S1 和 P3-01；V0.2 整体进行中、S2/S3 未开始，无新增债务。

- `2026-09-07`：登记 PM 批准 V0.2/S1 revision 1，状态同步为`待实现 / Ready for Builder`；批准依据见 Leader S1-report-002，尚未实现或验收。

- `2026-09-03`：同步 V0.2/S1 Draft revision 1 与`设计中 / Awaiting PM Decision`；记录 TD-005 的规划一致性检查，无新债务或状态关闭。
- `2026-09-03`：依据 S3 Reviewer 报告 001 的唯一 `PASS` 关闭 S3 阶段检查点与 P3-01；记录 TD-003/TD-005 的本阶段检查结果，两个持续风险条目仍保持 `Open`，未新增技术债。
- `2026-09-03`：记录 PM 批准 S3 revision 1；状态推进到 `待实现 / Ready for Builder`，未新增债务或完成声明。
- `2026-09-03`：同步 S3 Draft revision 1 与 `Awaiting PM Decision`；扩展 TD-003 的 S3 检查点，但未把未实现功能或普通待办登记为新技术债。
- `2026-09-01`：依据 S2 Reviewer 复审 `PASS` 完成阶段边界同步，关闭 P3-01 与 TD-005 的本阶段检查点；未新增技术债，下一步进入 S3 设计。
- `2026-08-27`：同步 S2 Draft 已形成、活动阶段为 `设计中` 且 `Awaiting PM Decision`；未新增技术债。
- `2026-08-25`：依据 S1 Reviewer `PASS` 完成阶段边界同步，关闭 RO-001 的观察检查点；S1 未产生新技术债，下一步进入 S2 设计。
- `2026-08-24`：将文档重构为跨阶段技术债与风险清单；同步 S1 为待实现；标准化类型、状态和退出标准；移除不属于技术债范围且与用户本地协作策略冲突的旧条目。
- `2026-05-25`：同步初始 S1 设计状态。
- `2026-05-21`：初始化 TD-001 至 TD-005 和风险观察。

- `2026-09-09`：依据 PM 批准与 Leader V0.4/S1-report-002 登记 S1 Approved revision 1；TD-005 持续 Open，无新增债务、风险豁免或延期。

- `2026-09-09`：V0.4/S1 经 Reviewer001 PASS、Leader003 完成，关闭 P3-01 与 TD-005 当前检查点，TD-005 持续 Open；队列容量治理保留 S4，未新增债务或必需未验证项。

- `2026-09-09`：依据 PM 明确批准及 Leader V0.4/S2-report-002 登记 S2 Approved；TD-005持续Open，无新增债务、风险豁免或延期。

- `2026-09-09`：V0.4/S2经Reviewer001 PASS、Leader003收口，P3-01及TD-005本阶段检查点关闭；TD-005持续Open，固定池容量不等于活跃连接全局配额，S3/S4未开始。

## V0.4/S3 准备检查点（设计中）

- 2026-09-09：S2已合并并推送v0.4-s2标签；S3设计/审查Draft revision1及Leader001形成，尚无实现或验收。
- TD-005持续Open：批准、实现、独立审查及收口仍待执行；现有S2检查点保留完成。timer索引取消/续期、fd身份、超时策略与真实生产验证均是本阶段任务，不另登记延期债务。
- 不承诺请求绝对时限、最低速率、阻塞provider抢占或完整慢连接防护；S4高水位和优雅关闭仍未开始。

- `2026-09-09`：依据PM批准与Leader V0.4/S3-report-002登记S3 Approved；TD-005持续Open，无新增债务、风险豁免或延期。

## V0.4/S3 关闭检查点

- 2026-09-09：Approved、Builder001、Reviewer001 PASS与Leader003齐备，8REQ/12AC/RV和8条生命周期通过，P3-01及TD-005本阶段检查点关闭。TD-005持续Open，无新增债务、风险豁免或必需未验证项；S4未开始，V0.4未完成。

## V0.4/S4 准备检查点（设计中）

- 2026-09-09：S3经PR #12合并，v0.4-s3已发布并核对；S4 design/review为Draft revision1，见Leader S4-report-001。
- TD-005持续Open，S3检查点保持关闭；S4待批准、实现、独立审查与收口。本阶段输出上限、任务边界和关闭验证属于正常阶段任务，无新增债务或批准延期。
- ROADMAP当前未开始快照暂保留，实际已进入Draft设计中；批准后同步全局状态。无全局连接/捕获字节配额、阻塞回调不可抢占等限制必须在最终说明中保留。

## V0.4/S4 批准检查点

- 2026-09-09：PM批准design/review revision1，见Leader S4-report-002；当前待实现，无新增债务或风险豁免，TD-005持续Open。实现/独立验收/收口后才更新完成检查点。

## V0.4/S4与版本关闭检查点

- 2026-09-09：Reviewer002独立PASS、两P2关闭；Leader003完成P3-01同步及“七项”计数勘误，按ROADMAP实际六条满足退出条件，关闭S4/V0.4。TD-005阶段检查点关闭、风险持续Open，无新增债务/延期；V0.5未开始。

## V0.5/S1 准备检查点（设计中）

- 2026-09-10：前置v0.4-s4已发布，S1设计/审查为Draft revision1，见Leader V0.5/S1-report-001。TD-005持续Open，当前阶段待批准、实现和独立验收，无新增债务或批准延期。
- TD-001保留V0.5/S4压测检查点，TD-002保留S2日志检查点；sendfile机制验证不替代性能数据。
- S4的测试有效性修复已完成，不另登记为未关闭债务；本次M0明确稳定TID/owner握手、注入负对照和准备时限隔离，避免重复同类夹具问题。

## V0.5/S1 批准检查点

- 2026-09-10：PM批准design/review revision1及Leader001，登记见Leader002；当前待实现，TD-001留S4、TD-002留S2、TD-005持续Open，无新增债务或风险豁免。

## V0.5/S1 关闭检查点

- 2026-09-10：Reviewer002 PASS、Leader003收口，P2-01和P3-01关闭；TD-005本阶段检查点完成但风险持续Open，无新增债务。TD-001保持S4压测、TD-002保持S2日志，均不由sendfile机制提前关闭。

## V0.5/S2 准备检查点（设计中）

- 2026-09-10：S2 design/review为Draft revision1，见Leader S2-report-001。TD-002持续Open：拟有界异步队列、满队列丢新、健康sink排空；阻塞stderr可能拖延最终join的边界尚待设计批准，未批准风险豁免。
- TD-002只能在Approved与独立验收后关闭；TD-005持续Open，TD-001仍留S4，本轮无新增批准延期。

### V0.5/S2 批准登记

- 2026-09-10：PM整体批准design/review revision1，登记见Leader S2-report-002；当前待实现 / Ready for Builder。阻塞stderr可能拖延最终join是已批准方案边界，不豁免HTTP drain及其余生命周期验收。
- TD-002仍Open，须实现、独立验收及Leader收口才关闭；TD-005持续Open，TD-001保留S4。新增延期债务：None。

## V0.5/S2 关闭检查点

- 2026-09-10：Reviewer001 PASS、Leader003完成根状态及架构兼容入口澄清，P3-01与TD-002关闭。TD-005本阶段检查点完成，持续风险仍Open；TD-001留S4。
- 新增债务、延期或风险豁免：None。S3/S4未开始，V0.5未完成；S2提交及推送由父协调者在收口后执行。

## V0.5/S3 准备检查点

- 2026-09-10：S3 design/review Draft revision1及Leader001决策包已准备，待整体批准。当前计划优化Buffer复制与空闲容量保留并验证已有背压，不新增全局配额/最低速率或S4性能要求。
- TD-002保持Closed，TD-001留S4，TD-005持续Open；S2检查点保持完成，S3待批准、实现、独立审查及收口。新增延期债务或风险豁免：None。

## V0.5/S3 批准登记

- 2026-09-10：PM整体批准S3 design/review revision1，见Leader S3-report-002；当前待实现 / Ready for Builder，>64KiB完全空闲输出释放/重复大输出重分配为已批准取舍。
- TD-002保持Closed，TD-001留S4，TD-005持续Open；S3检查点待实现、独立验收及收口。新增延期债务或风险豁免：None。

## V0.5/S3 关闭检查点

- 2026-09-10：Reviewer002以精确两换行及其他源/二进制/证据不变关闭P2-01并给出最终PASS；Leader003同步根文档与阶段状态，P3-01及TD-005本检查点完成。001 FAIL历史保留，不把八sanitizer或27/27称作002重新执行。
- TD-005风险持续Open，TD-002保持Closed，TD-001留S4；新增延期债务、风险豁免和未验证必需项：None。64KiB取舍及原cold-file/阻塞stderr边界保持，S4未开始，V0.5未完成。

## V0.5/S4 准备检查点

- 2026-09-10：S4 design/review Draft revision1及Leader001就绪，待批准；TD-001当前WSL2基线检查点将记录完整环境、有限比较及适用范围，风险持续Open，不以本阶段代替原生复测。
- RO-002仍未有本阶段实测值，不设QPS通过门槛；完整基线后再记录真实数值/噪声，不预设优化收益。wrk缺失为实现前置，准备期不安装。
- TD-005持续Open，S3检查点已完成，S4待实现/独立复现/收口；TD-002保持Closed。新增延期债务或风险豁免：None。

## V0.5/S4 批准登记

- 2026-09-10：PM“批准”整体S4 revision1，见Leader S4-report-002；固定两版本/两文件/三轮WSL基线及wrk仓库内获取已批准，当前待实现。
- TD-001/TD-005持续Open，RO-002尚无S4实测值，TD-002保持Closed。新增风险豁免、延期债务：None；五项版本退出条件仍待实现、独立核对和Leader收口。

## V0.5/S4 R001批准登记

- 2026-09-10：PM在Leader003预算包后回复“批准”，原S4基线及R001/审查补充组成当前执行权威；2GiB累计日志与4GiB启动磁盘约束已批准，待Builder002实施并完整重跑。formal-001失败不计作完成基线。
- TD-001/TD-005持续Open、TD-002Closed，RO-002待完整实测；新增延期债务、风险豁免：None。尚未独立验收或关闭版本。

## V0.5/S4 收口检查点

- 2026-09-10：Reviewer002 PASS与Leader005确认五项版本条件齐备，S4/V0.5关闭；TD-001环境限定和TD-005同步检查点完成，风险继续Open，TD-002Closed。RO-002更新实际下降与不确定性；没有新增风险豁免或自动调优。

## R1重构收口检查点

- 2026-09-17：Approved R1 design/review revision2与R001，Builder001、独立Reviewer001 PASS及Leader005齐备；现行接口/调用链/format命令与README、架构和路线状态同步，TD-005本轮检查点关闭，TD-005总体继续Open。无新增债务、延期或风险豁免。
- TD-001继续Open，TD-002保持Closed，RO-002既有小文件下降及未知根因不变；未做性能声明。baseline构建应放在导出源码内baseline/build-debug，双方已实际验证；这是命令路径勘误，不是产品缺陷或新增跨阶段债务。

## R2重构收口检查点

- 2026-09-19：Approved R005、Builder004、独立Reviewer003 PASS与Leader007齐备，关闭TD-005的R2文档/实现同步检查点；TD-005总体继续Open，后续范围仍需按实际交付同步。旧F02按R005限定矩阵关闭，历史失败保留；无新增必需返工、延期债务或性能风险豁免。TD-001、TD-002及RO-002原状态不变。R2未提交推送，后续阶段未自动启动。

## R2最终回调注册收口检查点

- 2026-09-19：Approved R006、Builder007/008、独立Reviewer005 PASS与Leader012完成当前R2验收及TD-005文档同步检查点。F-C01关闭，原F02在当前候选规定矩阵通过；保留Reviewer004 FAIL、停工与后续用户窄范围授权，不把历史R005结论当作最终符合性证据。TD-005总体继续Open；TD-001、TD-002、RO-002及剩余R3/R4/R5不变。无新延期或性能风险豁免；未提交推送。

## R3 ConnectionIo收口检查点

- 2026-09-19：Approved R3 revision1、Builder001、独立Reviewer001 PASS及Leader003齐备，七文件命名/三处显式结果迁移与现行接口文档同步，TD-005本轮检查点关闭但总体继续Open。无新债务、延期、风险豁免或性能声明；R3返工0/2，未提交推送。TD-001/TD-002/RO-002不变；剩余R4应用辅助与R5日志/整体一致性未启动。R2已提交合并并发布refactor-r2，历史未发布检查点保留原时点含义。

## R4 CLI与信号辅助收口检查点

- 2026-09-19：Approved R4 revision1、Builder001、独立Reviewer001 PASS及Leader003齐备，四文件迁移与状态同步，TD-005本轮检查点关闭、总体仍Open。源码返工0；Reviewer格式参数及Leader审查构建清单遗漏造成两次证据纠正2/2，已补齐且保留首批NOT RUN。无新债务、风险豁免或性能声明；R4未提交推送，R5未启动。TD-001/TD-002/RO-002不变；R3已合并发布refactor-r3。

## R5与整体渐进重构收口检查点

- 2026-09-19：Approved R5 revision1、Builder001、Reviewer001 PASS及Leader003完成日志迁移和规定生产/测试入口一致性，R1–R5批准的渐进计划关闭。TD-005此次同步检查点关闭；其退出标准明确属于持续治理风险、需V1.0评估，故总体仍Open而非据本次永久关闭。TD-001/TD-002/RO-002不变，无新债务或风险豁免。明确允许的同步谓词、标准deleter/API/seam及历史未改目标fixture不是新增延期；不声称全测试风格迁移。R5源码返工0、路线纠正1/2，未提交推送；R4已合并发布refactor-r4。

## 全测试清理收口检查点

- 2026-09-19：Approved final-test-cleanup revision1、Builder002、独立Reviewer002 PASS与Leader002完成全部29个测试文件及窄范围观察接口的审查与文档同步。FS-01–04/FTC-01均关闭，原final-static001 FAIL保留为历史；历史fixture不再是当前豁免。全75格式、独立Debug28/28与sanitizer3/3通过，两轮纠正2/2成功且原失败保留。TD-005本次检查点关闭，总体仍按V1.0持续治理退出规则保持Open；TD-001/TD-002/RO-002不变，无新债务、延期或风险豁免。R5及清理尚未提交推送。
