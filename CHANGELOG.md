# Changelog

本文档记录 HP HTTP Server 已经完成并得到适当验证的重要变化。未来计划写入 `ROADMAP.md`，长期架构写入 `ARCHITECTURE.md`，技术债写入 `TECH-DEBT-TRACKER.md`，实现与审查证据写入对应报告。

## Unreleased

### 新增

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
