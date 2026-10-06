# V0.5.1 / S3 Builder 009 — R004 日志输出成本对照

日期2026-10-05。状态建议：**阻塞**。诊断observed，S3正式验收仍未通过；所有动态已停止。

## 依据、范围与路线

Approved R004 revision1、原S3及R003、Builder008/Reviewer007；复用原生WSL、同分支、Builder tmp/cache及原1800秒累计账本。Reviewer执行前静态确认无阻断，5项工具反例通过后Leader授权执行。只新增sink_diagnostic.py/test_sink_diagnostic.py/SINK-DIAGNOSTIC.md、报告及公开证据；原R003/正式工具及产品保持。你方注释及全部175保护hash一致。

预期只改变stderr输出目的地，观察日志输出成本与P3长尾是否有关。四次固定D/P3、4worker/t2/c128/1KiB，每次5秒预热20秒测量，sink依序file/null/null/file。null句柄仅传给server的Popen，核实际fd2指向/dev/null及设备1:3；stdout和wrk日志保留。日志提交、锁和flush调用没有禁用。null日志生产量unknown，不记零。

## 命令与验证

- 预算监督下python3 -B benchmark/regression/test_sink_diagnostic.py：run-sink-unit-001，5/5通过，约0.185秒；计入原账本。
- python3 -B benchmark/regression/sink_diagnostic.py --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/builder/run-sink-001：唯一套，exit0/observed，102.013389349秒。
- python3 -B benchmark/regression/collect.py --role builder --output benchmark/results/V0.5.1/S3/builder-r004：21条旧新记录，附原threads.jsonl及hash、r004-analysis.json和离线复算脚本。

4样本、8个phase五类错误全零，审计/前后身份一致，12个owned最终回收正常无forced；793采样、采样CPU3.118909秒、最大gap0.210637秒。原始所有结果保留。

|顺序|sink|QPS|p50 ms|p99 ms|max ms|server CPU %|wrk CPU %|
|---|---|---:|---:|---:|---:|---:|---:|
|1|file|48641.478|1.932|121.262|313.991|339.14|137.74|
|2|null|49521.403|2.139|32.219|145.427|317.18|137.52|
|3|null|53087.232|1.969|57.408|298.683|317.61|139.94|
|4|file|50055.154|2.019|133.180|435.809|336.57|136.66|

## 证据支持与限制

null两次P99低于file两次，回切file重新上升；server CPU亦在null下降约20个单核百分点，wrk CPU接近。按main创建顺序推定的logger线程CPU依次63.0/43.9/44.9/55.9%，四worker约68–72%，总CPU下降主要集中于疑似logger。该受控反序证据支持“日志输出目的地成本参与P3长尾”，强于仅观察日志量，但仍不能区分每条flush的系统调用、存储写入和调度的具体贡献。线程无具名角色，logger归属是启动顺序推断。

null仍有32.219/57.408ms校正P99，且原P3正式阈值为D/C≤0.25；本实验没有C对照，不能与旧C拼出正式PASS。不是生产修复，不证明锁争用已经定位。sched_schedstats=0、离散采样和wrk校正直方图边界继承R003；不把wchan比例当阻塞时间，不外推每请求路径。

## 预算与交接

原账本累计1670.759248781秒，剩129.240751219秒，未增加预算。日志1,786,923,540字节低于2GiB，null丢弃的字节不可计数。175保护文件不变，套前后工具/产品身份一致。公开builder-r004保存固定源身份、原始wrk/审计/回收、null fd2证据、时间线及静态复算；大server文件日志本地保留并公开hash/bytes。未追加样本、未改正式门槛、未提交发布。

交Reviewer只读复算，Leader汇总已支持/削弱/未决假设。后续生产修复、启用内核统计或增加预算须单独设计。本Builder停止动态，不自行将诊断关联升级为S3完成。
