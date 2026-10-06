# V0.5.1 / S3 Builder 008 — R003 有限长尾定位

日期2026-10-05；状态建议：**阻塞**。诊断本身observed，正式S3未通过。

## 依据、预期与修改

按原Approved S3、R003 Approved revision1、审查计划，读取Builder007/Reviewer006和交接路线。原生WSL、独立Builder tmp/cache、原累计账本和C/D构建；分支codex/v0.5.1-s3-performance-acceptance。预期借客户端线程单变量及线程观测区分长尾候选，不代替性能验收。只新增tail_diagnostic.py、test_tail_diagnostic.py、TAIL-DIAGNOSTIC.md、当前报告及公开证据；原工具、生产C++、测试、根文件和用户注释不改。

采样使用临时OwnedProcess子类的alive检查点，目标100ms、无后台线程；复用executor全部fixture/审计/超时/回收。missing数据保留unknown，明确PID身份漂移锁存invalid。Reviewer执行前指出信号可能中断原close以及身份漂移未锁存，两项已在真实诊断前修正：外层finally停观测、忽略后续信号、遍历所有owned.close并逐项记录失败。此前9反例保留；新增三项后12反例通过。未扩大正式门槛，无新产品策略或回调变化。

## 命令与实际结果

- run-tail-unit-001：预算监督下python3 -B benchmark/regression/test_tail_diagnostic.py，9/9，通过，实际约0.163s。
- run-tail-unit-002：同命令最终12/12，通过，实际约0.167s；两次均计入原ledger，总必要工具耗时低于60s。
- python3 -B benchmark/regression/tail_diagnostic.py --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/builder/run-tail-001：Reviewer静态及Leader确认后执行唯一套，exit0，observed，152.950395792s。
- python3 -B benchmark/regression/collect.py --role builder --output benchmark/results/V0.5.1/S3/builder-r003，附加原threads.jsonl/身份hash和离线分析脚本，不发布构建或私有docs。

六样本D，12个phase五类错误零，前后精确审计/身份和最终18个owned回收通过，无forced；175保护hash不变。1188连续序号观测，采样自身CPU4.067753s，整套最大gap0.591242s；采样有扰动，不外推未观测运行。

|顺序|场景|wrk线程|QPS|p50 ms|p99 ms|max ms|
|---|---|---:|---:|---:|---:|---:|
|1|P1|1|3004.963|0.318|0.659|6.065|
|2|P3|2|57042.097|1.834|97.725|249.791|
|3|P3|4|50746.497|1.953|569.808|1046.885|
|4|P3|4|49545.762|1.990|771.588|1375.648|
|5|P3|2|49423.871|1.921|851.471|1118.464|
|6|P1|1|2870.653|0.313|0.730|3.031|

## 观测解释及未确定原因

P3 t4两次没有稳定优于t2；同t2前后差异很大，不能认定客户端线程不足已证实或修复。P1首尾QPS约3005/2871，未复现旧三轮53.9%跨度，但两次诊断不构成正式稳定性验收。wrk直方图校正语义由Reviewer另核，不能用QPS乘mean直接推定实际在途请求数。

P3主线程近0%CPU、主要epoll；按main启动顺序推定的logger线程约54–59%单核CPU，futex抽样约16–23%；四worker各约65–72%，没有持续单worker失衡证据。TID无角色命名，logger归属属于创建顺序推断；futex也可来自正常空队列等待，wchan抽样比例不是阻塞时间比例，0不代表持续满载。每响应INFO经过全局logger和队列mutex，consumer每条flush；旧正式P3约110–120MB/25s日志形成具体候选，但未隔离sink变量，不能断言锁/落盘已是根因。

sched_schedstats原值0；虽schedstat可读且有非零值，不能假定计数完整或据零排除等待。某些约100ms窗口stat tick增量超过单线程100%，须视作计数更新/采样一致性限制，不能当物理瞬时峰值；r003-analysis.json保留原计算和警示。整段CPU与wchan仅支持成本/候选，不足以定位每请求长尾。

正常文件非pipeline请求在Session中pauseReading，读循环退出，flushOutput文件预算后break；持续单连接无限读取完整请求在此路径缺少支持。本轮没有修改公平性或日志策略。

## 预算、证据与交接

累计1568.558121472s，剩231.441878528s；无第四套正式矩阵，无第二套R003诊断。公开builder-r003包含全部19条旧新run副本、原wrk输出、当前threads时间线和hash、r003-analysis.json及复算脚本；大server日志本地保留公开hash/bytes。工具套前后身份一致。仍缺能区分日志sink成本的受控对照及可靠逐请求/调度关联证据；本次不能关闭RO-002/S3/V0.5.1。

请Reviewer独立复算当前观察及边界，Leader决定是否在原剩余预算中授权更有辨别力的单变量诊断。Builder不自行追加动态、不修改原性能标准、不发布。
