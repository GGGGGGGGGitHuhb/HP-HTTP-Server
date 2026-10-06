# V0.5.1 S3 Builder 018 — 采样时点确认，原长段未复现

2026-10-06。Approved R016仅恢复原未运行的一次3+6秒观察；原R015 capability invalid保持，未改生产/发布，正式S3仍返工中/FAIL。

QPS53095.451715、corrected P99 47.840ms/max177.602ms、errors0。完整sendto67021对/perf221样本，trace与perf无loss/未知且完整排空，独立checker一致；无对照，不能证明正式改善。

最长sendto7.058ms，未复现R014240.784ms。距该调用exit59.212us的一个样本IP rb_insert_color，调用链为TCP发送路径；只证明这个时点位置，不能将整7ms分配给RB或解释旧240ms，也不能用无采样证明hostpause。

最大sendto gap70.166ms，blocked69.939ms。gap内逐字段syscall232与stack do_epoll_wait支持该时点epoll等待路径；快照非原子、statR/wchan0不能拼成一致瞬时状态，不能认定整gap都在epoll或这是性能缺陷，更不证明logger锁。还缺请求/ready关联以区分正常空闲和请求停顿。

全部动态停止；原baseline40秒/48MiB及R008300秒/500MiB不重置，独立Reviewer汇总共享子预算15.394606秒/42975277B snapshot，R008189.588411秒/506007415B，预算内；172保护/seals/清理成立，本报告定版。完整证据与边界：`docs/builder/reports/V0.5.1/S3-report-018.md`。
