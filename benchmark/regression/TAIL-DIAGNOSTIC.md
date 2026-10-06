# R003 单次长尾定位

本入口仅供Approved R003：固定D，P1 / P3-t2 / P3-t4 / P3-t4 / P3-t2 / P1，各5s预热+20s测量。结果为diagnostic-tail/observed或invalid，不产生正式性能PASS。原工具及门槛保持。每角色原1800s累计中预留240s，唯一套，不重试；工具反例另在累计60s内计费。

沿用README中的Builder tmp/cache、wrk库和本地HTTP权限路线：

```bash
python3 -B benchmark/regression/tail_diagnostic.py --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/builder/run-tail-001
```

本仓库run-tail-001已经执行，不得重复执行上述命令。新增运行须Leader明确授权，不得改名绕开唯一kind检查。

临时OwnedProcess派生类在alive检查点每100ms目标间隔采样；实际间隙保留。无后台线程，不改原executor文件。server/wrk线程stat、schedstat、上下文切换/wchan、IO、日志大小、affinity、cgroup/system原始观测写threads.jsonl。读失败明确unknown；身份漂移或采样写异常锁存invalid，在当前有界样本回收后停止；外层finally禁止后续信号、停观测、逐个回收全部自有进程。原HTTP审计、phase/global watchdog和日志守卫仍运行。

观测可能扰动负载；sched_schedstats关闭、wchan零/不可读及线程退出不能解释为无等待。只可对同PID/TID/starttime相邻有效读数差分；大于100%的单线程短窗口tick增量须披露为计数/采样一致性限制，不能当真实CPU峰值。wchan抽样比例不是阻塞时间比例，futex可能是logger正常空队列等待。wrk校正分布不是逐请求原始时间线。

公开结果须连同threads.jsonl及其hash保存；既有collect只打包常规记录，故本次另复制时间线、静态分析和完整工具hash。大server日志继续本地保存并公开hash/bytes。
