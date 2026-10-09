# 运行配置与观测

从仓库根目录运行；通用构建入口见 [README](../README.md#快速开始)。本页按当前源码静态核对，V1.0/S1未重跑动态命令。

## 配置说明

当前没有配置文件系统；CLI 必须各提供一次 `--port <0-65535>` 和 `--root <directory>`，二者顺序可交换。

- `--threads <0-64>`：可选，默认2个worker；0为单Reactor，主线程不计入worker数。严格十进制，重复、缺值、符号、非数字、越界退出2；资源启动错误退出1。
- `--shutdown-timeout-ms <0-60000>`：默认5000。SIGINT/SIGTERM触发停止监听和当前输出排空，0立即关闭；再次观察到信号强关，不延长第一次观察时确定的steady_clock绝对截止。参数重复、缺值、符号、非数字或越界退出2；正常信号退出（含期限截断）退出0，资源/worker异常退出1。
- `--idle-timeout-ms <0-86400000>`：默认30000。从 owner 注册连接开始，只按实际 recv/send 正数字节刷新；EAGAIN、伪事件和只入缓冲均不刷新。
- `--keep-alive-timeout-ms <0-86400000>`：默认15000。一个响应实际排空、无缓存后缀且 parser 无部分下一请求时开始等待；新输入退出等待，重复等待通知不延长截止。
- 两项0分别禁用对应策略，两项都0恢复无超时；同时适用取较早截止。严格无符号十进制；符号、空值、重复、缺值、溢出/越界退出2。到期静默关闭，不发送408，未排空响应可能截断。C++ `TcpServer` 末尾 `ConnectionTimeouts` 默认0/0，app明确传入上述CLI默认值。

- `--port 8080 --root ./www`：监听显式端口并从 `./www` 只读提供文件。
- `--port 0 --root ./www`：由内核分配临时端口，启动输出报告实际非零端口。
- `--root` 必须在监听前成功打开为目录；缺失、非目录或不可打开时进程非零退出。
- 未知、重复、缺值、非法端口和将 `--help` 与其他参数混用都会受控失败。
- 普通错误不会回显 root 的绝对路径。

每个参数只有在实现、测试和 README 命令同时成立时，才视为可用接口。

### 异步日志

生产启动后保留 `info/warn/error` 和 `[LEVEL] message` stderr格式，当前实现（原R005修复候选）由唯一后台消费者每批最多64条，拼接后一次写出并flush，不等待凑批；批内日志在批次完成时计成功，可见时间可能晚于旧版逐条flush。固定1024个槽位、正文最多1024字节，超长在上限内追加 `...[truncated]`；消息复制后提交，所有等级队列满时丢新，无ERROR同步回退。统计快照提供提交、接受、丢弃、停止拒绝、截断、成功、失败及含在途记录的pending。队列外最多64条在途记录，pending上界1088；批量临时存储固定132672字节（当前ABI），sink写入或flush失败时整批计failed，不重试。R005完整性能矩阵P3未达标，独立Reviewer009为FAIL，不代表S3已完成。

LoggerSession先于服务对象启动，server/worker/callback销毁后停止接收、排空并join。可返回的sink错误计failed，不递归记录、不无限重试。健康stderr保证排空；**阻塞stderr可能拖延最终日志join，HTTP `--shutdown-timeout-ms` 不保证整个进程限时退出**。不修改共享stderr标志，也不强制取消/分离线程。启动失败在监听前退出非零；CLI帮助、参数错误及stdout就绪行通道不变。没有会话的旧库调用仍同步；生产会话停止后不会自动恢复同步。

### Buffer与既有背压

ConnectionIo使用单owner连续Buffer维护读写游标：consume不搬移后缀，仅尾空间不足时整理或增长；recv直接写入懒分配的持有尾区，取消栈中转复制。生产未消费输入仍最多16KiB；通用库显式/默认maxInputBytes=0仍无输入硬上限。临时view在consume或下一次修改后失效。

输出仍按“内存可读字节+文件remaining”执行9MiB逻辑上限。小file header复用已有空Buffer；只有内存及文件都排空时，**容量>64KiB的输出释放，≤64KiB保留**。64KiB是空闲保留门槛，不是响应拒绝或读暂停阈值；重复大内存响应会重新分配。单次增长临时旧+新存储上界为生产输入32KiB/内存输出18MiB，另计调用方响应、parser及其他资源，不是进程RSS上限。

Writing暂停读取、一次只生成一个响应、文件未排空时不推进pipeline、idle/drain截止都是原有背压规则，当前保持。机制测试证明减少搬移/分配及空闲大容量保留，不宣称吞吐提升；该机制交付时S4压测尚未实施；后续证据见 [性能索引](../benchmark/README.md)，不能据此单独归因性能。

## V0.6/S1 指标与访问记录

当前实现提供基础统计、可选访问记录和退出快照，S1已独立验收，不包含性能验收或长尾修复。启动方式沿用上述二进制，可追加两个无值开关；重复开关或给开关传值返回2：

```bash
./build/hp_http_server --port 8080 --root www --threads 2 --access-log --metrics-on-exit
```

`--access-log` 默认关闭。启用时通过原有异步日志向stderr写入 `[INFO] ` 前缀的完整JSON payload，`event=http_access`；其他启动/诊断日志仍是文本。记录仅含method、path、status、content_bytes、duration_us、outcome、path_truncated。method最多16原始字节，path最多96原始字节，去除query/fragment，不记录headers/body/peer；非ASCII逐字节以 `\u00XX` 转义，不进行URL decode。path可能包含业务标识，请按需求选择开启。每条payload最多1024字节，极长path明确标记截断，JSON保持完整；日志队满、sink写失败或观测异常不改变HTTP响应，不增加同步fallback。

`--metrics-on-exit` 默认关闭。正常或受控关闭时，在所有worker join、registry/Session销毁、异步日志排空后，stdout的唯一 `HP_METRICS_BEGIN`/`HP_METRICS_END` 段导出文本 `key integer`。没有自动写文件或监控HTTP路由；stdout写失败返回非零。CLI参数错误不导出。关闭后 `requests_started_total=responses_completed_total+requests_aborted_total`、`latency_count=requests_started_total`、`connections_active=logger_pending=0`。

请求从首次非空parser feed计起；初始空FIN仍保留旧400协议，但不产生请求计数或访问记录。completed表示内存/sendfile输出已被kernel接收，不证明peer完整接收；aborted表示开始后尚未排空即关闭。`responses_status_<200/400/403/404/405/500/unknown>_total` 是已构造响应的状态（也含aborted），未构造响应用unknown；`content_bytes` 是计划正文长度。`errors_total` 每请求至多一次，表示aborted或status>=400；parse/provider原因分项可能重叠，不能相加。steady_clock整数微秒延迟包含该请求解析/provider/输出排队到终结，completed和aborted都入count/sum/max，不是RTT/P99。runtime逐原子快照非事务一致；uint64增量与sum饱和，超出范围不回绕，饱和后总账等式不再保证精确数量。

最终 `logger_submitted/accepted/dropped_full/rejected_stopped/truncated/written/failed/pending` 属于全部共享日志；它们不是精确访问记录丢失数。`access_log_failures_total` 只记录格式化/提交异常；默认关闭无访问记录格式化或提交。HTTP关闭截止不限制阻塞stderr造成的日志join等待，沿用原日志限制。
