# 面试讲解：执行流、状态与取舍

[项目入口](../README.md) · [架构](../ARCHITECTURE.md) · [项目展示](PRESENTATION.md) · [性能摘要](PERFORMANCE.md)

以当前已提交实现为机制依据，按完整场景解释；源码链接用于定位职责边界。历史测量见性能摘要，材料本身不表示学习者已通过测评。

## 接受连接到worker接管

入口：[Acceptor](../src/net/Acceptor.cpp)、[TcpServer](../src/net/TcpServer.cpp)、[EventLoopThread](../src/net/EventLoopThread.cpp)、[EventLoop](../src/net/EventLoop.cpp)、[ConnectionRegistry](../src/net/ConnectionRegistry.cpp)。

1. main EventLoop从epoll拿到监听就绪，Channel回调进入Acceptor。Acceptor持有监听Socket；接受的Socket在交付前设置TCP_NODELAY，失败只释放本次连接，不停监听。
2. `TcpServer::onAccepted`在main创建该连接的MessageCallback。workers为0时直接进入main registry；多worker时轮转选目标，把Socket与回调移动到共享`ConnectionHandoff`。
3. `postTaskToWorkerAtIndex`经`EventLoopThread::postTaskToWorker`包装为LoopBoundTask；队列包装持有任务，任务捕获handoff，目标loop队列持有包装。`enqueueLoopTask`排队并通过eventfd唤醒，main投递后返回，不等待连接I/O完成。
4. 目标worker处理loop队列，包装调用`runTaskInWorkerLoop`，再到`TcpServer::adoptConnection`。目标registry此时才创建TcpConnection，绑定消息、关闭和超时回调，插入registry后激活Channel。连接可变状态与timer始终归这个owner。
5. 停止状态下拒绝接管或投递未被接受时，捕获对象/任务释放后handoff的Socket按RAII释放；已接管则由ConnectionIo持有Socket。投递成功不保证接管成功，关闭/错误路线仍需保全资源。

**核心追问：为什么不在main创建后直接给worker用？** EventLoop、Channel注册与连接状态都要求owner线程；交接的是尚未激活的资源，目标线程才建立连接及事件关系。factory可在main创建回调，但共享Session延迟至首次消息，在连接owner创建；不能把回调创建线程当成业务执行线程。server持有线程池，cleanup后join保证捕获server的任务不越过其寿命。

## 请求解析到发送完成

入口：[HTTP适配](../app/HttpConnectionHandler.cpp)、[HTTP parser](../src/http/HttpRequest.cpp)、[TcpConnection](../src/net/TcpConnection.cpp)、[StaticFileService](../src/http/StaticFileService.cpp)。

1. worker读就绪→Channel→`TcpConnection::handleConnectionEvent`→ConnectionIo读取Buffer。消息回调里的HttpMessageHandler第一次创建Session；消息回调和writeComplete回调共享持有它。Session持有parser、Reading/Writing/Closing阶段及请求观测状态，provider借用活得更久的StaticFileService。
2. `Session::onMessage`只在Reading且非draining时推进。首次非空输入建立requestPending/开始计时；`feedRequestBytes`返回status与acceptedBytes，仅消费当前请求已接受的字节。消费后不能再用旧inputView；半包返回kNeedMore，恢复读取并回到loop等待新输入。
3. 完整请求调用provider；静态文件服务以root目录fd逐级打开并返回响应头及独占FileRegion。解析错误返回400、合法不支持方法返回405；provider异常或放宽terminal policy生成500并关闭。
4. Session先进入Writing、暂停读取，写定响应状态、close与completed等字段，再调用`sendFile`或`sendBytes`。当前HTTP消息处于handlingEvent_内，send只排队；即使在事件外，装有writeCompleteCallback_时也不直接flush。消息处理返回后，同一次handleConnectionEvent调用flushOutput，可能立即排空并同步触发writeComplete，不保证等到另一个EPOLLOUT事件。先写定状态才能让该同步回调与后缀推进看到完整响应状态。
5. 全部输出被kernel接收后，`Session::onWriteComplete`一次终结requestPending。需close或draining时转Closing并closeAfterFlush；否则reset parser、转Reading，调用onMessage处理已缓存后缀，即使没有新EPOLLIN也能推进下一请求。
6. 回调内再次send只排队，由外层flush循环推进；文件路径完成后让出本轮，防止连续文件请求绕过发送公平性安排。没有后缀时kNeedMore返回loop；收到部分下一请求则继续读取，而非误标纯Keep-Alive等待。

**核心追问：流水线为何一次只生成一个响应？** 保持响应顺序并避免把后缀请求全部物化成待发送输出。Writing暂停读取与逻辑输出上限共同约束单连接；完成以kernel接收为界，不能证明客户端已收全，也不是客户端RTT。

## EAGAIN与文件发送恢复

入口：[ConnectionIo](../src/net/ConnectionIo.cpp)、[FileRegion](../include/base/FileRegion.h)、[TcpConnection](../src/net/TcpConnection.cpp)。

1. ConnectionIo复制内存头到输出Buffer，接收移动来的FileRegion；FileRegion持有独占文件fd、offset和remaining。pending按内存剩余与文件remaining计，不仅是Buffer长度。
2. `writeAvailable`先send头，后sendfile正文。正字节消耗Buffer或推进文件offset，同时刷新传输活动；EINTR按路径重试，EAGAIN/EWOULDBLOCK报告wouldBlock并保留全部剩余状态。
3. `flushOutput`遇wouldBlock返回，TcpConnection根据pending订阅EPOLLOUT，owner回到事件循环处理其他连接；不能在EAGAIN处忙等，也不能从另一线程继续写。
4. 同一worker随后收到EPOLLOUT，再从已有offset继续sendfile，不重发已接受正文。即使未遇EAGAIN，文件路径也受单轮字节/调用预算约束，pending继续通过可写事件推进。
5. 排空时释放FileRegion并按门槛整理空闲Buffer，触发writeComplete；错误、超时或强关转关闭并最终释放文件和连接Socket，不能把未完成输出计为成功。

**核心追问：sendfile的优势和边界？** 正文不先完整拷入用户态响应Buffer，减少物化和用户态复制；内核仍有传输与缓存成本，不能宣称全链路零拷贝。普通文件I/O/冷缓存可能阻塞owner，当前只测hot-cache，不由epoll解决磁盘阻塞。预算意图是减少单次文件发送独占loop，未测其独立收益。

## 超时与进程关闭

入口：[ConnectionRegistry](../src/net/ConnectionRegistry.cpp)、[TimerQueue](../src/timer/TimerQueue.cpp)、[SignalWatcher](../app/SignalWatcher.cpp)、[TcpServer](../src/net/TcpServer.cpp)、[main](../app/main.cpp)、[AsyncLogger](../src/base/AsyncLogger.cpp)。

**单连接超时场景。** registry在owner注册timer，idle只由实际recv/send正字节刷新；Keep-Alive只在响应完成且没有部分下一请求/后缀时等待，取启用截止的较早者。到期回调带fd与identity，查registry防fd复用误关，然后requestClose。连接先移除Channel，关闭回调`onClose`把对象挂入待回收链；loop的`onCleanup`在安全点erase unique_ptr，Socket/FileRegion和回调随之释放。Session析构若仍requestPending则终结为aborted，已完成请求不重复计数。关闭可截断输出，两种超时0为禁用；持续小输入可延长idle，timer不能抢占同owner阻塞provider。

**进程信号场景。** 启动前屏蔽退出信号，main的SignalWatcher持有signalfd并由控制Channel触发；第一次观察到SIGINT/SIGTERM固定绝对截止，调用`requestServerGracefulShutdown`。server先确立stopping拒绝交接、停止监听，再向各owner发控制通知。registry取消连接timer并drain，只排空当前输出，Session看到draining不推进后缀/provider；空闲连接关闭，最后一条连接回收后通知所属loop停止。再次观察到信号或截止强关，不续截止。

worker退出在owner清理registry，再由pool join；main上的registry同样清理。app作用域内server及service销毁后，`LoggerSession::stopSessionLogging`停止接收、排空并join consumer，最后导出metrics。因此捕获service的provider及Session不会晚于service析构使用它；捕获server的worker任务也在server资源生命周期内结束。

**核心追问：为什么不在close回调立即delete连接？** 当前事件分发还可能使用它，先摘Channel再在cleanup安全点销毁避免悬垂。**为什么优雅退出仍有边界？** HTTP截止只能控制连接drain，阻塞stderr可拖延logger join；当前没有全局连接/内存配额或总请求时限，不能承诺整个进程固定时间退出。

## 测量与失败怎么回答

- **“有多少QPS？”** 先说[六档条件与原证据](PERFORMANCE.md#v06s2六档独立基线)：WSL2同机loopback、closed-loop、热缓存，M2为1KiB/2workers/wrk2线程32连接、每轮2s预热+10s测量三轮，QPS中位44884.42、校正P99各轮中位1.648ms。同时M3为4workers/128连接，P99中位811.771ms，不能用最高QPS掩盖长尾。
- **“最有价值的性能修复？”** [TCP_NODELAY案例](PERFORMANCE.md#tcp_nodelay历史局部修复案例)：交付前设置，保持sendfile路径、隔离单连接设置失败；独立配对小文件收益与CPU成本一起报。C固定commit、D固定archive；不把后续提交冒充当时实测身份。
- **“futex排第一说明锁是瓶颈？”** [有限观测](PERFORMANCE.md#v06s3有限观测)的单次M2跟踪使QPS变化-95.85%，system time覆盖完整生命周期，不是函数CPU；未知TID不绑定worker，M6 syscall未知，因此只支持有限现象，尚不足以认定根因。
- **“服务端completed和wrk请求数为何不同？”** 一个是kernel排空且包含审计/预热/退出，另一个是客户端测量窗口完成数；应核各自总账，不能强求相等。校正人口未采为null；三轮P99中位不是合并P99。
- **“当前验证到哪里？”** V0.6历史独立基线有效；本轮只核文档和源码，没有动态测试。V0.5.1搁置未完成，RO-002、TD-001、TD-006仍[开放](../TECH-DEBT-TRACKER.md)；V1.0整体未完成，S3最终回归未开始。
