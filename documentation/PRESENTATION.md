# 项目展示与简历条目

[项目入口](../README.md) · [架构](../ARCHITECTURE.md) · [性能摘要](PERFORMANCE.md) · [面试讲解](INTERVIEW.md)

## 定位与讲解主线

**高性能HTTP服务器｜C++20主从Reactor与非阻塞文件传输**

Linux C++20静态HTTP服务器学习项目。这里的“高性能”表示围绕连接并发、输出背压和文件传输组织设计，具体测量有环境与负载条件；不表示物理机容量、多核线性扩展或全局长尾已解决。支持受限HTTP/1.1无body GET、Keep-Alive和顺序流水线；TLS、HTTP/2、代理、Range等不在当前能力内。

一分钟讲解可按以下顺序展开：

1. 主线程接受连接、处理信号，把Socket与每连接消息回调交给worker；连接对象在目标线程创建，后续I/O、定时器和回收均归owner，避免跨线程直接操作epoll与连接状态。
2. 每连接Session增量解析请求，只消费acceptedBytes；响应期间暂停读取，文件头与正文依次发送，全部排空后才推进缓存中的下一请求，保持顺序并限制待发送资源。
3. sendfile减少用户态正文物化，EAGAIN保留偏移与pending并通过EPOLLOUT继续；文件发送设单轮预算以减少独占loop的机会。这里说明设计目的，收益大小需实验验证。
4. 用固定源码身份和独立配对检验小响应TCP_NODELAY修复，既记录收益也保留CPU成本与未关闭的高并发长尾；当前展示基线使用独立六档矩阵，不混历史样本。

## 可复用简历条目

以下为Server项目条目素材；据实际参与范围选用，不据此推定全部由个人独立完成，也不替代整份简历的经历核验。

- **连接并发与线程归属**：基于epoll实现主从Reactor，主线程接收后携带Socket与回调投递至worker，由目标线程创建和管理连接；关闭通过owner上的延迟回收完成，约束跨线程连接操作及事件回调中的对象寿命。
- **协议推进与背压**：实现HTTP增量解析与顺序流水线，按acceptedBytes消费输入；Writing阶段暂停读取，输出完成后再解析缓存后缀，并在发送前确立阶段状态，兼容同步writeComplete回调，避免重入导致响应错序。
- **非阻塞文件传输**：用独占FileRegion管理文件fd/offset，先send响应头、再sendfile正文；EAGAIN保留进度并订阅EPOLLOUT继续，结合单轮文件发送预算和逻辑输出上限约束慢接收方的资源占用。
- **性能诊断与验证**：针对小响应Nagle/默认ACK交互，在接受连接交付前启用TCP_NODELAY；独立C/D三轮配对于WSL2同机loopback热缓存、1KiB、2workers、wrk2线程/32连接、5s预热+20s测量下，QPS中位由716.522升至37200.425、各轮校正P99中位由48.399ms降至2.024ms，同时server单核CPU由6.823%升至188.920%。这是局部修复，高并发长尾仍开放。

需要更短的指标条目时可采用：“独立六档矩阵中，WSL2同机loopback/热缓存/closed-loop、1KiB Keep-Alive、2workers、wrk2线程/32连接、每轮2s预热+10s测量、三轮QPS中位44884.42，校正P99各轮中位1.648ms；4workers/128连接档P99为811.771ms，长尾风险未关闭。”它是条件化基线，不能写成修复前后收益。

## 声明追溯

| 展示声明 | 机制与设计判断 | 源码与验证依据 |
| --- | --- | --- |
| worker管理连接生命周期 | handoff持有资源到目标执行，registry只在owner创建/回收，避免抢先激活或事件中销毁 | [TcpServer](../src/net/TcpServer.cpp)、[EventLoopThread](../src/net/EventLoopThread.cpp)、[ConnectionRegistry](../src/net/ConnectionRegistry.cpp)；[架构线程与所有权](../ARCHITECTURE.md#线程与资源所有权) |
| HTTP状态与同步完成安全 | parser独属Session；先Writing再send；writeComplete后reset并处理缓存后缀 | [HTTP适配](../app/HttpConnectionHandler.cpp)、[parser](../src/http/HttpRequest.cpp)；[完整讲解](INTERVIEW.md#请求解析到发送完成) |
| 文件路径可恢复且有界 | FileRegion独占fd/offset，EAGAIN保留进度；响应排空前不生成下一响应 | [ConnectionIo](../src/net/ConnectionIo.cpp)、[FileRegion](../include/base/FileRegion.h)、[TcpConnection](../src/net/TcpConnection.cpp)；[EAGAIN场景](INTERVIEW.md#eagain与文件发送恢复) |
| 安全静态文件访问 | root目录fd逐级打开，拒绝symlink与越界；避免直接把URL当任意文件路径 | [StaticFileService](../src/http/StaticFileService.cpp)；[协议与资源边界](../ARCHITECTURE.md#错误处理与安全边界) |
| 条件化性能基线和局部修复 | 固定对象、独立角色、三轮口径；带收益与成本，避免筛选数字 | [性能摘要及原报告索引](PERFORMANCE.md)、[矩阵Reviewer](../benchmark/results/V0.6/S2-reviewer-001.md)、[修复Reviewer](../benchmark/results/V0.5.1/S2/S2-reviewer-001.md) |
| 关闭与观测有明确边界 | signalfd→owner drain→join→logger drain→最终metrics；HTTP截止无法抢占阻塞sink | [main](../app/main.cpp)、[SignalWatcher](../app/SignalWatcher.cpp)、[AsyncLogger](../src/base/AsyncLogger.cpp)；[关闭场景](INTERVIEW.md#超时与进程关闭) |

机制声明通过当前已提交源码静态核对；历史验证绑定原报告时点。本轮文档整理没有运行build、CTest、smoke、benchmark或strace，不把历史测试数量写成当前全量回归。

## 展示时的取舍与限制

单Reactor便于理解归属，多worker提供并行处理结构，但矩阵同时改变客户端并发，不能声称已验证线性扩展。暂停读取与有界输出保护单连接；它们不能代替尚缺的全局连接/内存配额或总请求时限。sendfile避免用户态正文物化，不等于全链路零拷贝；热缓存结果也不覆盖冷盘阻塞。

超时按正字节进展刷新，持续少量输入可以延长idle；同owner阻塞provider无法被timer抢占。日志队满丢新避免业务线程等空间，代价是日志丢失；阻塞stderr仍可拖延consumer join。RO-002、TD-001、TD-006保持[开放](../TECH-DEBT-TRACKER.md)，V0.5.1搁置未完成；V1.0整体未完成，S3最终回归尚未开始。
