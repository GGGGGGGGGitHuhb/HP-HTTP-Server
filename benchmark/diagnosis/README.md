# V0.5.1 / S1 性能诊断

这是独立的新入口，复用只读 `../build.py`、`../run.py` 的固定身份、HTTP 审计、采样和进程回收原语；历史入口及结果未改。正式 A/B/C 与中间版本、trace、单因素实验分开记录。仅运行本次自启动 loopback 服务，不接受外部 URL，不安装依赖。

## 准备与正式测量

在仓库根目录执行；Reviewer 把所有 `builder` 替换为 `reviewer`，独立导出/构建。需要历史已校验的 wrk 缓存及其动态库。构建/测试不得和正式测量并行。真实 socket/strace 按仓库规则申请窄提升。

```bash
mkdir -p .cache/v0.5.1-s1/builder/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.5.1-s1/builder/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR" XDG_CACHE_HOME="$PWD/.cache/v0.5.1-s1/builder/cache"
export PYTHONDONTWRITEBYTECODE=1 HP_S3_TEST_TMP_ROOT="$TMPDIR"
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
python3 benchmark/diagnosis/test_diagnose.py
python3 benchmark/diagnosis/diagnose.py --role builder build
python3 benchmark/diagnosis/diagnose.py --role builder run --suite AB --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s1/builder/run-AB-001
python3 benchmark/diagnosis/diagnose.py --role builder run --suite C --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s1/builder/run-C-001
```

每套新目录，禁止覆盖证据。AB 12 样本保留历史运行顺序，C 单独 6 样本；各样本 5s warmup +20s measurement、1KiB/1MiB、2 workers、wrk `-t2 -c32 --timeout 2s --latency`。记录 CPU、RSS、五类错误、审计与回收；整个套有效才汇总。每组三轮跨度 >20% 标 noisy。计时响应未逐个核对 body，前后审计不能替代这一限制。WSL2/同机 loopback/热缓存结果不能推广为物理机容量。

每角色动态测量/机制实验合计 30min，单套 600s，每阶段 duration+10s；累计 stdout/stderr/trace 2GiB；free disk≥4GiB、MemAvailable≥1GiB、nofile≥256。预算轮询不是硬配额。运行中 `status=invalid` 是尚未满足整套完成条件的初始状态；以进程退出、`ended_utc` 和 `error` 判断是否已失败。失败非零退出、保留 invalid 与原始文件，不能自动重跑。角色动态账本由 `run-*/run.json` 组成，因此所有运行目录必须沿用 `run-` 前缀。

## 中间阶段与机制对照

`intermediate.py` 固定已解析的 v0.5-s1 commit `70b866bddbe7b4219037a93d23bde19702399d73`，只测三轮 1KiB。它属于引入区间定位，不计入正式18样本。

```bash
python3 benchmark/diagnosis/intermediate.py --role builder --build
python3 benchmark/diagnosis/intermediate.py --role builder --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s1/builder/run-S1-001
```

`timeline.py` 每组≤10s，启动自己拥有的 server，strace 仅附加该 PID；为兼容本机 Yama=1，只有本次 tracee 在 exec 前调用 `PR_SET_PTRACER` 允许同用户调试，系统 Yama/TCP 参数未改；客户端记录头部/正文收到的纳秒时间，每组在三个独立连接上各顺序32请求。单因素只改变客户端收到响应头后的 `TCP_QUICKACK=1`；服务端字节、参数和日志策略不变。假设：拆开发头部/小正文，与客户端 delayed ACK 的交互产生等待；反证条件：正文等待不消失或服务端 sendfile 本身存在相当等待。时间线和单因素结果不参与正式吞吐。

按以下六组运行（每角色最多六组）：

```bash
python3 benchmark/diagnosis/timeline.py --role builder --label A --output .cache/v0.5.1-s1/builder/run-timeline-A
python3 benchmark/diagnosis/timeline.py --role builder --label B --output .cache/v0.5.1-s1/builder/run-timeline-B
python3 benchmark/diagnosis/timeline.py --role builder --label B --quickack --output .cache/v0.5.1-s1/builder/run-timeline-B-quickack
python3 benchmark/diagnosis/timeline.py --role builder --label C --output .cache/v0.5.1-s1/builder/run-timeline-C
python3 benchmark/diagnosis/timeline.py --role builder --label C --quickack --output .cache/v0.5.1-s1/builder/run-timeline-C-quickack
python3 benchmark/diagnosis/timeline.py --role builder --label S1 --output .cache/v0.5.1-s1/builder/run-timeline-S1
```

源码身份和结果记录见 `manifest.json`、`run.json`；短跟踪见 `syscalls.trace`。对照通过不等于生产修复完成，也不证明 QUICKACK 是服务端正确修复方案。S2 须另行批准设计。

## 本次记录

[Builder S1 结果](../results/V0.5.1/S1-builder-001.md)保留首次invalid、有效18样本、中间阶段及六组机制对照。公开紧凑归档由以下只读收集入口生成；大日志留在角色目录，归档保存其hash及逐样本量：

```bash
python3 benchmark/diagnosis/collect.py --role builder --output benchmark/results/V0.5.1/builder-001
```

输出必须是新目录；Reviewer可换成自己的新结果目录。`collect.py`只整理证据，不重算或改写原始run的有效性。

F-01最小返工后的最新说明见 [Builder002](../results/V0.5.1/S1-builder-002.md)。中间套现在在尾部重新核验wrk/动态库、共享脚本和自身身份后才生成成功汇总；最终必要测试为38项。旧报告与旧中间套保留，Builder最终中间证据采用`run-S1-002`；复现仍使用上述命令并选择新目录。
