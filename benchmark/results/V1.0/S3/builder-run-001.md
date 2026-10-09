# V1.0/S3 Builder run-001：Release 回归失败后停止

2026-10-09。固定候选 `20bd03142b4c828a7e939c30b501812da2bd5440`，由独立 `git archive` 导出构建。该记录是 Builder 实际执行证据，不是独立验收或发布结论。

[机器摘要、身份与命令](builder-run-001.json) · [本批次编排源文本](builder-run-001-source.py) · [候选发布检查](../../../../documentation/RELEASE-CHECK.md)

## 实际结果

- Debug：干净构建及当前注册14项 CTest 全部通过；threads0/2 两次 HTTP smoke 通过。
- README/CLI：实际使用8080和导出 `www/`，首页200及正文一致、资源404、合法POST405；`--help`、必需port/root错误与SIGINT正常退出通过。
- Release：干净构建成功，注册14项；CTest 12项退出成功、2项失败，批次立即停止。
- 失败项：`r6_callbacks_tests`、`http_observability_tests` 均以 `std::system_error` / `epoll_ctl: Operation not permitted` 中止。
- Release 两次独立 smoke、sanitizer构建及五项专项、唯一M2准入smoke均 `NotRun`；正式压测0 valid、0 invalid、9 NotRun，没有性能数字。

Release使用批准的 `-O3 -DNDEBUG`。源码检查发现两失败测试均将 `socketpair` 初始化调用置于 `assert(...)` 中，随NDEBUG消失；之后读取未初始化的 `int pair[2]`，属于确定的测试源码问题。`send`、任务投递等也有类似副作用断言。此事实可解释失败，实际错误fd未动态采集，不能断言实际fd值或将其误判为权限失败。其余12项的退出成功也不证明所有 `assert` 语义检查仍有效。未进行额外诊断、权限重试或修改测试追绿。

## 预算、治理与保全

单批次上限1800秒，首次动态准入至失败后清理累计146.073秒，monotonic持久账本包含各命令之间等待；不是免费重试或可自动延续的剩余额度。自有资源全部回收，强杀0、未回收0。角色树轮询观测峰值107442177 bytes；真实捕获日志轮询峰值475136 bytes、最终结算479232 bytes，两者最大观测值479232 bytes，均低于4GiB/512MiB上限。冻结测试稀疏负例计角色总量，按精确test-tmp路径及生产者分类为输入夹具；不继承历史稀疏豁免。观测以≤1秒扫描为准，不能声称捕获到瞬时夹具最大值。此处最终日志值补充是结算口径勘误，原账本和执行脚本保持冻结，没有重跑。

正式运行前18项治理检查通过，包含summary缺字段/重复/非零错误/计数/顺序反例、完整路径与同名非目标、未预创建生成树、输入夹具分类、登记真实capture小额度超限、deadline拒绝及finally短sleep回收。真实Debug/Release的source内构建与test-tmp入口已实际运行；尚未进入压测链，不能把synthetic通过写成端到端性能准入通过。

原导出文件逐字节清单和原始日志留在本批独立cache中；四个生成树之外无额外输出或源码漂移。wrk/Lua/精确运行库前后SHA一致。当前10份学习源码及5份历史文档快照保护SHA全部通过；产品、tests、CMake、旧工具/raw/ignore未修改。

公开源文本只是该批次的可审查转录，唯一差异是把私有绝对仓库定位替换为当前目录；执行版与公开版hash分别在JSON中记录，公开转录未另行动态验证，不是授权重跑的永久工具。原始大日志没有发布，完整私有环境没有发布。

`server_integration_tests` 是 `http_server_integration_tests` 的别名；14项不能等同14套独立覆盖。TD-006的23个旧接口目标仍冻结，旧28/28未在此轮恢复。RO-002长尾、TD-001 WSL2代表性、TD-006覆盖缺口继续Open。本批次不足以关闭V1.0/S3或项目，需有界返工设计和独立验收。
