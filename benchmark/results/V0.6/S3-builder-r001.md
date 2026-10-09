# V0.6/S3 R001 Builder — 正式套不完整

2026-10-08：父strace launch probe有效；正式3valid/1invalid/0NotRun，不能形成有效完整性能分析。第4 M6 traced measurement末读取server `/proc/7009/stat`时已不存在，wrapper exit=-13；server最终metrics缺失，清理标invalid。已有syscall摘要与client五errors0不能替代终态/CPU原证据。根因未知，没有新采样、诊断或修复。

原始失败标量、完整生命周期summary、identity/cleanup及预算见 [机器事实](S3-builder-r001.json)。wrapper被控制器直接reaped，tracee及线程消失/端口关闭可核；控制器没有直接wait4非child server。measurement client的cleanup/rusage未持久化，不补造退出值。旧sibling attach权限失败与费用保持；R001独立新180s已消费，等待Leader决定。S3未完成，无热点/长尾根因或性能恢复结论。
