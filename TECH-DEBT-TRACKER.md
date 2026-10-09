# HP HTTP Server 技术债跟踪

## 当前概况

V1.0主线阶段验收已完成，S3 Reviewer006独立PASS，待用户合并发布；不新增放行债务或关闭原条目。当前14项CTest含HTTP黑盒别名，23旧目标/31源码冻结限制保持；条件化压测不证明高并发长尾修复或物理机容量，历史EOF根因未知，见[发布检查](documentation/RELEASE-CHECK.md)。V0.6按有限范围已完成；V0.5.1搁置未完成，长尾仍未解决，旧诊断入口仅归档，不能直接恢复。TD-001/004/005/006及RO-002/003保持Open，TD-002/003保持Closed。完整原检查点见 [归档](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md) 与 [停工记录](benchmark/results/V0.5.1/SHELVED.md)。

V0.6/S3独立有限三条PASS只说明Approved R002范围：M2 syscall全生命周期粗粒度证据及M6未跟踪CPU；M6 traced缺证仍invalid，旧raw不补造。strace巨大扰动不能解释为稳定瓶颈或长尾根因；S2矩阵也不证明恢复、物理机容量或多核线性扩展。无新债务豁免。

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
- R6 当前清单：23 个旧接口耦合 CTest 目标暂退，5 个兼容旧 CTest 保留，新增 1 个专项目标；精确名单与 include 依赖原因见 [测试覆盖说明](documentation/DEVELOPMENT.md#冻结旧测试与覆盖限制)。31 个旧测试路径及内容保持冻结，独立 Reviewer R6-report-001 为 PASS WITH DEBT：6/6 CTest、专项 sanitizer 1/1、HTTP smoke 和47文件格式检查通过，31个旧文件逐一校验一致。Leader R6-report-002 接受此已批准延期并关闭 R6，TD-006 保持 Open。

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

### RO-001 初始目录和 CMake 结构验证

原检查点已完成，当前仍保留include/src布局；不据此执行目录迁移。原记录见归档。

### RO-002 性能目标尚未量化

- 状态：Open。历史局部TCP_NODELAY修复与高并发长尾失败同时有效；V0.5.1管理停工不关闭本风险。
- 当前决定：V0.6提供条件化观测/矩阵及有限分析，不替代旧高并发验收。没有新恢复门槛或根因结论，不继续旧诊断。
- 责任/检查点：Leader在后续获批性能阶段定义完整对象、环境、数值目标与停止规则，Builder执行，Reviewer独立验收；不得由当前S1文档整理扩展测量。
- 退出标准：批准明确目标后有独立性能证据满足退出要求，或另经用户明确风险处置；目前均未满足。原各次数字、失败及转换规则保存在 [RO-002历史](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#ro-002-性能目标尚未量化)。

### RO-003 L7 Gateway 与独立 L4LB 边界

- 检查点：进入 `V1.1` 前
- 当前结论：本仓库只考虑 HTTP 层转发；独立 L4LB、XDP、DPDK 不进入本项目。
- 转换规则：若用户目标转向 L4，应另立项目或路线，不在本仓库混合实现。

## 关闭与更新规则

- 技术债只有在退出标准满足且存在 Builder/Reviewer 证据后才能关闭。
- `PASS WITH DEBT` 中的条目必须给出负责人、目标阶段和退出标准。
- 阶段未实现的计划功能不创建技术债。
- Reviewer 发现的当前阶段必需缺陷优先返工，不得直接登记技术债规避验收。
- 条目状态变化时追加更新记录，不覆盖形成决策时的原因。


<details>
<summary>历史锚点导航</summary>

旧标题对应迁移前历史时点，不作为当前能力或执行授权。

<a id="当前停工状态"></a>
- [当前停工状态](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#当前停工状态)
<a id="v06s1收口检查点2026-10-08"></a>
- [V0.6/S1收口检查点（2026-10-08）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v06s1收口检查点2026-10-08)
<a id="v06s2收口检查点2026-10-08"></a>
- [V0.6/S2收口检查点（2026-10-08）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v06s2收口检查点2026-10-08)
<a id="v06s3收口检查点2026-10-08"></a>
- [V0.6/S3收口检查点（2026-10-08）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v06s3收口检查点2026-10-08)
<a id="阶段边界"></a>
- [阶段边界](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#阶段边界)
<a id="v03s1-关闭检查点"></a>
- [V0.3/S1 关闭检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v03s1-关闭检查点)
<a id="v03s2-关闭检查点"></a>
- [V0.3/S2 关闭检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v03s2-关闭检查点)
<a id="v03s3-检查点已完成"></a>
- [V0.3/S3 检查点（已完成）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v03s3-检查点已完成)
<a id="变更记录"></a>
- [变更记录](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#变更记录)
<a id="v04s3-准备检查点设计中"></a>
- [V0.4/S3 准备检查点（设计中）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v04s3-准备检查点设计中)
<a id="v04s3-关闭检查点"></a>
- [V0.4/S3 关闭检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v04s3-关闭检查点)
<a id="v04s4-准备检查点设计中"></a>
- [V0.4/S4 准备检查点（设计中）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v04s4-准备检查点设计中)
<a id="v04s4-批准检查点"></a>
- [V0.4/S4 批准检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v04s4-批准检查点)
<a id="v04s4与版本关闭检查点"></a>
- [V0.4/S4与版本关闭检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v04s4与版本关闭检查点)
<a id="v05s1-准备检查点设计中"></a>
- [V0.5/S1 准备检查点（设计中）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s1-准备检查点设计中)
<a id="v05s1-批准检查点"></a>
- [V0.5/S1 批准检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s1-批准检查点)
<a id="v05s1-关闭检查点"></a>
- [V0.5/S1 关闭检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s1-关闭检查点)
<a id="v05s2-准备检查点设计中"></a>
- [V0.5/S2 准备检查点（设计中）](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s2-准备检查点设计中)
<a id="v05s2-批准登记"></a>
- [V0.5/S2 批准登记](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s2-批准登记)
<a id="v05s2-关闭检查点"></a>
- [V0.5/S2 关闭检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s2-关闭检查点)
<a id="v05s3-准备检查点"></a>
- [V0.5/S3 准备检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s3-准备检查点)
<a id="v05s3-批准登记"></a>
- [V0.5/S3 批准登记](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s3-批准登记)
<a id="v05s3-关闭检查点"></a>
- [V0.5/S3 关闭检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s3-关闭检查点)
<a id="v05s4-准备检查点"></a>
- [V0.5/S4 准备检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s4-准备检查点)
<a id="v05s4-批准登记"></a>
- [V0.5/S4 批准登记](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s4-批准登记)
<a id="v05s4-r001批准登记"></a>
- [V0.5/S4 R001批准登记](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s4-r001批准登记)
<a id="v05s4-收口检查点"></a>
- [V0.5/S4 收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#v05s4-收口检查点)
<a id="r1重构收口检查点"></a>
- [R1重构收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#r1重构收口检查点)
<a id="r2重构收口检查点"></a>
- [R2重构收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#r2重构收口检查点)
<a id="r2最终回调注册收口检查点"></a>
- [R2最终回调注册收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#r2最终回调注册收口检查点)
<a id="r3-connectionio收口检查点"></a>
- [R3 ConnectionIo收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#r3-connectionio收口检查点)
<a id="r4-cli与信号辅助收口检查点"></a>
- [R4 CLI与信号辅助收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#r4-cli与信号辅助收口检查点)
<a id="r5与整体渐进重构收口检查点"></a>
- [R5与整体渐进重构收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#r5与整体渐进重构收口检查点)
<a id="全测试清理收口检查点"></a>
- [全测试清理收口检查点](history/documentation/V1.0-S1/TECH-DEBT-TRACKER.md#全测试清理收口检查点)

</details>
