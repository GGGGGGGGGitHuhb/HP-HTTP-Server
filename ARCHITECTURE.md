# HP HTTP Server 架构文档

本文描述当前实现（含V1.1/S2正文/期限及S3方法路由，均已独立验收），按现有源码静态核对；版本计划见 [ROADMAP](ROADMAP.md)，性能证据见 [benchmark](benchmark/README.md)。2026-10-29已取消本仓库L7代理规划，当前没有proxy模块或upstream转发。

## 模块职责

| 模块 | 当前职责与所有权 | 源码入口 |
| --- | --- | --- |
| app | CLI装配；SignalWatcher持有signalfd；每连接Session拥有parser、响应阶段与请求观测状态 | [main](app/main.cpp)、[HTTP适配](app/HttpConnectionHandler.cpp)、[信号](app/SignalWatcher.cpp)、[访问记录](app/AccessLog.cpp) |
| base | UniqueFd/Socket相关资源基础、FileRegion独占正文fd与偏移；Buffer读写游标；LoggerSession管理AsyncLogger消费者 | [Buffer](src/base/Buffer.cpp)、[FileRegion](include/base/FileRegion.h)、[异步日志](src/base/AsyncLogger.cpp) |
| net | TcpServer装配监听、线程池与registry；EventLoop/Epoller/Channel分离轮询、事件分发和fd所有权；TcpConnection管理单连接状态，ConnectionIo管理字节与文件发送 | [服务器](src/net/TcpServer.cpp)、[注册表](src/net/ConnectionRegistry.cpp)、[连接](src/net/TcpConnection.cpp)、[传输](src/net/ConnectionIo.cpp) |
| http | 增量请求解析、显式ResponseResult、响应序列化、只读方法路由/echo、fd-relative静态文件服务；不管理连接或epoll | [parser](src/http/HttpRequest.cpp)、[response](src/http/HttpResponse.cpp)、[文件服务](src/http/StaticFileService.cpp) |
| timer | owner线程的定时队列；EventLoop控制计时和到期分发 | [TimerQueue](src/timer/TimerQueue.cpp) |
| metrics | 固定容量原子计数与延迟count/sum/max，退出序列化 | [ServerMetrics](src/metrics/ServerMetrics.cpp) |

`include/` 与 `src/` 按模块分离头文件和实现，`app/` 保留现有组织；本项目明确保留布局。文件组织、内部类、类外实现及职责分组的通用规则只维护在个人 `personal-cpp-standards` Skill，不在项目复制第二份规范。

## 依赖方向

`app → http/net/base/metrics`；`http → base/标准库/Linux文件API`；`net → base/timer/metrics`；metrics不依赖HTTP会话。网络层不理解HTTP，HTTP层不拥有连接fd。CMake将base/net/http/timer/metrics编入hp_http_core，将app适配与信号编入hp_http_app，再链接hp_http_server。

## 线程与资源所有权

默认主线程创建main EventLoop与Acceptor，只监听并处理进程信号。Acceptor接收后在交付前启用TCP_NODELAY；失败只关闭该连接。TcpServer按轮转选择worker，将持有Socket与回调的handoff通过EventLoopThread任务队列投递；目标worker才创建TcpConnection并在自己的ConnectionRegistry注册。`--threads 0` 使用main registry处理连接，库TcpServer默认workerCount为0，app默认2。

每个EventLoop始终绑定构造线程；注册、更新、移除、poll与销毁均在owner。Channel只分发事件、不关闭fd；Socket/UniqueFd/SignalWatcher等拥有fd。关闭先移除Channel，registry在安全回收点销毁连接，避免事件回调中释放仍被使用的对象。EventLoopThreadPool拥有workers并join，registry在对应owner清理；跨线程任务排队与eventfd唤醒不授权调用方直接操作目标loop。

HTTP factory绑定文件服务provider；MessageHandler第一次收到输入通知时在连接owner创建共享Session。消息与writeComplete回调持有同一Session，parser和阶段状态不跨连接共享。pending/响应阶段先于send设置；当前HTTP事件内send只排队，消息处理返回后同一次事件分发的flushOutput可同步触发完成回调，不能假设完成总要等下一次可写事件。事件外装有writeCompleteCallback_时send同样只排队。

LoggerSession在服务对象前启动，唯一日志consumer与IO workers分离。服务/worker/registry/Session销毁后停止日志接收、排空并join，最后才导出metrics。StaticFileService活得比捕获它的provider及server更久；FileRegion独占文件fd，正常完成或中断均释放。

## 数据与控制流

1. epoll就绪→Channel→TcpConnection→ConnectionIo非阻塞recv，写入连续Buffer；消息span只在回调期间借用，consume后旧view失效。
2. Session在Reading阶段增量feed parser，只consume本次acceptedBytes；首次非空feed开始请求统计。半包继续等输入，不复制/解析流水线后缀为当前请求。
3. 完整请求→StaticFileService校验路径，以root目录fd逐级打开，拒绝symlink与越界；onResponse返回响应头及独占FileRegion。错误或自定义响应使用内存bytes；显式buildResponseBytes/handleResponse兼容入口会物化文件。
4. Session进入Writing并pause读取，先写定状态；TcpConnection发送内存头，再sendfile正文。EAGAIN保留pending并关注可写，输出逻辑上限按内存+文件remaining计。
5. 全部输出被kernel接收后writeComplete一次终结请求，计completed；保活时reset parser并处理已缓存后缀，一次只生成一个响应。close或draining时直接closeAfterFlush，不推进新请求/provider。
6. 初始空FIN沿用400但无请求计数；开始后未排空即关闭计aborted。registry连接统计包含空连接。完成不证明peer完整收到，服务端延迟不是客户端RTT。

## 错误处理与安全边界

受限HTTP/1.1合法method token在正文完整后交只读RequestRouter。精确POST /api/echo回显拥有型二进制正文；其他路径GET/HEAD进入静态服务。echo路径其他方法405 Allow: POST，静态fallback其他方法405 Allow: GET, HEAD并关闭；未知静态路径GET/HEAD为404，POST仍405且不探测文件。解析错误400关闭。CL缺省为0，唯一十进制CL允许OWS/前导零；重复/列表/符号/溢出、TE/CL共存、重复/畸形TE及重复/空Expect均400。合法数值超1MiB在完整头校验后413，后续非法头优先400，不分配或等待超限正文。Connection token大小写无关合并、close优先；非法token400。普通403/404及服务有界500可复用，provider异常或放宽terminal policy生成500并关闭。

请求头总量16KiB、请求行4KiB、正文1MiB、文件8MiB；不做URL decode，任何percent编码400，歧义路径/反斜杠/symlink403。无Range、压缩、缓存协商、目录列表、TLS、HTTP/2或代理。文件只读，root必须启动前成功打开为目录。

生产输入最多16KiB；库maxInputBytes=0无硬上限。输出上限9MiB，Writing暂停读取，文件未排空不推进pipeline。Buffer consume仅移动游标，尾空间不足才整理/增长；输出空闲容量>64KiB释放，否则保留。此门槛不是响应拒绝或RSS上限。单次增长旧+新暂存、parser/provider与其他资源需另计。

正文解析使用独立kBody状态及渐进std::string存储；acceptedBytes计本次消费，requestBytes计当前头+正文，headerBytes独立执行头限额。NeedMore不复制正文，完成结果拥有body，终态停止于最后正文字节，reset释放body容量。完整返回时parser与结果可短暂各持一份正文，兼容返回值亦有常数倍复制，不宣称全局RSS上限。bufferedBytes/peakBufferedBytes只代表头存储。

## 超时与关闭

owner定时器处理idle/keep-alive；idle只按实际recv/send正字节刷新，keep-alive在响应排空且无下一请求部分输入/后缀时等待。到期静默关闭，可截断输出；两项0禁用，取较早截止。生产额外提供默认10000ms的完整请求读取期限（0禁用）：Session观察当前请求首个非空输入起算，recv/send不续期，完成/拒绝前清除；pipeline后缀在前一响应结束、回到Reading才独立起算。Session读取前及provider前检查过期，超时静默关闭，计一次aborted/unknown，不发408。

Session将可选steady_clock截止通过TcpConnection::setReadDeadline交给Registry；它在idle/keep-alive/readDeadline中取最早，复用一个timeoutId和fd/identity检查。close/drain清除期限并取消timer，Writing不继承读取期限。makeHttpCallback/Factory尾部时长默认0兼容库调用，main显式传CLI值，不向ConnectionTimeouts混入HTTP配置。尚无全局连接/内存配额或业务执行期限，同owner阻塞provider不能被timer抢占。

SIGINT/SIGTERM由SignalWatcher/signalfd在main处理，停止监听并通知owner drain；只排空当前输出，不解析后缀。第一次观察到信号确定绝对截止，再次信号或到期强关。正常受控关闭（含截止截断）退出0，资源/worker错误退出1，CLI错误退出2。

## 日志与观测

AsyncLogger固定1024槽、正文1024字节，队满全等级丢新，无ERROR同步fallback；唯一consumer每批最多64条一次写/flush，sink失败整批计failed不重试。无日志会话的库调用仍同步；停止会话后不自动fallback。阻塞stderr可拖延join，HTTP关闭截止不能保证进程限时退出。

AccessLog默认off，有界owning记录去query/fragment，完整JSON payload≤1024字节，格式/提交失败不改变业务。ServerMetrics逐原子runtime snapshot非事务一致，uint64增量与sum饱和。最终snapshot在服务销毁与logger drain后导出stdout marker；饱和前started=completed+aborted，latency_count=started、active/logger_pending=0。详见 [字段与语义](documentation/RUNNING.md#v06s1-指标与访问记录)。

## 测试架构

V1.1/S3独立Reviewer PASS：Debug/Release各20/20、规定ASan+UBSan/LSan 8/8，新套件错误预期反证有效，73条HTTP进程与10份fixture正常清理。S1–S3版本验收完成，合并发布另行进行；保留历史测试覆盖限制。

V1.1/S2当前增加正文与期限两项测试，Builder自测及Reviewer独立PASS：Debug/Release各18/18、六项sanitizer 6/6。413状态桶附加在旧0/200/400/403/404/405/500之后，输出responses_status_413_total，计parseErrors/errors；日志不记录body。

当前22个注册CTest与历史23个冻结停用目标分别说明于 [开发说明](documentation/DEVELOPMENT.md)。当前接口专项补充生命周期与观测/工具失败覆盖，不恢复全部历史穷举。V1.1/S1新增当前parser接口与真实HTTP套件，覆盖与边界见[HTTP支持矩阵](documentation/HTTP-SUPPORT.md)；2026-10-29独立Reviewer PASS，Debug/Release各16/16及新增两项ASan+UBSan/LSan 2/2通过。历史28/28和12/12仍只代表各自时点；S1当时未迁移全部冻结目标且未改产品；S2在保留覆盖边界基础上增加正文和读取期限，不能沿用S1验收结论。

## 性能材料的系统边界

性能矩阵与analysis是独立工具，不改变服务器运行协议。固定S1 commit1340f5b、Release配置、wrk身份及CPU原始读取区间等条件见 [性能索引](benchmark/README.md)。S3有限三样本仅M2 syscall配对与M6未跟踪CPU；strace默认system time覆盖完整server生命周期，不是measurement独占、wall或函数CPU。M6 syscall未知，M2 -95.85%QPS变化仅观测扰动；不能推导恢复、线性线程扩展、容量或长尾根因。RO-002、TD-001、TD-006保持。

## 后续演进约束（已落实部分见前文，其余仍为规划）

- HTTP层负责消息分帧、方法、目标及响应语义；方法识别不等于资源允许该方法。路由层按方法与规范化路径选择处理器，静态服务是处理器之一，net不理解路由或业务。
- app装配路由、服务和资源预算；业务处理器拥有任务记录等业务状态，不在parser或TcpConnection中存放业务数据。无数据库版本采用有界内存示例，重启清空，不承诺持久化。
- 请求头、正文、chunk元数据、查询参数与响应分别定义边界；跨异步边界使用拥有型数据，不能保留Buffer借用视图。正文消费位置与下一请求起点必须唯一，保持每连接顺序响应。
- 耗时业务通过独立有界执行机制完成；网络连接和响应提交仍由owner管理。显式管理请求身份、结果、截止与取消状态，拒绝迟到结果；超时不等于任意函数可被抢占或副作用已回滚。
- 全局资源准入与owner局部资源释放有单一计数责任，释放恰好一次；日志、指标不改变协议结果。V1.4整合容量预算不免除前序阶段引入资源时的边界要求。
- 文件路径解码、路由匹配与静态根访问保持一致的安全语义；现有fd-relative访问和符号链接约束不因增加URL支持而放宽。
- 延续include/src与app布局；类、回调、状态结果及生命周期设计引用personal-cpp-standards，不提前冻结具体接口或引入通用框架。
- Server独立运行，不依赖AI网关、L4LB、数据库或私有学习资料。协议版本及TLS等扩展需另立范围。

变更记录：2026-10-29依据用户服务器完善方向补充以上规划约束；前文当前实现与限制不变，阶段接口设计尚待批准。

## 架构变更流程

新架构或接口变更先形成Approved设计，实现/审查证据分角色记录，允许关闭后更新当前文档。旧架构时点完整保存在 [迁移归档](history/documentation/V1.0-S1/INDEX.md)，历史角色报告和性能raw保持原字节。

<details>
<summary>历史锚点导航</summary>

旧标题对应迁移前历史时点，不作为当前能力或执行授权。

<a id="当前状态与目标架构"></a>
- [当前状态与目标架构](history/documentation/V1.0-S1/ARCHITECTURE.md#当前状态与目标架构)
<a id="项目技术概览"></a>
- [项目技术概览](history/documentation/V1.0-S1/ARCHITECTURE.md#项目技术概览)
<a id="系统分层"></a>
- [系统分层](history/documentation/V1.0-S1/ARCHITECTURE.md#系统分层)
<a id="启动入口层"></a>
- [启动入口层](history/documentation/V1.0-S1/ARCHITECTURE.md#启动入口层)
<a id="网络事件层"></a>
- [网络事件层](history/documentation/V1.0-S1/ARCHITECTURE.md#网络事件层)
<a id="协议与应用层"></a>
- [协议与应用层](history/documentation/V1.0-S1/ARCHITECTURE.md#协议与应用层)
<a id="基础设施层"></a>
- [基础设施层](history/documentation/V1.0-S1/ARCHITECTURE.md#基础设施层)
<a id="测试与验证层"></a>
- [测试与验证层](history/documentation/V1.0-S1/ARCHITECTURE.md#测试与验证层)
<a id="文档与协作层"></a>
- [文档与协作层](history/documentation/V1.0-S1/ARCHITECTURE.md#文档与协作层)
<a id="app"></a>
- [`app/`](history/documentation/V1.0-S1/ARCHITECTURE.md#app)
<a id="includebase-与-srcbase"></a>
- [`include/base/` 与 `src/base/`](history/documentation/V1.0-S1/ARCHITECTURE.md#includebase-与-srcbase)
<a id="includenet-与-srcnet"></a>
- [`include/net/` 与 `src/net/`](history/documentation/V1.0-S1/ARCHITECTURE.md#includenet-与-srcnet)
<a id="includehttp-与-srchttp"></a>
- [`include/http/` 与 `src/http/`](history/documentation/V1.0-S1/ARCHITECTURE.md#includehttp-与-srchttp)
<a id="includeproxy-与-srcproxy"></a>
- [`include/proxy/` 与 `src/proxy/`](history/documentation/V1.0-S1/ARCHITECTURE.md#includeproxy-与-srcproxy)
<a id="includetimer-与-srctimer"></a>
- [`include/timer/` 与 `src/timer/`](history/documentation/V1.0-S1/ARCHITECTURE.md#includetimer-与-srctimer)
<a id="includemetrics-与-srcmetrics"></a>
- [`include/metrics/` 与 `src/metrics/`](history/documentation/V1.0-S1/ARCHITECTURE.md#includemetrics-与-srcmetrics)
<a id="tests"></a>
- [`tests/`](history/documentation/V1.0-S1/ARCHITECTURE.md#tests)
<a id="benchmark"></a>
- [`benchmark/`](history/documentation/V1.0-S1/ARCHITECTURE.md#benchmark)
<a id="静态文件请求流"></a>
- [静态文件请求流](history/documentation/V1.0-S1/ARCHITECTURE.md#静态文件请求流)
<a id="l7-代理请求流"></a>
- [L7 代理请求流](history/documentation/V1.0-S1/ARCHITECTURE.md#l7-代理请求流)
<a id="错误传播"></a>
- [错误传播](history/documentation/V1.0-S1/ARCHITECTURE.md#错误传播)
<a id="数据模型与持久化"></a>
- [数据模型与持久化](history/documentation/V1.0-S1/ARCHITECTURE.md#数据模型与持久化)
<a id="外部接口与集成"></a>
- [外部接口与集成](history/documentation/V1.0-S1/ARCHITECTURE.md#外部接口与集成)
<a id="命令行接口"></a>
- [命令行接口](history/documentation/V1.0-S1/ARCHITECTURE.md#命令行接口)
<a id="http-接口"></a>
- [HTTP 接口](history/documentation/V1.0-S1/ARCHITECTURE.md#http-接口)
<a id="文件输入输出"></a>
- [文件输入输出](history/documentation/V1.0-S1/ARCHITECTURE.md#文件输入输出)
<a id="系统命令和第三方服务"></a>
- [系统命令和第三方服务](history/documentation/V1.0-S1/ARCHITECTURE.md#系统命令和第三方服务)
<a id="并发状态与资源管理"></a>
- [并发、状态与资源管理](history/documentation/V1.0-S1/ARCHITECTURE.md#并发状态与资源管理)
<a id="架构约束"></a>
- [架构约束](history/documentation/V1.0-S1/ARCHITECTURE.md#架构约束)
<a id="变更记录"></a>
- [变更记录](history/documentation/V1.0-S1/ARCHITECTURE.md#变更记录)
<a id="v05s1-文件传输已交付边界"></a>
- [V0.5/S1 文件传输已交付边界](history/documentation/V1.0-S1/ARCHITECTURE.md#v05s1-文件传输已交付边界)
<a id="独立矩阵测量边界"></a>
- [独立矩阵测量边界](history/documentation/V1.0-S1/ARCHITECTURE.md#独立矩阵测量边界)

</details>

## V1.1/S3 方法路由（已独立验收）

main在启动前构造StaticFileService和RequestRouter，factory保存router引用；服务与router寿命覆盖worker停止。兼容makeHttpFactory(service)拥有router并让每连接回调保留共享所有权，service仍由调用者保证寿命。Session在完整解析并清读取deadline后同步调用ResponseCallback→RequestRouter::onResponse→echoResponse/StaticFileService::onResponse，返回拥有型bytes或独占FileRegion，再由owner发送；WriteComplete重置解析器后才推进后缀。net层没有路由规则。

parser仅在完整合法请求行后记录精确HEAD；错误头/正文/FIN仍可生成无正文400/413，未验证行不猜测HEAD。HEAD静态成功只取fd元数据并RAII关闭，不读取整个文件或提交FileRegion；成功和错误均保留表示Content-Length，contentBytes为0。Session catch的HEAD500也无正文。GET静态忽略请求body，echo复制原始body，不保留借用Buffer视图；常数倍临时正文/响应不代表全局内存配额。同步处理器不能被读取deadline抢占。

路由只切离第一个问号后的query，不解码；target任何位置percent均400且优先于方法判断。同名静态/api/echo不覆盖动态精确路由，/api/echo/仍走原静态校验（当前403），没有隐式/api命名空间。405计errors而不计parseErrors；分帧400/413仍计parseErrors。无运行期路由注册/JSON/数据库/线程池/网关。

## V1.2/S1 chunked与临时响应（已独立验收）

Reviewer独立PASS：最终候选Debug/Release各22/22、十项ASan+UBSan/LSan10/10；三条真实EAGAIN/pending25和30条约300ms滴流证据有效，97条HTTP与13份fixture正常清理。仅S1完成，S2/S3未开始；不关闭历史覆盖/性能风险。

RequestParser新增chunk-size/data/data-CRLF/trailer状态，仅数据进入拥有型body；wire计数含元数据，终止空行后停止。1MiB单块/解码总量、1KiB size行、64KiB总元数据与8KiB trailer分别限制，不预分配声明大小。扩展按token/quoted-string/BWS解析后丢弃，trailer只允许X-Checksum与X-Trace-Id且不改变请求语义；RequestHead拥有method/target/closeRequested，不保留Buffer借用。

Expect正文请求在完整头边界一次性返回kHeadersComplete，即使同送body也先停。Session检查原绝对期限，再经RequestHeadersCallback→RequestRouter::onRequestHeaders纯头准入；该入口复用最终路由规则，禁止调用业务或提前打开静态文件。拒绝输出唯一final；允许则先进入kInterimWriting再排队固定100。writeComplete在同owner同步执行，interim分支不reset parser、不清deadline、不completed或记日志，恢复Reading处理缓存body；最终Writing才沿旧流程终结。两回调捕获同一长寿命/ownedRouter，setter仅保存，net/static无需改动。

100排队/EAGAIN期间读取期限仍从首字节计，100不续期；drain只排空100后关闭并将请求一次aborted/unknown，不生成final。417/501附加到原413之后的状态桶且计parseErrors/errors，100无桶。纯头早405与未读非法body的优先级只适用于有效Expect；没有Expect时必须完整校验正文再路由。无响应chunked、通用trailer、解压、URL解码或业务范围扩展。
