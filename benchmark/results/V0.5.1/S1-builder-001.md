# V0.5.1 / S1 诊断结果（Builder 001）

2026-09-28，WSL2/同机loopback、热页缓存；不是独立验收，不表示生产修复已完成。固定身份与环境、原始计数、逐样本资源/日志量、错误、前后审计及回收见 [原始JSON和摘要](builder-001/summary.json)。复现入口见 [诊断说明](../../diagnosis/README.md)。

## 正式矩阵

A=`e6aa82b5e2a95dc24bb76ff822599518a1e0d9fd`（V0.4/S4），B=`89514bd99a4410a6eb35d02444fe9e73f56232a2`（V0.5/S3），C=`63c184297a01004c381d07fa5845cdd675a62739`（当前固定候选）。每组合三轮，5s warmup +20s measurement、2 workers、wrk -t2 -c32。

| 场景 | A QPS中位数 | B QPS中位数 | C QPS中位数 |
| --- | ---: | ---: | ---: |
| 1KiB | 34204.239 | 712.419 | 713.416 |
| 1MiB | 432.610 | 442.061 | 442.712 |

1KiB每轮P99中位数为2.240 / 48.348 / 48.463ms；server CPU中位数为135.197% / 6.623% / 6.430%（单核100%）。A小文件跨度22.03%，标noisy；B/C小文件跨度均低于1%。不能把A精确QPS或下降比例当稳定普适容量。

[AB第二套](builder-001/run-AB-002.json)12样本及 [C第一套](builder-001/run-C-001.json)6样本全部有效，五类wrk错误零，前后长度/hash/keepalive/尾字节审计通过。计时响应不逐个审计body；C后测存在环境漂移限制。首次[AB invalid](builder-001/run-AB-001.json)完整保留：尾部把ldd的ASLR地址误作工具身份，12样本结束后整套失败。Leader明确授权修复、增加正反例、保留失败并新编号重跑，未覆盖结果或自动筛选样本。

## 定位与反证

[v0.5-s1三轮](builder-001/run-S1-001.json)的精确commit为`70b866bddbe7b4219037a93d23bde19702399d73`：715.455 / 713.619 / 715.737QPS，说明该发布快照已出现异常。

每组短跟踪≤10s，各在三个连接上顺序32请求，记录header/body接收时刻和server syscalls。默认客户端正文等待中位数：A约0.003ms，S1/B/C约42.3–42.8ms。只把客户端收到header后的TCP_QUICKACK打开，B/C三连接中位数均降至0.32–0.40ms，服务端代码/参数不变。

C代表请求：header sendto105字节在50us返回，随后sendfile1024字节在54us返回，客户端正文仍等待46.766ms；同线程epoll_wait约46.797ms。QUICKACK对照中sendfile仍立即返回，而正文等待降至亚毫秒。完整时间线见 [C默认](builder-001/run-timeline-C.json)、[C QUICKACK](builder-001/run-timeline-C-quickack.json)及相邻`.trace`文件。

A输出完整内存响应；S1/B/C改为header send后接sendfile。源码、时间线及重复单因素对照支持“头部/小正文拆开发送与ACK等待交互”的原因链；与Nagle/delayed ACK机制解释一致，但没有抓包或直接观测内核ACK/Nagle状态。证据不支持把这次40ms级停顿单独归因于日志、Buffer或CPU饱和，也不表示它们没有其他性能影响。

## 后续建议与限制

S2应针对服务端文件响应发送策略另行设计最小修复，保护部分写入、背压和关闭行为；客户端QUICKACK仅用于反证。建议后续批准相对门槛：同环境稳定配对样本中，1KiB吞吐≥当前C的10倍、每轮P99中位数≤当前C的25%，默认客户端不再出现40ms等待平台；1MiB吞吐保护线为当前C的90%。这些是待批准建议，不能当作已实现指标。

未修改生产C++、旧benchmark入口、冻结测试或用户注释。新工具最终33项synthetic测试通过。测量/trace含失败首套共856.86s，另有synthetic测试7.406s；原始日志/trace477947051字节。每角色30min/2GiB预算未提高。Builder结果须由Reviewer独立构建/测量后验收；RO-002未关闭。
