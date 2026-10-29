# 运行配置与观测

从仓库根目录运行；通用构建入口见 [README](../README.md#快速开始)。V1.1/S2新增正文与读取期限已由Builder自测并经Reviewer独立PASS；S3方法路由已独立验收PASS；旧版本验证记录不倒写。

## 配置说明

当前没有配置文件系统；CLI 必须各提供一次 `--port <0-65535>` 和 `--root <directory>`，二者顺序可交换。

- `--threads <0-64>`：可选，默认2个worker；0为单Reactor，主线程不计入worker数。严格十进制，重复、缺值、符号、非数字、越界退出2；资源启动错误退出1。
- `--shutdown-timeout-ms <0-60000>`：默认5000。SIGINT/SIGTERM触发停止监听和当前输出排空，0立即关闭；再次观察到信号强关，不延长第一次观察时确定的steady_clock绝对截止。参数重复、缺值、符号、非数字或越界退出2；正常信号退出（含期限截断）退出0，资源/worker异常退出1。
- `--idle-timeout-ms <0-86400000>`：默认30000。从 owner 注册连接开始，只按实际 recv/send 正数字节刷新；EAGAIN、伪事件和只入缓冲均不刷新。
- `--keep-alive-timeout-ms <0-86400000>`：默认15000。一个响应实际排空、无缓存后缀且 parser 无部分下一请求时开始等待；新输入退出等待，重复等待通知不延长截止。
- `--request-read-timeout-ms <0-86400000>`：默认10000，0禁用。Session首次观察当前请求非空输入起绝对计时，后续进度不刷新；头+正文完整或拒绝后清除。pipeline后缀在前一响应排空后进入Reading时独立起算，不计前一响应Writing时间。
- 三项0分别禁用对应策略，全部为0时没有连接/读取期限；同时适用取较早截止。严格无符号十进制；符号、空值、重复、缺值、溢出/越界退出2。到期静默关闭，不发送408，未排空响应可能截断。C++ `TcpServer` 末尾 `ConnectionTimeouts` 默认0/0，app明确传入上述CLI默认值。

- `--port 8080 --root ./www`：监听显式端口并从 `./www` 只读提供文件。
- `--port 0 --root ./www`：由内核分配临时端口，启动输出报告实际非零端口。
- `--root` 必须在监听前成功打开为目录；缺失、非目录或不可打开时进程非零退出。
- 未知、重复、缺值、非法端口和将 `--help` 与其他参数混用都会受控失败。
- 普通错误不会回显 root 的绝对路径。

每个参数只有在实现、测试和 README 命令同时成立时，才视为可用接口。

### 固定长度正文

请求头（含请求行与CRLF）最多16KiB，行内容最多4KiB；正文独立最多1MiB。CL缺省为0，单个十进制Content-Length支持前导零/外围OWS；重复、列表、正负号、非数字、uint64溢出及TE/CL共存均400并关闭。合法值超1MiB在完整头验证后413 Content Too Large，不读取声明正文；后续非法头仍优先400。413/400/405不处理pipeline后缀。

GET正文完整消费但不解释，仍按路径返回静态文件；HEAD返回对应GET的状态/类型/表示长度但完全不发送正文；POST /api/echo回显原始二进制正文。echo路径其他方法405 Allow: POST，其他路径非GET/HEAD为405 Allow: GET, HEAD，均关闭。正文不足而FIN为400；到读取截止则静默关闭，不发408。生产输入窗口仍16KiB，边消费边读，可接收超过窗口的有界正文；未完成结果不复制累计body，完成后返回拥有型body，reset释放正文容量。该逻辑上限不是全局内存/RSS配额。

例如固定长度GET正文仍得到首页：

```bash
curl --http1.1 -X GET --data-binary 'abc' http://127.0.0.1:8080/
```

时间边界只约束当前请求的应用层读取窗口，不是业务执行期限或网卡首字节起算；阻塞owner的provider不能被timer抢占，HTTP关闭期限也不保证整个进程退出上限。

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

请求从首次非空parser feed计起；初始空FIN仍保留旧400协议，但不产生请求计数或访问记录。completed表示内存/sendfile输出已被kernel接收，不证明peer完整接收；aborted表示开始后尚未排空即关闭。`responses_status_<200/400/403/404/405/500/413/417/501/unknown>_total` 是已构造响应的状态（也含aborted），未构造响应用unknown；`content_bytes` 是实际计划发送的正文长度，HEAD为0但响应Content-Length仍为表示长度。405不计parseErrors，分帧400/413仍计；不记录body/query。`errors_total` 每请求至多一次，表示aborted或status>=400；parse/provider原因分项可能重叠，不能相加。steady_clock整数微秒延迟包含该请求解析/provider/输出排队到终结，completed和aborted都入count/sum/max，不是RTT/P99。runtime逐原子快照非事务一致；uint64增量与sum饱和，超出范围不回绕，饱和后总账等式不再保证精确数量。

最终 `logger_submitted/accepted/dropped_full/rejected_stopped/truncated/written/failed/pending` 属于全部共享日志；它们不是精确访问记录丢失数。`access_log_failures_total` 只记录格式化/提交异常；默认关闭无访问记录格式化或提交。HTTP关闭截止不限制阻塞stderr造成的日志join等待，沿用原日志限制。

V1.1/S2的413新增尾部状态桶，不移动旧索引；413计parse_errors_total和errors_total。读取超时只终结一次aborted，未形成响应时状态为unknown，body不进入访问日志。

### 方法路由与回显（V1.1/S3已验收）

```bash
curl --http1.1 -I http://127.0.0.1:8080/
curl --http1.1 --data-binary 'abc' http://127.0.0.1:8080/api/echo
```

POST回显不依赖Content-Type，不添加换行；无CL或CL0为零字节。`/api/echo?tag=plain`匹配同一路由，query经有界解析但业务不使用且不记录；合法percent query可接受。只精确匹配/api/echo，尾斜杠由公共目标结构校验对全部方法403关闭；GET未知文件404而POST未知路径405，不探测静态文件存在性。HEAD的错误400/403/404/405/413/受控500也不发正文，只有完整合法HEAD请求行可信时才适用于解析错误。所有终止响应丢弃pipeline后缀，provider异常/放宽close策略500关闭。尚无JSON、Unicode规范化、响应chunked、数据库或运行期路由注册。

### V1.2/S1 chunked与Expect已验收

```bash
curl --http1.1 -H 'Transfer-Encoding: chunked' -H 'Expect: 100-continue' --data-binary 'abc' http://127.0.0.1:8080/api/echo
```

只接受唯一Transfer-Encoding字段的单个chunked（大小写不敏感、外围OWS）；TE/CL共存、重复TE、chunked非末尾/重复、空项/非法token/参数400。合法gzip, chunked为501，不解压；仅gzip最终未定界仍400。Expect唯一且精确100-continue（忽略大小写/外围OWS）支持，重复/空400，其他非空417。先校验全部头，400优先于超CL413、编码501、期望417，再进行路由头部准入。

解码正文和单块各≤1MiB；每size行含CRLF≤1024字节，全部chunk元数据≤65536字节（size行/data后CRLF/trailer/终止空行），trailer区≤8192字节。超数据限额413，语法/元数据超限400，关闭且丢弃后缀。合法未知扩展解析后忽略。可选唯一Trailer头只允许不重复的X-Checksum、X-Trace-Id名称列表；实际trailer允许这两字段重复或未声明，值丢弃且不校验checksum；其他字段400，绝不覆盖Host/Connection等语义。

只有有效Expect且CL>0或chunked才发一次`HTTP/1.1 100 Continue`空响应，不带CL/TE/body；CL0或无正文直接final。完整头后目标400/403、方法405可以早拒绝，不等body；静态文件存在性仍在完整请求后检查。故Expect GET /api/echo早405，无Expect同请求的非法chunk先400。coalesced头/body也允许先100，final及下一请求严格有序。可信HEAD的417/501等错误仍零正文且保留表示长度。

100排队和剩余body/trailer共用原绝对读取期限，不重新计时；0禁用不等于无限资源保障。100不增加响应/状态桶/access计数；后续超时/RST/drain可没有final，只记一次aborted/unknown、正文0。只有最终响应记表示的实际正文，417/501计parseErrors，早405不计。仍不记录body/扩展/trailer/query，不新增CLI或全局内存配额。

### V1.2/S2 请求目标与参数已验收

```bash
curl --http1.1 -i 'http://127.0.0.1:8080/space%20name.txt?q=%20'
curl --http1.1 --data-binary 'abc' 'http://127.0.0.1:8080/%61pi/echo?tag=one&tag=two'
```

第一例需root中存在`space name.txt`；第二例与/api/echo选同一业务。先按原始?分path/query再一次解码，strict hex允许大小写；残缺/非法编码、解码NUL/control为400。path编码得到/、反斜杠、?、#、%均400，因此双重编码路径拒绝；普通编码空格/高字节按Linux字节文件名访问。原始fragment、空白/控制禁止，原始或解码的点段/空内部段/重复slash/非根尾slash/反斜杠为403，不折叠。

query按原始&、每段首个=切分；flag/a=可空值，空key/空段拒绝，分号和value余下=是普通数据。+保留plus，%20为空格；%26/%3D/%3F不再拆分，%2526只得到%26，query的slash/反斜杠/编码#只为数据。保留重复key全部值与输入顺序，不last-wins，不转数字/布尔，不再解码。echo只回显body，参数不会拼入响应或路径。

raw target≤4096字节（生产仍受整请求行4KiB约束），raw query≤2048、最多64对、单decoded key≤128/value≤1024、decoded总量≤2048。超限400；完整目标语法/限额错误400优先结构403。目标验证早于方法，所有目标400/403 close并拒绝pipeline后缀；合法目标后的文件权限/symlink403或404仍沿原保活策略。因此POST /api/echo/现在403，合法POST /missing仍405；GET /missing仍404。

有效Expect在目标合法时100后继续CL/chunked；目标错误早final不发100，HEAD保持零正文和正确表示CL。无Expect仍先正文分帧，目标错误不增加parseErrors/providerErrors；日志仍只raw path去query，不打印参数。无新CLI、URL表单规则、Unicode规范化、Range/缓存、数据库或业务端点。


## V1.2/S3 条件请求与单区间（已独立验收）

无需新CLI。以现有静态文件`/note.txt`为例：

```bash
curl -i http://127.0.0.1:8080/note.txt
curl -i -H 'If-None-Match: *' http://127.0.0.1:8080/note.txt
curl -i -H 'Range: bytes=1-3' http://127.0.0.1:8080/note.txt
curl -I -H 'Range: bytes=1-3' http://127.0.0.1:8080/note.txt
```

首个响应含弱ETag/Last-Modified/Date/Accept-Ranges；第二个对存在文件返回304且无Content-Length/body；第三个返回206和实际单区间，文件不足则按单区间规则截断或416；HEAD仍为全表示200头无body。`bytes=-N`表示尾N字节，`bytes=N-`为开尾；suffix0/空文件/起点越界416并可保活；多区间、重复/畸形/溢出Range忽略回200。

If-None-Match支持弱列表和单独*，最多128条；存在但无效也抑制If-Modified-Since。唯一合法IMS支持三HTTP-date格式。任何If-Range（包括匹配标签、伪强标签、日期或空值）都保守回完整200，条件304优先；这不是安全断点拼接。echo忽略这些条件头，文件错误优先，Expect仍先100和完整body。没有文件内容缓存或快照，不改变8MiB限制。新metrics在原桶末尾增加responses_status_206_total/304_total/416_total，416不是parse/provider错误。
