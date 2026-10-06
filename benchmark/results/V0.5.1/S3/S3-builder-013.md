# V0.5.1 S3 Builder 013 — 有界观测结果

2026-10-05。依据Approved R009/R010/R011。没有生产代码或正式门槛变化，无发布；正式S3仍返工中/FAIL，产品根因未知。

基础能力恢复：实际事件名为sendfile64；用自有trace_marker逐线程建立namespace PID/TID到kernel身份映射，微型一对syscall返回1024、调度事件及恢复通过。诊断preload仅在线程入口记录身份；真实负载始终power身份，生产binary冻结。

本轮仅三次HTTP尝试，均冻结E/P3、4workers、wrk -t2 -c128、1KiB、5+20秒：

| 尝试 | 结果 |
| --- | --- |
| A1 | valid，QPS52341.7053、corrected P99 45.009ms；旧guard有虚拟树遍历，不能与新路线控制等同 |
| B2 | loss3884257，logical100570B/stored9165B，invalid |
| B3 | loss3808728，logical28896970B/stored2339408B，invalid；A4未启动 |

原ABBA不成立，没有额外A4/B4或HTTP，没有有效P3 syscall/调度因果分解。只选logger及双方各一个worker，其他线程、部分调用方向、覆盖窗外及单请求关联不覆盖。

已确认工具问题：原guard先遍历tracefs再排除；移出日志树后，B3仍有7813次容量扫描，guard总9.104184秒、drain10.202213秒、drain扣guard1.873376秒；内部guard占81.6%。最终微型guard总0.004705秒、drain0.655299秒、扣guard0.650837秒，内部占约0.681%。不同负载不能据此宣称优化收益；微型仍丢失48185事件，不能声称可靠采集能力完成。

2000对低量容量微型loss0；随后严格buffer读回检查失败，保留原run。最后100000次微型实际收发102400000B，捕获enter75941/exit75939，logical14992640B/stored600263B，invalid。实际buffer4099KiB/cpu，共20CPU。事件格式二进制payload下界与实际容量用于设计，不能用文本长度或成功收发代替完整事件通过。

所有失败保留，清理摘要无遗留owned/cleanup_errors，私有实例和挂载恢复、默认状态保持。离线micro003 fixture的CRC/hash核验及坏CRC拒绝通过；原未经预登记的1.631958秒离线执行以同ledger保守补3秒，并保留偏差。

最终独立快照：Builder累计2895.4126306123567秒；两角色R008起合计111.97124616033398秒≤300；stage全部实际325641827B≤500MiB。Reviewer已确认172个protected及candidate seals一致、无unknown/running；不替代正式性能验收。

完整说明：docs/builder/reports/V0.5.1/S3-report-013.md。原始run：.cache/v0.5.1-s3/builder/run-r009-*；独立预算/保护：.cache/v0.5.1-s3/reviewer/r009/{independent.json,final-stage.json}。R012低事件量诊断仅Draft，本轮停止；不能据这些失效trace把锁、I/O、网络、CPU或缓存认定为根因。
