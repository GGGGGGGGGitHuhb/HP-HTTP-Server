# V1.0/S3 Builder R001：功能回归通过，压测前审计失败

2026-10-09。候选为base `20bd03142b4c828a7e939c30b501812da2bd5440` 加仅四份活跃测试文件的批准补丁，SHA256 `6ad80910acf90acc1c76ad3fefb4034864d521ae282f18500c30731acfd52b83`。本轮不是整体验收通过或发布。

[机器结果、原summary及身份](builder-rework-001.json) · [执行源文本转录](builder-rework-001-source.py) · [原失败记录](builder-run-001.md) · [发布检查](../../../../documentation/RELEASE-CHECK.md)

## 修正与实际验证

仅将R6Callbacks/HttpObservability/ServerMetrics三个活跃测试共94处assert替换为 `requireTestCondition`，新增 `tests/TestCheck.h`。表达式、顺序、故障注入、捕获、时限及注释逐项保持；没有改变产品、CMake、NDEBUG或共享头。四文件已按clang-format18.1.3格式化并检查，独立静态审查确认完整581文件候选身份一致。

- checker实际使用 `-O3 -DNDEBUG`：成功分支副作用求值一次，malloc/operator new包装观察0分配、stdout/stderr均0；失败分支SIGABRT，stderr含准确调用文件及行号。
- 新候选独立Debug/Release干净构建，各当前14项CTest全部通过；四次threads0/2 HTTP smoke全部通过。
- README实际8080/www首页200、404/POST405、CLI必需port/root与SIGINT正常退出通过。
- ASan/UBSan五专项5/5通过，`detect_leaks=1` 保持LSan开启。
- 唯一M2准入smoke有效；正式前三样本r1-M2/M3/M6有效。
- 第4正式样本r2-M6在 **pre_audit读取响应头时收到EOF**，尚未启动warmup/measurement，最终invalid；后续五样本NotRun。

最终正式账为 **3 valid / 1 invalid / 5 NotRun**。没有补样本、混旧候选结果或计算三轮聚合；本轮不足以满足AC03，不能关闭S3/V1.0。前三样本原summary、前后审计、退出metrics和正常回收见JSON，不把不完整批次当性能改善证据。

## 失败事实与缺失证据

r2-M6 server监听归属在启动时已确认；失败后的SIGTERM正常退出0、reaped true、forced false。已有退出metrics记录：2个请求开始、2个响应完成且均200、0请求中止/错误、最终connections_active/logger_pending为0；stderr有输出达到EAGAIN的证据及正常shutdown signal15。

`audit()`仅在三次请求全部成功时返回并持久化 observations，本失败分支没有保存每次recv字节数、已完成观察和准确请求序号。响应头读取实际为 `recv(65536)` 返回空，错误是 `header EOF`；现有metrics不能独自证明下一次EOF原因或精确客户端进度。没有新增动态采集、诊断、延长timeout、重跑或产品修复；根因保持未知。

运行期间原编排把未结算样本默认写为invalid，`not_run`也会暂保留上个阶段账。此前这一占位被误读后已纠正，**它不是实际失败**；最终state failed、r2-M6 failure header EOF和cleanup结算才是本次停止依据。旧脚本、候选补丁及失败记录保持冻结。

## 费用与保护

R001获批的新一次批次实际475.213277214秒，旧run-001实际146.072835749秒独立保留，Builder累计621.286112963秒。预算含checker、工具准入、构建/测试/压测、全部等待和失败清理；旧未使用额度没有续跑。总量最大观测211230720 bytes，真实capture最大/最终均696320 bytes；末次结算纳入最大值，4GiB/512MiB上限未触及，自有资源0、强杀0、未回收0。

18项治理反例重新通过；真实source内build/test-tmp运行成功。原文件SHA、四文件补丁与候选清单保全，固定wrk/Lua/运行库前后SHA一致。10学习源码、5文档历史快照、31冻结测试保持；14当前注册含HTTP黑盒别名，23个旧目标依旧冻结，不能用本轮14/14替代历史28/28。

公开编排源文本仅机械替换私有REPO绝对定位为cwd，执行/转录hash和差异见JSON，未另行动态验证转录版，不作为新永久压测框架或重跑授权。原大日志和候选补丁全文保全于独立cache，没有发布完整私有环境。RO-002/TD-001/TD-006保持Open，独立Reviewer动态尚未执行。
