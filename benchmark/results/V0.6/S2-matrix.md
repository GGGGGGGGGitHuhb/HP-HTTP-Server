# V0.6/S2 场景矩阵 — 本轮未完成

日期：2026-10-08；Builder首次正式套已停止，**未完成/无有效完整矩阵**。18计划中 **2个有效测量、1个无效尝试、15个未执行**；独立Reviewer套未执行。不能据此宣布S2通过、性能恢复或长尾原因；V0.5.1仍搁置未完成，RO-002/TD-001/TD-006保留。

固定产品commit `1340f5bb8a303769a8be32b2d8cc29482fd2a8fe`，tree `f2fdabf35fb754b7f4e981708efd34e40e53ceac`；git archive独立Release/C++20/GCC13/-O3 -DNDEBUG/-j2、BUILD_TESTINGOFF，无LTO/native/sanitizer。固定wrk debian4.1.0-4build2，ELF SHA `b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d`。产品ELF/compiler/运行库/工具SHA与原始scalar及完整小文本stdout见 [Builder样本数据](S2-builder-samples.json)。命令和参数口径见 [matrix入口](../../matrix/README.md)。

WSL2/x86_64，同机loopback、closed-loop、热缓存；不是物理机容量。每正式测量前2s预热、测量10s，pre/post各3次完整200/header/正文SHA/无尾字节审计。计划固定6组×3轮与次序，本轮只有r1 M1/M2完成；无三轮分组median/min/max，不混样或补跑。

| 本轮已完成样本 | 正文/模式 | workers | wrk T/C | QPS | 接收MiB/s | 校正P99(ms) | server CPU单核% | server RSS采样max(KiB) |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| r1 M1 | 1KiB/keepalive | 0 | 1/1 | 2238.81 | 2.41 | 0.791 | 12.16 | 4800 |
| r1 M2 | 1KiB/keepalive | 2 | 2/32 | 39633.96 | 42.67 | 2.018 | 143.60 | 5280 |

这两个是单次部分结果，不构成可接受的完整基线。QPS按client completed/实际duration_us计算；MiB/s包含HTTP headers。client CPU、每秒RSS采样数、mean/P50/P95/P99/max、五类错误（两样本均0）、pre/post审计、server独立最终账和完整wall见机器数据。server completed指kernel排空且含审计/预热/退出边界，不能与wrk客户端测量窗口requests机械等同；最终active/logger_pending均0且总账平衡。CPU单核100%口径；RSS是采样最大值，VmHWM可含warmup，不是精确峰值。

r1 M3（1KiB keepalive、4workers、wrk4/128）预热完成，正式measurement未在批准的10+5s process deadline内退出。measurement stdout/stderr均0字节，无MATRIX_SUMMARY，不能以零填充错误/人口/吞吐。控制器按首失败规则SIGTERM回收wrk，exit=-15、非forced且reaped；server正常exit0/reaped。formal实际44.27s，整体窗口至停止183.16s，仍有总剩1016.82s，但smoke/正式套次数均已消费，**没有自动重跑或追加额度**。本任务计费空间约32.10MiB、raw约38.91KiB，远低于4GiB/512MiB检测式上限，无资源overshoot。

工具未闭发现：Lua为了精确校正人口，对每个非零bin调用latency(i)；只读wrk4.1.0源码确认该接口每次从min重新扫描，密集分布会重复扫描，违背轻量scalar意图。M3预热已有23948个非零bin。这是静态工具超时风险，未取得超时现场执行栈，不能认定本次原因，更不能认定服务器长尾根因。原控制器not_run字段还包含失败M3（16个尾项），本报告明确去除该重叠，实际人口是2 valid+1 invalid+15 NotRun；原私有失败证据不覆盖。

wrk分位数来自stats_correct后的校正分布，不是逐请求raw/网络RTT。机器数据保留已完成样本corrected_population（sum(count)）、nonzero_bins与correction_interval；#latency不是人口。没有完整histogram、独立合并P99或coordinated-omission-free承诺。后续工具/统计方案必须由Leader决策并重新授权，不把剩余时间当第二套额度。

后续：已明确批准R001额外有限恢复；新schema2完整Builder套另见 [R001结果](S2-builder-r001.md)，本文件及旧机器raw保留首次失败事实，不合并样本。

R002最后一次已批准有限重采另见 [CPU原始证据齐备的新套](S2-builder-r002.md)；本页历史CPU只保留原派生计算值，缺CPU原始输入、不可独立核验，不将它混入新套。
