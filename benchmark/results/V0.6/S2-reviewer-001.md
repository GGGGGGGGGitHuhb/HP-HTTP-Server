# V0.6/S2 Reviewer独立矩阵

2026-10-08：Reviewer独立固定源码Release构建、功能反例、唯一M2冒烟及正式18样本通过，18 valid / 0 invalid / 0 NotRun。独立逐原始ticks/wait4/clock复算CPU及原stdout标量/三轮汇总；这是Reviewer自己的测量，未与Builder或历史样本混合。阶段最终同步由Leader负责。

[机器结果](S2-reviewer-001-samples.json)；[固定矩阵与命令](../../matrix/README.md)。产品固定 `1340f5bb8a303769a8be32b2d8cc29482fd2a8fe`，tree `f2fdabf35fb754b7f4e981708efd34e40e53ceac`；Release C++20/-O3/-DNDEBUG，无LTO/native/sanitizer；固定wrk SHA `b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d`。

| 档位 | QPS median [min,max] | 接收MiB/s median | P99 ms median [min,max] | server CPU单核% median | client CPU单核% median | server RSS采样max KiB median |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| M1 | 2120.60 [1906.21,2308.10] | 2.28 | 0.817 [0.785,0.887] | 9.35 | 5.65 | 4800 |
| M2 | 44884.42 [39283.03,45849.80] | 48.33 | 1.648 [1.491,2.013] | 135.89 | 99.94 | 5280 |
| M3 | 52514.45 [50786.18,52615.08] | 56.54 | 811.771 [390.712,1188.674] | 270.89 | 203.82 | 7200 |
| M4 | 9476.90 [9416.37,9826.00] | 10.16 | 5.090 [5.061,5.346] | 102.80 | 119.63 | 4640 |
| M5 | 5350.92 [5062.27,5399.34] | 335.01 | 13.014 [11.838,13.913] | 59.85 | 102.31 | 5440 |
| M6 | 414.06 [382.39,417.13] | 415.80 | 178.022 [167.016,194.914] | 17.71 | 99.18 | 5280 |

每个样本固定2s预热+10s测量，五measurement errors全零；pre/post各3次200/长度/正文SHA/无尾字节审计，短连接每次EOF重连、keepalive同socket，所有自有server/wrk回收，最终S1 active/logger_pending为0且总账平衡。CPU schema1保留原始/proc ticks与Hz、server PID/starttime/前后读取clock、单child wait4 PID/usage，以包围monotonic wall为分母；单核100%口径可大于100%。RSS是每秒采样最大值，VmHWM可能含预热，不是精确峰值。

延迟为wrk校正分布，population_status=not_collected、corrected_population/nonzero_bins为null，不是0或请求数。三轮分位数只是各轮分位数的中位，不是合并请求P99；接收MiB/s包含headers。S1 completed是kernel排空并包含审计/预热/退出，wrk requests是不同窗口client completed，不能机械相等。

WSL2同机loopback、closed-loop、hot-cache及调度噪声限制适用；M1/M2/M3同时改变连接数和workers，不证明单独多核线性扩展。没有性能通过门槛、恢复/根因/物理机容量宣称；RO-002/TD-001/TD-006和V0.5.1搁置未完成保持。Builder首次失败、R001 CPU输入缺失与Reviewer001 FAIL原始材料不覆盖。
