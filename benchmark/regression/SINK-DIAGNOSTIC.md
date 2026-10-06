# R004 日志sink单变量诊断

唯一命令（本仓库已执行，不得重复）：

```bash
python3 -B benchmark/regression/sink_diagnostic.py --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/builder/run-sink-001
```

沿用README中原Builder tmp/cache/LD_LIBRARY_PATH及真实HTTP窄权限路线。固定D/P3-t2-c128，四样本file/null/null/file，各5+20秒，原账本预留140秒，启动要求至少300MiB日志余量。diagnostic-sink结果只有observed/invalid，不替代正式性能验收；先执行Reviewer静态审查及总20秒预算内必要反例。

仅server子进程stderr在构造时选择/dev/null，核验实际fd2目标及字符设备1:3。server stdout、wrk stdout/stderr仍保存，生产日志调用/锁/flush未改。null的日志生产字节unknown，不能将设备stat的0当无日志。复用R003观测、原executor审计与超时；外层兜底停止观测并回收全部本次owned进程。原始工具和结果不覆盖。

与R003相同，采样自身有扰动，schedstats关闭和短窗口tick异常限制判读。收集器保留原始wrk/审计/清理及日志hash；时间线与hash另行复制。相对改善只是有限因果对照证据，不能直接认定生产修复或普适收益。
