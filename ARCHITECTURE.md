# HP HTTP Server 架构文档

本文描述当前V0.6交付后的实现，按现有源码静态核对；版本计划见 [ROADMAP](ROADMAP.md)，性能证据见 [benchmark](benchmark/README.md)。轻量L7代理仅为V1.1可选规划，当前没有proxy模块或upstream转发。

## 模块职责

| 模块 | 当前职责与所有权 | 源码入口 |
| --- | --- | --- |
| app | CLI装配；SignalWatcher持有signalfd；每连接Session拥有parser、响应阶段与请求观测状态 | [main](app/main.cpp)、[HTTP适配](app/HttpConnectionHandler.cpp)、[信号](app/SignalWatcher.cpp)、[访问记录](app/AccessLog.cpp) |
| base | UniqueFd/Socket相关资源基础、FileRegion独占正文fd与偏移；Buffer读写游标；LoggerSession管理AsyncLogger消费者 | [Buffer](src/base/Buffer.cpp)、[FileRegion](include/base/FileRegion.h)、[异步日志](src/base/AsyncLogger.cpp) |
| net | TcpServer装配监听、线程池与registry；EventLoop/Epoller/Channel分离轮询、事件分发和fd所有权；TcpConnection管理单连接状态，ConnectionIo管理字节与文件发送 | [服务器](src/net/TcpServer.cpp)、[注册表](src/net/ConnectionRegistry.cpp)、[连接](src/net/TcpConnection.cpp)、[传输](src/net/ConnectionIo.cpp) |
| http | 增量请求解析、显式ResponseResult、响应序列化、fd-relative静态文件服务；不管理连接或epoll | [parser](src/http/HttpRequest.cpp)、[response](src/http/HttpResponse.cpp)、[文件服务](src/http/StaticFileService.cpp) |
| timer | owner线程的定时队列；EventLoop控制计时和到期分发 | [TimerQueue](src/timer/TimerQueue.cpp) |
| metrics | 固定容量原子计数与延迟count/sum/max，退出序列化 | [ServerMetrics](src/metrics/ServerMetrics.cpp) |

`include/` 与 `src/` 按模块分离头文件和实现，`app/` 保留现有组织；本项目明确保留布局。文件组织、内部类、类外实现及职责分组的通用规则只维护在个人 `personal-cpp-standards` Skill，不在项目复制第二份规范。

## 依赖方向

`app → http/net/base/metrics`；`http → base/标准库/Linux文件API`；`net → base/timer/metrics`；metrics不依赖HTTP会话。网络层不理解HTTP，HTTP层不拥有连接fd。CMake将base/net/http/timer/metrics编入hp_http_core，将app适配与信号编入hp_http_app，再链接hp_http_server。

## 线程与资源所有权

默认主线程创建main EventLoop与Acceptor，只监听并处理进程信号。Acceptor接收后在交付前启用TCP_NODELAY；失败只关闭该连接。TcpServer按轮转选择worker，将持有Socket与回调的handoff通过EventLoopThread任务队列投递；目标worker才创建TcpConnection并在自己的ConnectionRegistry注册。`--threads 0` 使用main registry处理连接，库TcpServer默认workerCount为0，app默认2。

每个EventLoop始终绑定构造线程；注册、更新、移除、poll与销毁均在owner。Channel只分发事件、不关闭fd；Socket/UniqueFd/SignalWatcher等拥有fd。关闭先移除Channel，registry在安全回收点销毁连接，避免事件回调中释放仍被使用的对象。EventLoopThreadPool拥有workers并join，registry在对应owner清理；跨线程任务排队与eventfd唤醒不授权调用方直接操作目标loop。

HTTP factory绑定文件服务provider；MessageHandler第一次收到输入通知时在连接owner创建共享Session。消息与writeComplete回调持有同一Session，parser和阶段状态不跨连接共享。send可同步触发完成回调，因此pending/响应阶段先于send设置；不能假设完成总是延迟发生。

LoggerSession在服务对象前启动，唯一日志consumer与IO workers分离。服务/worker/registry/Session销毁后停止日志接收、排空并join，最后才导出metrics。StaticFileService活得比捕获它的provider及server更久；FileRegion独占文件fd，正常完成或中断均释放。

## 数据与控制流

1. epoll就绪→Channel→TcpConnection→ConnectionIo非阻塞recv，写入连续Buffer；消息span只在回调期间借用，consume后旧view失效。
2. Session在Reading阶段增量feed parser，只consume本次acceptedBytes；首次非空feed开始请求统计。半包继续等输入，不复制/解析流水线后缀为当前请求。
3. 完整请求→StaticFileService校验路径，以root目录fd逐级打开，拒绝symlink与越界；onResponse返回响应头及独占FileRegion。错误或自定义响应使用内存bytes；显式buildResponseBytes/handleResponse兼容入口会物化文件。
4. Session进入Writing并pause读取，先写定状态；TcpConnection发送内存头，再sendfile正文。EAGAIN保留pending并关注可写，输出逻辑上限按内存+文件remaining计。
5. 全部输出被kernel接收后writeComplete一次终结请求，计completed；保活时reset parser并处理已缓存后缀，一次只生成一个响应。close或draining时直接closeAfterFlush，不推进新请求/provider。
6. 初始空FIN沿用400但无请求计数；开始后未排空即关闭计aborted。registry连接统计包含空连接。完成不证明peer完整收到，服务端延迟不是客户端RTT。

## 错误处理与安全边界

仅受限HTTP/1.1无body GET；合法非GET为405+Allow并关闭，解析错误400关闭。仅允许无Content-Length或唯一十进制零值，重复/列表/非零CL、Transfer-Encoding、Expect均400关闭。Connection token大小写无关合并、close优先；非法token400。普通403/404及服务有界500可复用，provider异常或放宽terminal policy生成500并关闭。

请求累计16KiB、请求行4KiB、文件8MiB；不做URL decode，任何percent编码400，歧义路径/反斜杠/symlink403。无Range、压缩、缓存协商、目录列表、TLS、HTTP/2或代理。文件只读，root必须启动前成功打开为目录。

生产输入最多16KiB；库maxInputBytes=0无硬上限。输出上限9MiB，Writing暂停读取，文件未排空不推进pipeline。Buffer consume仅移动游标，尾空间不足才整理/增长；输出空闲容量>64KiB释放，否则保留。此门槛不是响应拒绝或RSS上限。单次增长旧+新暂存、parser/provider与其他资源需另计。

## 超时与关闭

owner定时器处理idle/keep-alive；idle只按实际recv/send正字节刷新，keep-alive在响应排空且无下一请求部分输入/后缀时等待。到期静默关闭，可截断输出；两项0禁用，取较早截止。尚无全局连接/内存配额或总请求时限，持续少量输入可延长idle，同owner阻塞provider不能被timer抢占。

SIGINT/SIGTERM由SignalWatcher/signalfd在main处理，停止监听并通知owner drain；只排空当前输出，不解析后缀。第一次观察到信号确定绝对截止，再次信号或到期强关。正常受控关闭（含截止截断）退出0，资源/worker错误退出1，CLI错误退出2。

## 日志与观测

AsyncLogger固定1024槽、正文1024字节，队满全等级丢新，无ERROR同步fallback；唯一consumer每批最多64条一次写/flush，sink失败整批计failed不重试。无日志会话的库调用仍同步；停止会话后不自动fallback。阻塞stderr可拖延join，HTTP关闭截止不能保证进程限时退出。

AccessLog默认off，有界owning记录去query/fragment，完整JSON payload≤1024字节，格式/提交失败不改变业务。ServerMetrics逐原子runtime snapshot非事务一致，uint64增量与sum饱和。最终snapshot在服务销毁与logger drain后导出stdout marker；饱和前started=completed+aborted，latency_count=started、active/logger_pending=0。详见 [字段与语义](documentation/RUNNING.md#v06s1-指标与访问记录)。

## 测试架构

当前14个注册CTest与历史23个冻结停用目标分别说明于 [开发说明](documentation/DEVELOPMENT.md)。当前接口专项补充生命周期与观测/工具失败覆盖，不恢复全部历史穷举。历史28/28和S1的12/12不代表本轮14项已执行；本阶段没有动态回归。

## 性能材料的系统边界

性能矩阵与analysis是独立工具，不改变服务器运行协议。固定S1 commit1340f5b、Release配置、wrk身份及CPU原始读取区间等条件见 [性能索引](benchmark/README.md)。S3有限三样本仅M2 syscall配对与M6未跟踪CPU；strace默认system time覆盖完整server生命周期，不是measurement独占、wall或函数CPU。M6 syscall未知，M2 -95.85%QPS变化仅观测扰动；不能推导恢复、线性线程扩展、容量或长尾根因。RO-002、TD-001、TD-006保持。

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
