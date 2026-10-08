# V0.6/S2 R002 Builder完整矩阵（待独立审查）

2026-10-08：本次独立新套18 valid / 0 invalid / 0 NotRun；每组3轮，不混入首次失败套的两条样本。Reviewer独立套尚未执行，阶段未完成。固定产品、构建、wrk身份及原始scalar见 [机器结果](S2-builder-r002-samples.json)，运行与指标口径见 [入口](../../matrix/README.md)。

schema2仅固定数量标准mean/P50/P95/P99/max调用，wrk校正分布精确人口未采：population_status=not_collected，corrected_population/nonzero_bins为null，不是0或requests。CPU evidence schema1保留真实server ticks/频率/读取clock、client wait4 PID/usage及包围clock，独立消费者从原值复算18与6组汇总通过；所有measurement五类errors=0，pre/post各3次HTTP正文/header/SHA/无尾字节审计通过；最终metrics active0、总账平衡，server与wrk全部reaped。

| 场景 | QPS median [min,max] | MiB/s median | P99 ms median [min,max] | server CPU单核% median | RSS采样max KiB median |
| --- | ---: | ---: | ---: | ---: | ---: |
| M1 | 2891.66 [1974.57,3143.35] | 3.11 | 0.719 [0.680,0.873] | 9.17 | 4800 |
| M2 | 43234.00 [41326.48,45047.66] | 46.55 | 1.810 [1.588,2.137] | 136.73 | 5280 |
| M3 | 57550.02 [50950.18,58093.99] | 61.96 | 272.549 [253.841,1478.748] | 277.83 | 7200 |
| M4 | 8847.24 [8648.21,8982.74] | 9.48 | 6.042 [5.089,6.884] | 88.51 | 4640 |
| M5 | 5574.74 [5096.42,5735.14] | 349.01 | 16.339 [10.744,19.469] | 56.80 | 5440 |
| M6 | 419.18 [372.27,424.73] | 420.51 | 197.746 [179.813,247.466] | 16.76 | 5280 |

机器文件保留六组每指标三原始数、median/min/max/(max-min)/median及逐样本标量/stdout/CPU/RSS/完整wall。QPS按client requests/duration；接收MiB/s包含headers。延迟分位数不合并成全体P99；CPU单核100%与每秒RSS采样最大值口径，VmHWM可能含预热。server completed是kernel排空且包含审计/预热/退出，不能与wrk测量requests机械相等。

本次最后R002 600s额外恢复获明确PM批准，原及R001 budget/settlement SHA引用且字节不改，不重build。fastchecks18类（包含实际Run第三synthetic失败2/1/15及CPU坏输入）、唯一M3 1+1s smoke、唯一全新18 suite通过；资源按旧+新累计，原首次超时/费用完整保留。首次schema1 not_run16重复含失败M3，实际2valid/1invalid/15NotRun，见 [首次不完整结果](S2-matrix.md)。

WSL2同机loopback、closed-loop、hot-cache和调度噪声限制保留；本结果不宣称性能恢复、服务器长尾根因、独立多核线性扩展或物理机容量，RO-002/TD-001/TD-006不关闭。
