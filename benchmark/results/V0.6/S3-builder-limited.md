# V0.6/S3 Builder有限性能分析（待独立审查）

2026-10-08：按明确批准的最后R002，只选择原M2未跟踪/跟踪与M6未跟踪三条；来源四套仍3valid/1invalid/0NotRun，selection=3valid，M6 traced无效且不纳分析。没有重新取样。原CPU/ticks/clock、stdout、生命周期summary、源文件hash与独立复算见 [机器证据](S3-builder-limited.json)。

观察（WSL2同机loopback、hot-cache、closed-loop；CPU单核100%，接收MiB/s含headers）：

| 样本 | QPS | MiB/s | P99 ms | server CPU % | client CPU % |
| --- | ---: | ---: | ---: | ---: | ---: |
| M2 untraced | 39302.22 | 42.32 | 2.304 | 121.26 | 85.19 |
| M2 traced | 2031.27 | 2.19 | 31.872 | 68.08 | 7.37 |
| M6 untraced | 427.45 | 430.71 | 152.496 | 17.63 | 113.19 |

M2单次跟踪相对QPS变化为-94.83%，仅量化此配对的观测扰动，不是稳定改善/退化或容量。strace覆盖server完整生命周期（初始化、审计、预热、5smeasurement及退出），默认system time；不可将calls/time机械除measurement请求或解释成用户态/函数CPU。

M2生命周期system-time排名前三：
- epoll_wait: 3.961939s，871 calls，0 syscall errors；原报告占比33.57%。
- futex: 3.942027s，272 calls，27 syscall errors；原报告占比33.40%。
- rt_sigprocmask: 0.634165s，24717 calls，0 syscall errors；原报告占比5.37%。

线程CPU逐TID差/Hz/各读取clock独立复算，完整列表见机器文件；主TID=PID确定main reactor，其余worker/logger映射未知，不按排名猜角色。线程与进程读取窗口有偏差，不强求CPU和精确相等。五wrk errors0和HTTP正文审计/最终metrics均为所选三条的独立条件，syscall errno不等HTTP失败。

源码机制解释（固定1340f5b）：src/net/ConnectionIo.cpp通过send写普通输出，通过sendfile发送文件正文；src/net/EventLoopThreadPool.cpp建立2个worker，src/base/AsyncLogger.cpp另建consumer thread。此结构可以解释线程/系统调用类别，不证明哪个未知TID或用户态函数是瓶颈。

未证实假设与后续方向：较大正文提高接收字节率，可能受到传输、内核和WSL调度共同影响；本数据不能分解原因。后续若需容量结论，应单独批准物理机/固定连接档位重复测量；若需函数热点，应另批准可用profile环境。此处未执行这些实验。

S2有效独立基线仍见 [Builder R002](S2-builder-r002.md)与 [Reviewer](S2-reviewer-001.md)，不混不同窗口/轮次或把每轮P99中位称合并P99。M1–M3同时改变workers/连接数，不能称独立线程线性扩展。校正分布精确人口未采；RSS是采样max，S1 kernel完成与wrk client窗口不同。

缺口：M6 traced失败、原after CPU/client cleanup/final metrics缺失保持原字节，wrapper -13原因未知；本报告没有大文件syscall或长尾根因结论。原失败见 [R001事实](S3-builder-r001.md)，首次权限失败另存。V0.5.1搁置未完成，RO-002/TD-001/TD-006不关闭；S3仍待独立Reviewer与Leader收口。
