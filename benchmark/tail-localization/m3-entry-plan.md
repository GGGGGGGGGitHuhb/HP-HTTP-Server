# M3 两个执行入口（静态准备，尚未准入）

阶梯使用 `staircase_sample.py`，原固定 E-S3 Release `d5053e...`、原 sealed wrk `b10e53...` 与固定导出内原 regression executor/run/model/summary；原 INFO、原 warmup 和 measurement 分进程协议、5+20 秒、4 server workers/2 wrk threads保持。唯一顺序如下，所有请求均为原 `payload-1024.bin`（SHA `785b0751...`）。每轮独立 outer45秒/预留7秒，真实 native power HTTP 首次窄提升，不走 root marker launcher。

|number|runId|connections|
|---:|---|---:|
|1|run-staircase-01|8|
|2|run-staircase-02|32|
|3|run-staircase-03|64|
|4|run-staircase-04|128|
|5|run-staircase-05|128|
|6|run-staircase-06|64|
|7|run-staircase-07|32|
|8|run-staircase-08|8|

对应每轮 wrapper 参数为 `localize_v7.py --role builder --run-id <上述runId> --kind staircase --seconds 45 --static-inventory-sha256 8199f58a268884125ce50c454821fc6318533e50824817e2306726273c782b61 -- python3 benchmark/tail-localization/staircase_sample.py --number <上述number>`。若新返工改变 wrapper，应重新给完整 SHA/准入命令，不能把本计划当执行授权。入口只允许前序 staircase 全valid且当前running record与固定number吻合，不重跑/跳序；保存 CPU/RSS/全部错误/原corrected分布及实际命令、环境和时长。原 wrk 无 request-level raw数据，明确不可用。

ABBA 另用诊断 builds及已通过 smoke 的 root/power mapping入口，只允许128连接、5+20秒、固定4 selected、同预分配/连接冻结。顺序 Aoff/B on/B on/Aoff；每轮45秒共用 outer绝对期限。不得把该客户端用于原阶梯。首 smoke001 保留 invalid/20秒/bytes unknown；Leader 已恢复自有资源并绑定 R007 当前上界 receipt，仅解除该历史 unknown 的后续入场阻止。root/power v2 已静态封存，尚待纯回归和唯一 smoke002，因此 ABBA 精确命令暂不封存、更不执行。

分析必须同时列原阶梯两次128结果对A（诊断构建整体差别）及A/B中位值；QPS变化>15%或可比 raw P99变化>25%标记扰动/自然波动未分离。原工具只有 corrected指标时并列而不替换 raw定义。≥50ms只取真实完整请求；未复现/记录不完整/扰动未知则核心目标 BLOCKED，不自动补样。

R007 恢复后入口使用 v7 预算守卫，仍须先完成 M2 唯一剩余 smoke 与必要独立审查。原 stairs 仅原 E/原 wrk，不启动诊断 buffers/root marker。静态输入函数引用的 resource_gate/wire_types 源字节已纳入入口 SHA 核验；不将通用资源门槛视为任何实际负载完成。
