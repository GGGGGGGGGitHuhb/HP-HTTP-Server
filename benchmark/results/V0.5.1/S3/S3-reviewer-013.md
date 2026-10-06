# V0.5.1 / S3 Reviewer 013 — R012短诊断准入复核

2026-10-06，唯一结论：**FAIL**。按Approved R012与既有Reviewer/route规则静态预审并独立复核。R005有效30样本矩阵保留，正式性能门槛仍未达标。本轮唯一微型invalid后立即停止，没有control/observed HTTP或新增微型，产品根因未知。

精简工具只选server首worker sendfile64、client首worker read及两TID调度；逐TID marker身份映射、不用PID差值，完整窗内调用与调度才可解释。Reviewer发现结束排空未检残留entries，Builder定版修正为drained_to_empty、所有CPU entries0、loss0同时成立；两份final SHA独立匹配。预审通过仅允许执行微型，不等于动态准入通过。

微型请求buffer256KiB，实际258KiB/CPU，共20CPU；单CPU helper突发10000对sendfile/read，每次1024B、每1000轮仅sleep1ms。覆盖约60.672ms，CPU0 read_events18916+overrun21284=40200，entries0且已排空仍不能补回丢失事件。支持结论是**这一小buffer突发参数未通过可靠采集准入**；计划HTTP使用约4096KiB且server/client分线程，不能据此证明真实P3必失败，也不能排除锁或归因磁盘、网络、scheduler。未运行完整因果decoder；R运行态不等于pureCPU。

Reviewer原ledger `run-r012-incomplete-integrity-001`独立核验wall0.3860816955566406秒：CRC/hash/rawbytes正确；logical1710343B、stored89435B、18979行；观察sendfile enter4674/exit4687、read enter4687/exit4684、loss21284。`reviewer/r012/incomplete-independent.json`明确trace_is_complete=false、causal_decomposition_performed=false。仅压缩文件完整性通过，不能声明完整调用配对通过。

`reviewer/r012/independent.json`与`final-stage.json`独立复算：Buildercharged2896.698628956778、Reviewer186.34735703221213秒，无unknown/running；R012动态1.6720800399780273秒/文件snapshot248207B，R008累计113.643326200312秒/325893577B，均在60秒/120MiB及300秒/500MiB内，旧R005剩余额度仍足。snapshot之后少量报告元数据另记，不重置预算或删除失败证据。

172个非例外protected项无变化，descriptor/patchseal一致；本次cleanup_errors为空、instance/mount移除、默认tracing状态恢复，监督记录进程回收。未改生产代码或正式门槛，未重新宣称功能测试通过。P3场景corrected P99 E/C=3.2446仍未达到≤0.25正式门槛，本轮没有可解释生产根因的完整证据。所有dynamic停止，阶段同步交Leader，S3不得关闭。

勘误：尾段明确失败的是P3场景，避免与问题严重度P1混淆；Builder014摘要与本次独立counts/CRC/停止范围一致。仅文字复核，无新增dynamic。
