# Changelog

本文档记录 HP HTTP Server 已经完成并得到适当验证的重要变化。未来计划写入 `ROADMAP.md`，长期架构写入 `ARCHITECTURE.md`，技术债写入 `TECH-DEBT-TRACKER.md`，实现与审查证据写入对应报告。

## Unreleased

### 新增

- 新增并批准 `docs/leader/designs/V0.1/S1-design.md`，形成 Builder 可执行的 S1 基线。
- 新增并批准 `docs/reviewer/reviews/V0.1/S1-review.md`，形成 Reviewer 的 REQ/AC/RV 审查基线。
- 新增 `docs/leader/reports/V0.1/S1-report-001.md`，保留初始设计过程。
- 新增 `docs/leader/reports/V0.1/S1-report-002.md`，记录文档完整性整改、决策和验证结果。
- 新增 S1 的 CMake/C++20 工程骨架、同步日志、Socket fd RAII、基础设置操作、最小 CLI 和 3 个 CTest 测试目标。
- 新增 Builder 实现报告 001/002、Reviewer 审查报告 001 和 Leader 阶段关闭报告 003，保留实现、独立验收与关闭证据。

### 变更

- 统一仓库级文档权威顺序、阶段生命周期、返工规则、角色交接和 Reviewer 唯一结论。
- `ARCHITECTURE.md` 区分当前实现状态与长期目标，补充 metrics 依赖、事件循环线程池和信号关闭边界。
- `ROADMAP.md` 补充版本前置条件、阶段完成门槛，并明确 V1.1 是 V1.0 之后的可选扩展。
- S1 设计固定工具链基线、CLI 范围、Socket 所有权、错误策略、Builder 里程碑和验收追溯。
- S1 状态同步为 `已完成`；下一步为 Leader 设计 `V0.1 / S2`，S2 尚未开始实现。
- 技术债跟踪器改为只记录跨阶段债务、批准延期和持续风险，并移除不属于技术债范围的本地文档发布议题。

### 验证

- Builder 已完成配置、构建、CTest 和三种 CLI 验证，CTest `3/3` 通过。
- Reviewer 使用全新的 `build-review/` 独立完成 Debug/Ninja 配置与构建，RV-01 至 RV-07 全部通过，CTest `3/3` 通过，唯一结论为 `PASS`。
- Reviewer 确认 P0、P1、P2 均无；唯一 P3 顶层状态同步已由 Leader 阶段关闭处理。
- Reviewer 报告：`docs/reviewer/reports/V0.1/S1-report-001.md`。

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
