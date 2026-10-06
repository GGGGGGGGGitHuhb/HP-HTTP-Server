# V0.5.1 S3 Builder 017 — 采样发生，原准入失败保留

2026-10-06。Approved R015只执行一次自有进程能力检查和既有记录的只读符号补证；没有HTTP、产品修复或发布，正式S3仍返工中/FAIL。

perf_event_open成功fd3，44条内核样本、身份/时钟/原始记录独立核验成立；原cap仍invalid，原因是符号工具错误要求kallsyms全局排序。它不是环境不支持采样的证据；清理完整，无强杀或遗留。

只读离线按core/module分组与严格text区间解析：44samples/unknown0，库存10组/329次地址回退。原cap未封boot_id，后来同release查询只能条件性确认当前core映射，historical_symbol_identity_verified=false；不能把它当历史HTTP函数证据。

原cap失败封存、不再采样或运行HTTP。新补充设计须Approved和独立准入后才能恢复尚未执行的观察，不重置40秒/48MiB或R008300秒/500MiB。最终独立预算/保护复核待收口。

完整说明：`docs/builder/reports/V0.5.1/S3-report-017.md`。
