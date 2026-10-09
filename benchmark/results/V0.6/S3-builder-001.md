# V0.6/S3 Builder首次取样 — 权限阻塞

2026-10-08：**BLOCKED（ptrace权限），没有产品性能结论**。固定产品1340f5b；fastchecks7类通过，唯一能力probe失败，正式4样本全部NotRun，未启动正式suite、未重试/改设置/换工具。机器事实见 [数据](S3-builder-001.json)，已批准取样口径见 [analysis入口](../../analysis/README.md)。

实际`/usr/bin/strace -f -c -p 26872`返回：`attach: ptrace(PTRACE_SEIZE, 26872): Operation not permitted`。server PID26872/starttime17042093，tracer PID26876/starttime17042094；tracer exit1、server exit0，均reaped且未forced。附着未成立、没有系统调用summary或性能样本；不能将能力失败解释为服务器长尾、CPU热点或吞吐问题。

180s新窗口内fast1.115140s、probe0.320484s；首次失败后停止，所有11个受控child已回收，四条正式负载未执行。费用/空间与旧数据独立保留；用户10学习源与全部8个既有S2预算/结算SHA一致。后续采集机制和额度由Leader决定，现入口不能绕过此次已消费probe自动重试。S2有效基线保持；S3未完成、V0.5.1搁置未完成，RO-002/TD-001/TD-006不关闭。
