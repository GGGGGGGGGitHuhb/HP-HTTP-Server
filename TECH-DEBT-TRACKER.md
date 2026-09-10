# HP HTTP Server 技术债跟踪

本文档只记录跨阶段技术债、已批准延期和持续风险。普通阶段待办、尚未开始的计划功能和一次性实现缺陷不在此跟踪；它们应写入阶段设计、Builder 报告或 Reviewer 报告。

## 当前概况

- 当前检查：V0.5/S3 Approved revision1、Builder001/002、Reviewer002最终PASS与Leader003齐备；P2-01格式问题及P3-01状态同步关闭，TD-005本阶段检查点完成、风险持续Open，无新增债务。此前各阶段检查点及失败/修复依据保留在下方历史记录。
- 最近完成阶段：V0.5/S3已完成，尚未提交推送；S2已发布v0.5-s2，S4未开始，V0.5未完成。V0.1/V0.2/V0.3/V0.4已完成，历史完成记录保留。
- S1的P3-01根状态同步完成，TD-005的S1检查点完成且风险持续Open；TD-003按V0.3退出条件关闭。S2的P3-01及TD-005当前检查点也已由Leader003关闭，无新增债务或批准延期。
- V0.3/S2已有单响应积压控制、暂停读取及排空恢复；V0.3/S3只补充回归，不新增治理能力。S3已交付空闲超时，S4已交付局部输出/任务界限与关闭；全局配额仍不包含，不形成生产安全或性能承诺。
- 本文档不跟踪本地协作文档是否进入版本控制或远程发布。

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

- 检查点：`V0.5 / S4`
- 当前结论：在服务器正确性、连接模型和压测脚本稳定前，不预设 QPS 数字。
- 转换规则：基线完成后用固定环境的实测值建立优化目标，不用推测值倒逼实现。

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
