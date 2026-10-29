# 当前 HTTP 支持与回归矩阵

V1.1/S1 增加当前接口测试，产品协议行为不变。2026-10-29 Builder自测及Reviewer独立验收通过（PASS），阶段已完成；两角色分别使用独立构建与运行目录。本文中“覆盖”指可执行用例，不表示全部HTTP/1.1兼容。

## 运行入口与计数

按README构建后：

```bash
ctest --test-dir build -R '^http_protocol_baseline(_http)?_tests$' --output-on-failure
```

- `http_protocol_baseline_tests`：直接使用当前 `RequestParser::feedRequestBytes/resetRequestParser`；短语料每个切分点及逐字节、长边界整包/逐字节/127字节块。每次核acceptedBytes、累计requestBytes、终态及reset，不访问私有状态。
- `http_protocol_baseline_http_tests`：真实启动服务器，分别threads=0/2，解析完整响应及正文；同一socket验证连续请求，正常SIGTERM回收；只使用Python标准库。
- CMake共16项，原14项全部保留；server_integration_tests仍是历史别名，不能当作新增独立覆盖。现有23个冻结目标及31份相关源码仍不迁移，见[开发说明](DEVELOPMENT.md#冻结旧测试与覆盖限制)。
- 新HTTP测试在`HP_S3_TEST_TMP_ROOT`（CTest设为构建目录的test-tmp）下建立唯一运行目录，删除业务fixture，保留有界服务器日志、每进程JSON回收记录及summary.json。失败亦保留证据。每次socket操作服从同一场景5秒绝对截止，套件120秒，每响应最多9MiB；不依赖短sleep确认事件。

## 支持/拒绝及覆盖映射

| 编号 | 当前行为 | 当前源码入口 | 当前可执行用例 |
| --- | --- | --- | --- |
| P01 | 只接受HTTP/1.1 origin-form GET；Host唯一且非空。非法method token、版本、空白、控制字符、折行、头名拒绝。合法POST/HEAD/扩展方法为405；非法framing优先400 | [HttpRequest](../src/http/HttpRequest.cpp) validateRequestLine/validateHeader/finishLine | 新C++ P01-get/post/head/extension/line-*/missing-host/empty-host、P01-P02-header-*；新HTTP P01-*及P02-post-bad-body |
| P02 | 无CL或唯一全零十进制CL可接受。非零、重复、列表、符号/非数字/空CL、TE、Expect、CL+TE拒绝；拒绝后关闭且不推进后缀 | 同上validateHeader；[Session](../app/HttpConnectionHandler.cpp) onMessage | 新C++ P02-zero/zeros/duplicate-cl/cl-te/post-framing-first、P01-P02-header-*；新HTTP P02-*，每条附有效后缀并检查终止后无多余响应 |
| P03 | 请求行最多4096内容字节（不含CRLF）；请求累计最多16384字节（含CRLF且最后LF必须在上限内完成）。只消费第一请求，reset后消费下一请求，结果拥有method/target | HttpRequest feedRequestBytes/buildFeedResult/resetRequestParser | 新C++ P03-line-equal/over、total-equal/over、bare-lf/cr-not-lf/nul、owning-pipeline；全部短语料切分含CR/LF位置；新HTTP P03-segmented-and-P04-sequential |
| P04 | 默认保活，Connection大小写无关、close优先；当前允许空token和空列表项。顺序流水线，一次只生成一个响应，close/400/405不处理后缀 | HttpRequest validateHeader；Session onMessage/onWriteComplete | 新C++ P04-close/repeated/empty-tokens；新HTTP P04-pipeline-200-404-200、valid-invalid-second、empty-tokens、sequential；原http_keep_alive_integration_tests |
| P05 | 首页/小文本/二进制/8MiB文件返回200；超8MiB、危险路径、反斜杠、symlink返回403；不存在404。查询串在路径校验后剥离，任何位置的percent均400（包括query） | [StaticFileService](../src/http/StaticFileService.cpp) validatePath/prepareFileResponse/onResponse；[ConnectionIo](../src/net/ConnectionIo.cpp) | 新HTTP P05-*核完整长度/正文SHA、二进制NUL、等于/超过文件上限、普通和目录symlink、query/percent；原http_server_integration_tests |
| P06 | 空FIN及不完整请求FIN为400并关闭；idle/keep-alive到期静默关闭；RST连接不影响新连接。EOF与RST不互相替代 | [TcpConnection](../src/net/TcpConnection.cpp)、Session onMessage及连接超时链 | 新HTTP P06-fin-0/fin-21、rst-isolation、idle-timeout、keepalive-timeout及P08-after-rst；原keep_alive集成专项 |
| P07 | 当前输出/背压/文件发送机制保持；SIGTERM触发受控停机 | ConnectionIo、[TcpServer](../src/net/TcpServer.cpp)、[SignalWatcher](../app/SignalWatcher.cpp) | 新HTTP每个server作用域的SIGTERM/退出0/未强杀/已wait记录、完整8MiB传输；原r6_callbacks_tests与http_observability_tests保留 |
| P08 | provider异常/放宽终止策略生成500且计错误，日志失败不改变响应；不同连接隔离 | Session及[AccessLog](../app/AccessLog.cpp)、[ServerMetrics](../src/metrics/ServerMetrics.cpp) | 原http_observability_tests的testProvidersAndPipeline及日志异常专项、r6_callbacks_tests会话隔离、observability_http_tests、server_metrics_tests；新HTTP P08-after-rst |

错误响应正文固定为状态码与短语加换行；新HTTP逐个核400/403/404/405正文、唯一Content-Length及405的Allow: GET。HEAD当前仍按不支持方法返回405错误正文，不具备成功HEAD响应语义。

## 当前验证与明确缺口

本阶段Builder执行Debug/Release全16项和新两项ASan+UBSan（LSan启用），并用临时错误预期证明新C++检查在Debug及NDEBUG Release下均失败；HTTP错误预期验证失败也会回收真实子进程与fixture。Reviewer独立复跑同样的16/16、16/16和2/2并给出PASS，独立负对照有效；19条服务器记录（三套正常运行及一次故意失败）均正常回收且未强杀，业务fixture已删除。Reviewer Debug构建有一条make时钟偏差告警，无C++编译诊断；未据此修改候选或重复测试。

- parser覆盖针对当前子集，不是旧parser全部历史语料迁移；逐切分点是输入调度测试，不证明网络每个分段都按send边界到达。
- 8MiB测试证明端到端字节完整，不保证每次运行都触发特定sendfile短写/EAGAIN分支；冻结的内部故障、背压穷举、定时器/线程竞态仍有覆盖缺口。
- 当前新套件验证正常停机与失败fixture回收，不补全强制排空竞态，也不解决阻塞stderr可能拖延进程退出的限制。
- 不实现非空正文、chunked、Expect/100、HEAD成功语义、动态路由、URL解码、Range、缓存协商、TLS或数据库。未来规划不能作为本阶段缺陷或当前功能。
- TD-006、TD-001、RO-002及旧EOF未知根因保持；无新性能采集、容量或长尾修复结论。
