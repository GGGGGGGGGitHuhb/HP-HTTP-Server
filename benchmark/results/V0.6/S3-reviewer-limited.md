# V0.6/S3 Reviewer有限独立分析

2026-10-08，独立唯一三条均有效；[原始CPU/thread/stdout/生命周期summary与复算](S3-reviewer-limited.json)。固定产品1340f5bb8a303769a8be32b2d8cc29482fd2a8fe、tree f2fdabf35fb754b7f4e981708efd34e40e53ceac，自身S2 Release artifact只读复用。各1s预热+5smeasurement，workers2/wrk2/32、keepalive。

| 观察 | QPS | 接收MiB/s | server单核CPU% | client单核CPU% |
| --- | ---: | ---: | ---: | ---: |
| M2 untraced | 41380.95 | 44.55 | 134.78 | 99.56 |
| M2 traced | 1718.38 | 1.85 | 66.47 | 7.57 |
| M6 untraced | 410.56 | 413.90 | 16.91 | 105.13 |

M2单次跟踪相对QPS变化-95.85%，只是观测扰动，不证明稳定改善/退化或瓶颈。M2 syscall完整server生命周期含启动/审计/预热/measurement/退出，strace6.8默认system time，不是wall、用户态或函数CPU；calls/time不除5s请求数。system-time前三：
- futex：3.817667s，33.21%，240 calls，23 syscall errors。
- epoll_wait：3.817243s，33.21%，720 calls，0 syscall errors。
- rt_sigprocmask：0.645049s，5.61%，21213 calls，0 syscall errors。

五wrk errors均0、pre/post各3正文/status/连接审计正确，退出S1总账平衡且active/logger_pending0；syscall errno不等HTTP失败。thread从各TID ticks/Hz/read clock独立复算，main TID=PID已知，其余worker/logger映射unknown；线程和进程读取区间稍异，不强求精确相等。

源码解释：固定ConnectionIo.cpp普通输出用send、文件正文用sendfile；EventLoopThreadPool.cpp建立workers，AsyncLogger.cpp另有consumer。这些结构说明类别，不能把未知TID绑定为特定worker或断言函数热点。假设与后续方向：传输/内核/WSL调度可能共同影响，原因未分解；容量重复测量或函数profile需要另行批准，本轮未执行。

仅M2有syscall配对；M6只有untraced CPU证据。Builder原四套仍3valid/1invalid，M6 traced after CPU/client cleanup/final metrics缺失、wrapper SIGPIPE原因未知，旧记录未补造。本Reviewer三条与Builder或S2数据不混池；S2已独立有效基线见[BuilderR002](S2-builder-r002.md)与[Reviewer](S2-reviewer-001.md)。WSL2同机loopback/closed-loop/hot-cache，MiB/s含headers、校正人口未采null、三轮P99中位非合并P99、RSS为采样max，S1 kernel完成与client窗口不同。没有物理机容量/多核线性扩展/函数CPU/长尾根因结论；RO-002/TD-001/TD-006及V0.5.1搁置未完成保持。阶段收口由Leader负责。
