# V0.6/S2 R001 Builder矩阵（CPU证据审查FAIL）

2026-10-08：本次独立新套18 valid / 0 invalid / 0 NotRun；每组3轮，不混入首次失败套的两条样本。Reviewer独立套尚未执行，阶段未完成。固定产品、构建、wrk身份及原始scalar见 [机器结果](S2-builder-r001-samples.json)，运行与指标口径见 [入口](../../matrix/README.md)。

schema2仅固定数量标准mean/P50/P95/P99/max调用，wrk校正分布精确人口未采：population_status=not_collected，corrected_population/nonzero_bins为null，不是0或requests。所有measurement五类errors=0，pre/post各3次HTTP正文/header/SHA/无尾字节审计通过；最终metrics active0、总账平衡，server与wrk全部reaped。

| 场景 | QPS median [min,max] | MiB/s median | P99 ms median [min,max] | server CPU单核% median | RSS采样max KiB median |
| --- | ---: | ---: | ---: | ---: | ---: |
| M1 | 1974.68 [1904.26,2174.82] | 2.13 | 0.832 [0.811,1.660] | 10.87 | 4800 |
| M2 | 41592.80 [27788.47,45372.36] | 44.78 | 1.806 [1.539,8.979] | 137.07 | 5280 |
| M3 | 48903.49 [48687.79,56962.50] | 52.65 | 490.635 [75.670,1056.915] | 277.83 | 7040 |
| M4 | 9392.00 [9037.17,9787.73] | 10.07 | 4.974 [4.782,11.621] | 113.89 | 4800 |
| M5 | 5479.31 [3174.16,5514.15] | 343.05 | 12.516 [12.334,40.745] | 62.36 | 5280 |
| M6 | 405.21 [339.16,419.60] | 406.62 | 194.623 [170.492,338.791] | 17.32 | 5440 |

机器文件保留六组每指标三原始数、median/min/max/(max-min)/median及逐样本标量/stdout/CPU/RSS/完整wall。QPS按client requests/duration；接收MiB/s包含headers。延迟分位数不合并成全体P99；CPU单核100%与每秒RSS采样最大值口径，VmHWM可能含预热。server completed是kernel排空且包含审计/预热/退出，不能与wrk测量requests机械相等。

本次600s额外恢复获明确PM批准，原budget/settlement SHA引用且字节不改，不重build。fastchecks16类、唯一M3 1+1s smoke、唯一全新18 suite通过；资源按旧+新累计，原首次超时/费用完整保留。首次schema1 not_run16重复含失败M3，实际2valid/1invalid/15NotRun，见 [首次不完整结果](S2-matrix.md)。

WSL2同机loopback、closed-loop、hot-cache和调度噪声限制保留；本结果不宣称性能恢复、服务器长尾根因、独立多核线性扩展或物理机容量，RO-002/TD-001/TD-006不关闭。

2026-10-08独立审查勘误：此旧套CPU只有派生百分比，测量边界server ticks/单client wait4原值缺失，**CPU不可独立核验**；表中旧值为历史计算结果，不能当资源验收证据。旧机器文件/raw保持原字节，R002只通过新采补齐，不反推旧输入。
