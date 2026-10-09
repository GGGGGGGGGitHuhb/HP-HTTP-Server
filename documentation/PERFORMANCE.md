# 性能摘要与证据边界

[项目入口](../README.md) · [项目展示](PRESENTATION.md) · [面试讲解](INTERVIEW.md) · [实验索引](../benchmark/README.md)

这里整理已有独立测量，不是V1.0/S2新压测。当前已提交实现的结构见[架构](../ARCHITECTURE.md)；下列测量分别绑定各自历史源码身份，不能直接当成当前分支重新验收。QPS表示客户端完成的HTTP请求/响应事务数每秒。

## V0.6/S2：六档独立基线

依据[Reviewer报告](../benchmark/results/V0.6/S2-reviewer-001.md)、[机器样本与汇总](../benchmark/results/V0.6/S2-reviewer-001-samples.json)及[固定矩阵](../benchmark/matrix/README.md)。固定产品commit `1340f5bb8a303769a8be32b2d8cc29482fd2a8fe`、tree `f2fdabf35fb754b7f4e981708efd34e40e53ceac`；Release C++20、`-O3 -DNDEBUG`，无LTO/native/sanitizer。wrk SHA256为 `b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d`。

环境为WSL2同机loopback、closed-loop与hot-cache；每档三轮，每样本2s预热、10s测量，共18 valid、0 invalid、0 NotRun。以下所有列取各轮指标的中位数，QPS和P99另列三轮[min,max]；不是合并请求的P99。

| 档位 | 正文 | 连接方式 | server workers | wrk线程/连接数 |
| --- | --- | --- | ---: | --- |
| M1 | 1 KiB | Keep-Alive | 0（单Reactor） | 1/1 |
| M2 | 1 KiB | Keep-Alive | 2 | 2/32 |
| M3 | 1 KiB | Keep-Alive | 4 | 4/128 |
| M4 | 1 KiB | 每响应EOF后重连 | 2 | 2/32 |
| M5 | 64 KiB | Keep-Alive | 2 | 2/32 |
| M6 | 1 MiB | Keep-Alive | 2 | 2/32 |

| 档位 | QPS median [min,max] | 接收MiB/s | 校正P99 ms median [min,max] | server CPU单核% | client CPU单核% | server RSS采样max KiB的中位 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| M1 | 2120.60 [1906.21,2308.10] | 2.28 | 0.817 [0.785,0.887] | 9.35 | 5.65 | 4800 |
| M2 | 44884.42 [39283.03,45849.80] | 48.33 | 1.648 [1.491,2.013] | 135.89 | 99.94 | 5280 |
| M3 | 52514.45 [50786.18,52615.08] | 56.54 | 811.771 [390.712,1188.674] | 270.89 | 203.82 | 7200 |
| M4 | 9476.90 [9416.37,9826.00] | 10.16 | 5.090 [5.061,5.346] | 102.80 | 119.63 | 4640 |
| M5 | 5350.92 [5062.27,5399.34] | 335.01 | 13.014 [11.838,13.913] | 59.85 | 102.31 | 5440 |
| M6 | 414.06 [382.39,417.13] | 415.80 | 178.022 [167.016,194.914] | 17.71 | 99.18 | 5280 |

M2可作为带条件的展示基线。M3吞吐更高，同时三轮P99中位达到811.771ms，且轮间范围很大，不能只摘52514.45 QPS写成全面提升。M1/M2/M3同时改变workers、客户端线程和连接数，无法据此证明多核线性扩展。M4及M5/M6分别展示短连接成本和文件大小变化下的表现，未设性能达标门槛，也未分离系统调度、客户端与传输路径的因果贡献。

## TCP_NODELAY：历史局部修复案例

依据[V0.5.1/S2独立报告](../benchmark/results/V0.5.1/S2/S2-reviewer-001.md)、[原始summary](../benchmark/results/V0.5.1/S2/reviewer-001/summary.json)与[配对方法](../benchmark/repair/README.md)。C为 `942f72cd9cea58e097025c3b9dd660f4132a1ffb`；D为当时未提交候选 `uncommitted-workspace-snapshot`，commit/tree为null，archive SHA256为 `3492c5926d8e62b75832c3a808c0aba95265dbf5bfcf0b703c968605e7a3478f`，含当时实际学习注释字节。后续发布提交不能替代D的实测身份。

C/D各自独立Release构建，`-O3 -DNDEBUG`，无LTO/native/sanitizer；WSL2同机loopback、热缓存、默认客户端ACK，2workers、wrk2线程/32连接，每样本5s预热+20s测量。三轮C/D分别测1KiB和1MiB，共12有效样本，奇数轮C→D、偶数轮D→C，文件顺序交替。计时body未逐个审计；机制验证另做逐body校验，不与正式样本混合。

| 指标（各轮中位） | C | D |
| --- | ---: | ---: |
| 1 KiB QPS | 716.522 | 37200.425 |
| 1 KiB 校正P99 ms | 48.399 | 2.024 |
| 1 MiB QPS | 432.535 | 434.197 |
| 1 MiB 校正P99 ms | 187.780 | 199.955 |
| 1 KiB server CPU单核% | 6.823 | 188.920 |
| 1 KiB client CPU单核% | 3.391 | 94.817 |

问题来自小响应头/正文发送路径中的Nagle与默认ACK等待交互。生产修复在[Acceptor](../src/net/Acceptor.cpp)交付新Socket前启用TCP_NODELAY，设置失败只关闭该连接；文件正文仍走[sendfile路径](../src/net/ConnectionIo.cpp)。原独立默认ACK机制证据中三连接正文等待中位：C约42.15–42.89ms，D约0.271–0.283ms；候选trace确认TCP_NODELAY且未使用QUICKACK或系统TCP调参。

收益伴随CPU投入：小文件server CPU由6.823%升至188.920%，不能只报吞吐收益。1MiB保护档的P99也由187.780ms升至199.955ms；它通过当时限定修复门槛，不表示所有指标都改善。V0.5.1/S2的局部PASS不能替代后来未通过的S3高并发长尾验收；V0.5.1仍[搁置未完成](../benchmark/results/V0.5.1/SHELVED.md)。本案例与上方V0.6矩阵不跨版本配对或混池。

## V0.6/S3：有限观测

依据[有限独立报告](../benchmark/results/V0.6/S3-reviewer-limited.md)及[机器证据](../benchmark/results/V0.6/S3-reviewer-limited.json)。固定产品仍为1340f5b及上文tree，自身S2 Release artifact只读复用；每条1s预热+5s测量，2workers、wrk2线程/32连接、Keep-Alive。

| 单次观察 | QPS | 接收MiB/s | server CPU单核% | client CPU单核% |
| --- | ---: | ---: | ---: | ---: |
| M2 untraced | 41380.95 | 44.55 | 134.78 | 99.56 |
| M2 traced | 1718.38 | 1.85 | 66.47 | 7.57 |
| M6 untraced | 410.56 | 413.90 | 16.91 | 105.13 |

M2跟踪相对QPS变化-95.85%，表示巨大观测扰动，不能推断稳定瓶颈或改善。只有M2有syscall配对，M6 syscall未知。strace默认system time覆盖server启动、审计、预热、测量和退出，不是wall或函数CPU；futex、epoll_wait的排序不证明业务函数热点或锁竞争根因，不能用完整生命周期calls除5s请求数。除main TID=PID外，worker/logger TID映射未知；syscall errno也不等于HTTP错误。

## 指标怎么读

- wrk延迟是stats_correct之后的校正分布；精确人口未采，`population_status=not_collected`、`corrected_population/nonzero_bins=null`，null不是0。三轮分位数中位不能称为全体请求P99，也不承诺消除coordinated omission。
- 接收MiB/s包含headers；QPS、MiB/s、客户端校正延迟与服务端请求计时回答不同问题。
- CPU以单核100%为口径，可超过100%。V0.6 server从PID/starttime、/proc ticks/Hz及读取clock复算；client从单独wait4 child及其wall复算，不使用累计children。
- RSS为每秒采样最大值，不是瞬时精确峰值；VmHWM可能包含预热。表内再取三轮采样最大值的中位。
- server completed表示输出被kernel接收，且server统计覆盖审计/预热/退出；wrk requests只属客户端测量窗口，不要求两者机械相等，不能据server completed证明peer收全正文。

五类测量错误零及前后状态/正文审计证明各套在声明的验证范围有效；不能扩展成公网/物理机容量或当前全量测试通过。RO-002、TD-001、TD-006仍[开放](../TECH-DEBT-TRACKER.md)，高并发长尾未关闭。V1.0整体未完成，最终回归属于尚未开始的S3。
