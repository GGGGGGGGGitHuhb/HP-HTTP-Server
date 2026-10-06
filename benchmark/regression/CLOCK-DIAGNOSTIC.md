# R001 独立clock诊断

仅Approved S3-R001授权的诊断入口，不产生正式性能验收结果，也不执行第三套矩阵。每角色一次120s空闲观察及P4 C/D各5s预热、20s测量；全套240s含回收，并累计原1800s账本。当前Builder唯一动态已中止，不能重跑。

在原生WSL仓库目录，使用角色自己的现有C/D导出、refs及ledger；先完成Reviewer静态与合成反例核对，确保没有其他角色压测。TMPDIR/TMP/TEMP和XDG_CACHE_HOME使用该角色tmp/cache，wrk局部LD_LIBRARY_PATH使用.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu。无需下载或系统调整。

Reviewer入口：
```bash
python3 benchmark/regression/clock_diagnostic.py --role reviewer --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/reviewer/run-clock-001
```

合成反例入口test_clock_diagnostic.py，应在原budget.Reservation及supervision.run监督下累计角色账本。没有真实clock调整。clock-timeline.jsonl保存100ms目标采样、时钟缺测/暂停/差异、进程CPU及状态；采样开销计入包络，不作修正。time.clock_gettime不可用记unknown，采样间隔>500ms记缺测/暂停，不能从诊断未复现推断根因消失。任何日志、资源、累计预算或清理异常立即停止；普通样本错误仅允许另一个隔离label，不重试。用户中断整体退出。恢复调度后首次检查才能停止，宿主暂停期间无法保证清理。

最终输出始终kind=diagnostic-clock及NOT_APPLICABLE_DIAGNOSTIC，observed仅表示诊断观测完成，不是性能PASS。原formal executor/timeout/门槛不变。后续完整矩阵需Leader明确授权与至少900s剩余预留；不能换目录清预算。
