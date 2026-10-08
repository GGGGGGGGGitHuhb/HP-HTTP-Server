# S4 evidence schema 1（M1 候选）

## 固定记录

拟使用 little-endian 32 字节 record：`uint64 time_ns; uint64 value; uint32 connection_id; uint32 request_sequence; uint16 kind; uint16 flags; int32 result`。
每个文件 header 包含 magic/schema/record_size/record_count/capacity/overflow/writers_stopped/线程 PID、starttime、TID、worker、boot/time namespace、clockres。connection_id 只能查预热前封存的四元组与连接生命期 metadata，不能从 fd 推导。
kind 与 value/result 解释须在二进制 decoder 和 C/C++ 定义单一对应；未识别类型、文件长度/计数不符、clock/身份不符、窗内事件缺失均 invalid。
每线程只顺序写自己的预分配 buffer，记录 timestamp 为实际事件点；export 在 writer 全停之后。采集容量满锁存 overflow，停止样本且保留已写数据，不覆盖旧事件。

## 容量预估及边界

历史约 50k QPS × selected16/128 ×25秒 ×每请求12事件 ×32字节 ≈60MB（双端事件的实际 kind 数待 M2 源码落点精确核对）。平均 worker 分布不能作为安全证明：单 server worker 最坏约60MB，高于单线程16MiB。
因此预热前选择按 owner worker 配额分布（上限16，不要求恰好16），保存真实 owner 分布和选择依据；无法控制请求率，任何 worker overflow 都立即 invalid。不能仅凭静态估算声称容量够；不能删关键端点压容量。
server4×16MiB +main4MiB +client2×16MiB =100MiB，小于128MiB总限制。控制结构额外部分必须列 size 与总计，预分配/预触页在 warmup 前完成，统计 A/B 同样分配。

## 请求时间线

规范化 JSON 供 `analyze.py` 读取，但必须经 binary decoder 完整性校验；当前直接 JSON 分析仅 synthetic/M1 离线功能。
所有端点身份字段必填，事件 kind 端点必须匹配；request sequence 正整数且每连接连续，warmup 序号延续。固定 key 包含 run ID/四元组/lifetime/sequence。
首尾 unknown 必须附 decoder 的 recording boundary 证据；窗内缺端点不允许自动降级 unknown。measurement 完整慢请求与 boundary 慢请求分别列出。
`Content-Length/body verified` 必须由真实客户端逐响应检查产生，不是分析器从延迟或 wrk summary 推定。

## 动态 ledger

authoritative limits 来自 `.cache/v0.5.1-s4/leader/authorization.json`；每角色独立 `ledger.json`，全阶段 `dynamic.lock` 串行。
reservation 包括 `run_id/kind/reserved_seconds/start_monotonic/status/output`，结算增加 charged_seconds、错误、role_output_bytes。不明 running 保留全预留额并阻止自动重试；不修改旧 S3 ledger。
runtime maximum 必须包含 owned cleanup 最多7秒；runner 需在此前留出 TERM 回收余量。预算 helper 本身不提供硬定时或身份回收，未接入 runner 之前不可执行真实负载。
2GiB按批准原文计全部动态 run/output/temp/cache/分析结果；固定源码/archive/export/build单列静态 inventory bytes。静态排除文件必须在首次动态前列 size/mtime/hash 并以独立封存 SHA 绑定命令，运行期间不能改 manifest 后自授权排除；未知新文件立即拒绝。原料目录不能写动态产物。
