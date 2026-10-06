# V0.5.1 S3 Builder 014 — R012 停止结论

2026-10-06。Approved R012仅完成一次微型，invalid；本轮control与observed两个HTTP样本均未启动，按任loss即停收口，无重试。没有生产改动、正式性能验收或发布，S3仍返工中/FAIL，产品根因未知。

微型单CPU紧密完成10000次sendfile/read，收发各10240000B，实际buffer258KiB/cpu；trace logical1710343B/stored89435B，loss21284。最终空读及entries0只证明残留排空，不能恢复已丢事件。外层同ledger计费1.285998秒，恢复摘要无cleanup_errors、私有instance/mount移除、owned空。

producer仅每1000次sleep1ms，覆盖约60.672ms，是突发压力，不等同P3持续请求流；本次失败拒绝工具准入，不能证明P3也必败或理论上无法零丢失。reader已在释放producer前打开；最大排空间隔33.722ms有记录，但未单独测首次read延迟/各阶段速率，不能把全部loss归给某一机制。guard内部占drain约1.58%，目录扫描并非本次主要已量化耗时。

新工具限运输syscall及双方各一个worker调度事件，保留真实marker映射、gzip/容量/期限与身份清理。Reviewer013已独立核验gzip CRC/bytes/SHA与残存事件数量，trace仍不完整；R012动态1.672080秒/248207B，R008累计113.643326秒/325893577B快照均在预算内，172保护文件/seals一致、无unknown/running；没有把失效trace用于CPU/锁/I/O/网络/缓存的产品因果解释。

Leader下一步优先校正与生产参数匹配的准入gate；事件驱动或专用drainer仅为待审候选，本轮不执行。本报告定版收口。完整报告：`docs/builder/reports/V0.5.1/S3-report-014.md`。原始证据：`.cache/v0.5.1-s3/builder/run-r012-micro-001`。
