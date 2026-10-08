# V0.6/S2 独立场景矩阵入口

该入口只测固定已验收S1 commit `1340f5bb8a303769a8be32b2d8cc29482fd2a8fe`，不读取dirty学习源码、不修改历史A/B工具。2026-10-08经Reviewer002独立PASS，S2已完成；[Reviewer独立18套](../results/V0.6/S2-reviewer-001.md)与[Builder R002套](../results/V0.6/S2-builder-r002.md)分别保留。每角色只允许一次smoke和一次18样本正式套；失败保留并停止，不换目录补跑。没有性能达标阈值，不能用数值关闭原长尾风险。

## 输入、路径与实际命令

Linux/WSL、既有Python3标准库、git、cmake、g++及已冻结wrk。无需安装/下载。固定wrk只读来自仓库 `.cache/v0.5-s4/tools/root/usr/bin/wrk`，SHA `b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d`，库根同tools/root/usr/lib/x86_64-linux-gnu；缺失/身份不同停止，不能换版本。

在仓库根使用Bash，角色根只能是 `.cache/v0.6-s2/builder` 或独立 `reviewer`。准备者只建tmp/cache；Build拥有首次artifact/source/build创建，Run拥有尚不存在的suite/payload/sample创建，校验者不提前建未来子目录。旧存在输出拒绝，不覆盖。

```bash
mkdir -p .cache/v0.6-s2/builder/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.6-s2/builder/tmp" TMP="$TMPDIR" TEMP="$TMPDIR"
export XDG_CACHE_HOME="$PWD/.cache/v0.6-s2/builder/cache" PYTHONDONTWRITEBYTECODE=1
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
python3 benchmark/matrix/Build.py --root .cache/v0.6-s2/builder
python3 tests/MatrixBenchmark_test.py --role-root .cache/v0.6-s2/builder
python3 benchmark/matrix/Run.py --root .cache/v0.6-s2/builder --mode smoke
python3 benchmark/matrix/Run.py --root .cache/v0.6-s2/builder --mode formal
```

实际socket/HTTP/进程清理测试按AGENTS首次窄提升。上述命令需要该轮预算授权；已有budget/phase/output意味着已消费，不能再次运行当重试。Reviewer将root和环境换成自己的reviewer树，独立导出、构建、夹具和统计。

首个Build创建固定monotonic deadline：总1200s（独立cleanup预留10s），build600s/fastchecks120s/smoke60s/formal600s均共用总额剩余。编码、文档必须先备齐；deadline一旦开始，之后等待也减少剩余，不续额。task4GiB、raw512MiB按逻辑/分配最大值及stdout/stderr/log分类统一扫描；≤1s检测轮询，可能overshoot而非硬quota，真实用量保存budget.json。先检查磁盘free能容纳4GiB；每进程nofile依据128连接+32控制/thread reserve至少160，记录宿主实际值而不改系统limit。

## 输出与口径

Build导出fixed git archive，Release/C++20/-O3 -DNDEBUG/-j2、BUILD_TESTINGOFF，不启LTO/native/sanitizer；manifest记录archive/完整source map、compile_commands/CMakeCache/binary、compiler/wrk/运行库hash，运行前后校验。

每个新sample自启loopback/port0 server，核PID/starttime与socket inode归属；access-log关闭、metrics-on-exit开启。pre/post各审计3次：keepalive同socket串行3完整200/header/长度/SHA且无尾字节；short每次Connectionclose、正确正文后EOF、重连。热缓存不主动清系统cache。

smoke固定M2（1KiB/2worker/wrk2线程32连接），1+1s，不进正式统计。正式每样本2s warmup+10s measurement，固定18顺序：r1 M1..M6；r2 M6..M1；r3 M3,M4,M5,M6,M1,M2。M1/2/3分别1KiB keepalive、0/2/4worker与wrk1/1、2/32、4/128；M4为M2短连接；M5/M6为M2的64KiB/1MiB正文。

机器schema2 suite result含identity、sequence、samples/groups、valid/invalid/not_run互斥总账（已开始失败只属invalid，NotRun为未开始后缀）。每sample含完整private argv、pre/post audit、warmup/measurement原始scalar、每秒RSS采样和CPU、server最终metrics、PID回收与complete_wall_s。原stdout/stderr、process/cleanup JSON保留角色cache，不发布原始大日志或完整环境。

QPS=client completed/duration_us；MiB/s=接收bytes/秒/1048576，包含headers。Lua done仅输出scalar，不注册计时response回调；短连接仅设置Connectionclose。延迟是wrk4.1.0 stats_correct之后校正分布：精确人口未采，population_status=not_collected、corrected_population与nonzero_bins为显式null；done只调用固定数量的标准统计接口（不承诺内部O(1)），correction_interval_us=duration_us/(requests/connections)。它不是逐请求raw或RTT，不能合并三轮分位数称全体P99；组统计是每轮指标median/min/max和(max-min)/median。

CPU server以/proc PID身份一致的CPU秒增量除包围monotonic wall，client以单独wait4 child rusage除自己的wall，单核100%口径；不使用累计children。RSS是每秒采样最大值与sample count，VmHWM可能包含warmup，不是精确峰值。server S1统计包含审计/预热/测量/退出边界且completed是kernel排空，不能与wrk测量窗口客户端completed强行相等；只核最终marker/active0/总账。五类测量错误必须全零，失败数值保留为invalid。

WSL2同机loopback、closed-loop、hot-cache与调度噪声限制始终适用；组合worker/连接档位不证明独立多核线性扩展或物理机容量。无恢复、根因、coordinated-omission-free承诺；RO-002/TD-001/TD-006不关闭。

新 `matrix_benchmark_tests` CTest只测功能/合成反例，源码tests/MatrixBenchmark_test.py；正式fastchecks应使用上述--role-root以包括真实manifest与HTTP。CTest无role-root时是更小合成边界子集，不冒充正式smoke/18套。已消费角色不能借CTest再次取得smoke/formal额度。

## 已批准 R001 Builder 单次恢复

旧失败套与原budget/settlement保持只读，旧窗口已过期。Builder仅使用既有成功artifact，不重新构建；静态准备完成后以下首命令创建 `rework-001/budget.json`，绑定原ledger SHA、PM批准、role/host，开始额外600s绝对窗口。fastchecks≤60s、M3 smoke 1+1s≤60s、唯一全新18 formal共用剩余并留10s cleanup。空间按旧+新Builder累计计算，不暂停、不自动重试；Reviewer仍用上面的原独立额度。

```bash
python3 tests/MatrixBenchmark_test.py --role-root .cache/v0.6-s2/builder --recovery
python3 benchmark/matrix/Run.py --root .cache/v0.6-s2/builder --mode smoke --recovery
python3 benchmark/matrix/Run.py --root .cache/v0.6-s2/builder --mode formal --recovery
```

历史schema1 suite.not_run16含失败M3；实际18为2valid/1invalid/15NotRun。新schema2结果单独保存，不合并旧两条样本。

## R002 CPU原始证据与最后一次Builder窗口

measurement的`cpu_evidence` schema1明确ticks/seconds/单核100%单位；server before/after含真实utime_ticks/stime_ticks、sysconf频率、PID/starttime、cpu_seconds和读取monotonic。client以启动PID/starttime绑定实际wait4返回PID、ru_utime_s/ru_stime_s及读取clock，cleanup同值保存。包围started/ended/wall分母保持原式（包含创建/等待/close边界，不使用wrk duration）；缺失、身份漂移、频率/clock/派生值不一致均拒绝valid。

R001旧18仅派生CPU百分比，原始输入缺失，CPU不可独立核验；旧机器文件不补造。R002另存新完整套，不混旧样本。Approved最后一次600s窗口引用原+R001账SHA、same role/host，累计原空间限制；不重build，第一条命令才开始绝对窗口，后续等待也计时：

```bash
python3 tests/MatrixBenchmark_test.py --role-root .cache/v0.6-s2/builder --recovery-name rework-002
python3 benchmark/matrix/Run.py --root .cache/v0.6-s2/builder --mode smoke --recovery-name rework-002
python3 benchmark/matrix/Run.py --root .cache/v0.6-s2/builder --mode formal --recovery-name rework-002
python3 benchmark/matrix/Recompute.py --suite .cache/v0.6-s2/builder/rework-002/suite-formal/result.json --output .cache/v0.6-s2/builder/rework-002/independent-recompute.json
```

Recompute不import Run/Common公式，从ticks差/频率及真实wait4输入独立复算CPU，再核6组三轮汇总。fastchecks内第三项synthetic失败走实际Run dispatch→suite→sample→真实server/HTTP audit→finally/回收，mock负载明确标注，没有正式槽或wrk负载；预期2valid/1invalid/15NotRun。fast≤60s、M3 1+1s smoke≤60s、唯一18formal与cleanup10s共用600s；任何真实验证失败即停，最后第2/2轮不得自动重试。
