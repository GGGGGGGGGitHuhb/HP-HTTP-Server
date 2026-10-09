# V1.0 / S3 Reviewer R004 独立完整验证

独立固定581候选导出、原四文件补丁；新角色编排及动态状态不复用Builder。Debug/Release各14/14、四HTTP smoke、README/CLI/404/405/SIGINT与SIGTERM、checker true/false、五ASan/UBSan/LSan（detect_leaks=1）全部通过。唯一M2准入有效；三档三轮9valid/0invalid/0NotRun，全部pre/post同socket三响应审计、五类wrk错误0、守恒/退出metrics、PID/starttime和正常回收通过，无补样本。最后监督封包/扫描若失败以母进程FAIL为准，不提前关闭阶段。

条件：WSL2 loopback、默认ACK、热缓存、C++20 Release -O3 -DNDEBUG、固定wrk4.1.0/Lua及库SHA；M2=1KiB/2workers/2threads/32connections，M3=1KiB/4/4/128，M6=1MiB/2/2/32。准入1+1s；正式每样本2+10s，固定M2/M3/M6、M6/M3/M2、M3/M2/M6。

每栏为三轮median [min,max]；QPS、含headers接收MiB/s与每轮校正P99独立从原stdout复算，P99不称合并请求分布。

| 档位 | QPS | 接收MiB/s | 校正P99 ms |
| --- | ---: | ---: | ---: |
| M2 | 35356.001 [33623.467, 36254.287] | 38.068 [36.202, 39.035] | 1.978 [1.959, 2.034] |
| M3 | 58449.410 [53328.662, 59231.063] | 62.932 [57.419, 63.774] | 530.383 [273.912, 783.770] |
| M6 | 425.869 [414.306, 425.948] | 427.498 [415.920, 427.544] | 181.181 [172.266, 278.011] |

Reviewer原诊断连续clock由批准封存574.392325509s，其中诊断36.380733385s，其他/等待538.011592124s；R003失败批行政封存822.277372178s（准确dynamic terminal缺失）；R004准确runtime743.805646360s、行政持续1171.405364720s分开；累计旧账2568.075062407s。本轮新1800s为显式追加，不重置旧费用。新clock起点2026-10-09T07:20:02.580544+00:00；本文件费用/峰值是生成时快照，末次精确费用与active end由母进程terminal_snapshot记录。

本次scan固定0.25s计划tick，max开始间隔0.419322277s、完成间隔0.430212551s、自身耗时0.065116255s；静止交接不伪造采样，总预算含等待。每原逻辑socket操作2s deadline、≤0.1s select切片、20ms尾窗、5s TERM等待切片保持；真实partial send/EAGAIN/short read/timeout/finally及原scan真树分类等价已核。瞬时未采峰值仍可能存在，未声称连续资源观测。

server metrics按每样本JSON独立列示，不将客户端零错误称为所有server计数为零。历史r1-M6 aborted/errors各1、两旧失败批和未复现诊断保留。新完整验收不补造原header EOF根因，RO-002/TD-001/TD-006仍Open。当前14注册包含HTTP黑盒别名，23旧目标仍冻结；不等同旧28/28或生产网络容量。

[完整结果/原stdout SHA/argv与环境](reviewer-rework-005.json) · [一次性编排源文本](reviewer-rework-005-source.py)。源文本是本轮证据，大日志留Reviewer独立cache；不会成为常规工具。
