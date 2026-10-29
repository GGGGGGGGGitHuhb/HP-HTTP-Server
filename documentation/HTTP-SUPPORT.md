# 当前 HTTP 支持与回归矩阵

V1.1/S1已验收的是原零正文协议基线；V1.1/S2当前实现扩展有界Content-Length正文及读取期限，Builder自测及Reviewer独立PASS。S3方法路由已获独立Reviewer PASS；本文维护当前已验收行为，原S1设计/报告不改写。

## 运行入口与计数

按README构建后：

```bash
ctest --test-dir build -R '^http_protocol_baseline(_http)?_tests$' --output-on-failure
```

- `http_protocol_baseline_tests`：直接使用当前 `RequestParser::feedRequestBytes/resetRequestParser`；短语料每个切分点及逐字节、长边界整包/逐字节/127字节块。每次核acceptedBytes、累计requestBytes、终态及reset，不访问私有状态。
- `http_protocol_baseline_http_tests`：真实启动服务器，分别threads=0/2，解析完整响应及正文；同一socket验证连续请求，正常SIGTERM回收；只使用Python标准库。
- CMake共20项，S3前18项全部保留；server_integration_tests仍是历史别名，不能当作新增独立覆盖。现有23个冻结目标及31份相关源码仍不迁移（S2拒绝语料例外保留，本阶段仅两份按S3批准更新静态405 Allow及必要格式，其余29份不改），见[开发说明](DEVELOPMENT.md#冻结旧测试与覆盖限制)。
- 新HTTP测试在`HP_S3_TEST_TMP_ROOT`（CTest设为构建目录的test-tmp）下建立唯一运行目录，删除业务fixture，保留有界服务器日志、每进程JSON回收记录及summary.json。失败亦保留证据。每次socket操作服从同一场景5秒绝对截止，套件120秒，每响应最多9MiB；不依赖短sleep确认事件。

## 支持/拒绝及覆盖映射

| 编号 | 当前行为 | 当前源码入口 | 当前可执行用例 |
| --- | --- | --- | --- |
| P01 | 接受HTTP/1.1 origin-form与合法方法token；Host唯一且非空。非法method token、版本、空白、控制字符、折行、头名拒绝。合法POST/HEAD/扩展方法完整解析后交路由；非法framing优先400 | [HttpRequest](../src/http/HttpRequest.cpp) validateRequestLine/validateHeader/finishLine | 新C++ P01-get/post/head/extension/line-*/missing-host/empty-host、P01-P02-header-*；新HTTP P01-*及P02-post-bad-body |
| P02 | 无CL为零或单个合法十进制CL≤1MiB可接受；支持外围OWS/前导零。重复、列表、符号/非数字/空CL、uint64溢出、TE、Expect均400；合法超限完整头校验后413，后续非法头优先400；关闭不推进后缀 | 同上validateHeader；[Session](../app/HttpConnectionHandler.cpp) onMessage | 新C++ P02-zero/zeros/duplicate-cl/cl-te/post-framing-first、P01-P02-header-*；新HTTP P02-*，每条附有效后缀并检查终止后无多余响应 |
| P03 | 请求行最多4096内容字节（不含CRLF）；请求头累计最多16384字节（含CRLF且终止空行LF必须在上限内完成），正文独立最多1MiB，消费计数含头+正文。只消费第一请求，reset后消费下一请求，结果拥有method/target | HttpRequest feedRequestBytes/buildFeedResult/resetRequestParser | 新C++ P03-line-equal/over、total-equal/over、bare-lf/cr-not-lf/nul、owning-pipeline；全部短语料切分含CR/LF位置；新HTTP P03-segmented-and-P04-sequential |
| P04 | 默认保活，Connection大小写无关、close优先；当前允许空token和空列表项。顺序流水线，一次只生成一个响应，close/400/405不处理后缀 | HttpRequest validateHeader；Session onMessage/onWriteComplete | 新C++ P04-close/repeated/empty-tokens；新HTTP P04-pipeline-200-404-200、valid-invalid-second、empty-tokens、sequential；原http_keep_alive_integration_tests |
| P05 | 首页/小文本/二进制/8MiB文件返回200；超8MiB、危险路径、反斜杠、symlink返回403；不存在404。查询串在路径校验后剥离，任何位置的percent均400（包括query） | [StaticFileService](../src/http/StaticFileService.cpp) validatePath/prepareFileResponse/onResponse；[ConnectionIo](../src/net/ConnectionIo.cpp) | 新HTTP P05-*核完整长度/正文SHA、二进制NUL、等于/超过文件上限、普通和目录symlink、query/percent；原http_server_integration_tests |
| P06 | 空FIN及不完整请求FIN为400并关闭；idle/keep-alive到期静默关闭；RST连接不影响新连接。EOF与RST不互相替代 | [TcpConnection](../src/net/TcpConnection.cpp)、Session onMessage及连接超时链 | 新HTTP P06-fin-0/fin-21、rst-isolation、idle-timeout、keepalive-timeout及P08-after-rst；原keep_alive集成专项 |
| P07 | 当前输出/背压/文件发送机制保持；SIGTERM触发受控停机 | ConnectionIo、[TcpServer](../src/net/TcpServer.cpp)、[SignalWatcher](../app/SignalWatcher.cpp) | 新HTTP每个server作用域的SIGTERM/退出0/未强杀/已wait记录、完整8MiB传输；原r6_callbacks_tests与http_observability_tests保留 |
| P08 | provider异常/放宽终止策略生成500且计错误，日志失败不改变响应；不同连接隔离 | Session及[AccessLog](../app/AccessLog.cpp)、[ServerMetrics](../src/metrics/ServerMetrics.cpp) | 原http_observability_tests的testProvidersAndPipeline及日志异常专项、r6_callbacks_tests会话隔离、observability_http_tests、server_metrics_tests；新HTTP P08-after-rst |

错误表示为状态码与短语加换行；HEAD不发送该正文但保留表示长度。405必须且仅带一条Allow：echo资源为POST，静态fallback为GET, HEAD，非405不带Allow。

## S2新增矩阵

- B01：短正文全部切分/逐字节、二进制、1MiB整包/127字节及不规则块；头16KiB+正文独立限额、reset/结果拥有、兼容parseRequest及scanSteps线性界。
- B02：上限+1、uint64最大合法值413/溢出400、后续TE/Expect/重复CL/非法头优先400，超限未完成头仍NeedMore；无预分配超限body由源码无reserve/resize路径核对。
- B03/B04：真实socketpair经已有provider取得精确body，未完整不调用；两次请求各调用一次；413尾部桶/parseErrors/errors、完整非GET交provider（S3迁移）及FIN不足400。
- B05/B06：真实Registry单timer，close取消、同fd新identity拒绝旧回调、超时aborted一次、Writing超原期限仍可排空、drain清timer。跨线程真实性补充由H系列threads0/2覆盖，不宣称TSan。
- H01/H02：真实GET body+GET同连接200/200、正文含HTTP样式数据、1MiB、POST静态405、HEAD静态无正文200（S3迁移），头部早拒绝413、非法优先400、FIN不足。
- H03/H04：头/body每40ms进度仍约300ms总截止，idle1500ms更长；结束后其他连接可用；前请求清截止，第二请求独立计时。
- H05/H06：413/parseErrors、超时unknown/aborted、active/pending归零、日志不含body；0禁用、未完成正文RST及SIGTERM回收。
- H07：CLI/help默认10000ms、0/最大值可启动、非法/缺值/重复退出2。默认值由help与生产装配核对，实际超时用显式短期限验证，不声称执行10秒默认超时实测。

生产GET消费但忽略正文业务含义；HEAD返回表示头，POST /api/echo回显；不允许的方法由路由返回405。完整请求读取期限默认10秒、0禁用；pipeline后缀在Reading时独立起算，Writing不计入下一读取窗口。超时静默关闭、不发408，不抢占阻塞provider；413为Content Too Large、精确长度和close。

## 当前验证与明确缺口

S1历史验收：Builder执行Debug/Release全16项和新两项ASan+UBSan（LSan启用），并用临时错误预期证明新C++检查在Debug及NDEBUG Release下均失败；HTTP错误预期验证失败也会回收真实子进程与fixture。Reviewer独立复跑同样的16/16、16/16和2/2并给出PASS，独立负对照有效；19条服务器记录（三套正常运行及一次故意失败）均正常回收且未强杀，业务fixture已删除。Reviewer Debug构建有一条make时钟偏差告警，无C++编译诊断；未据此修改候选或重复测试。

- parser覆盖针对当前子集，不是旧parser全部历史语料迁移；逐切分点是输入调度测试，不证明网络每个分段都按send边界到达。
- 8MiB测试证明端到端字节完整，不保证每次运行都触发特定sendfile短写/EAGAIN分支；冻结的内部故障、背压穷举、定时器/线程竞态仍有覆盖缺口。
- 当前新套件验证正常停机与失败fixture回收，不补全强制排空竞态，也不解决阻塞stderr可能拖延进程退出的限制。
- 不实现chunked、Expect/100、运行期路由注册、JSON解析、URL解码、Range、缓存协商、TLS或数据库。未来规划不能作为本阶段缺陷或当前功能。
- TD-006、TD-001、RO-002及旧EOF未知根因保持；无新性能采集、容量或长尾修复结论。

S2已验收：Debug/Release各18项，S1两项/正文两项/r6_callbacks/http_observability六项sanitizer通过；S2 C++错误body预期在Debug/Release非零，HTTP错误预期失败正常清理。Reviewer独立Debug/Release各18/18、规定sanitizer 6/6通过；18次滴流在302–306ms触发300ms绝对期限，51条服务器记录全部正常回收；10秒默认值经CLI/help/config核对，未运行10秒实等待。产品正文逻辑长度1MiB，parser及完整返回结果会有常数倍临时持有，不宣称RSS硬上限。

## S3新增矩阵（已独立验收）

| 编号 | 当前契约 | 证据入口 |
| --- | --- | --- |
| M01 / AC-01 | 任意合法token大小写保留、正文完整才Complete；所有切分/逐字节/reset/拥有性、可信HEAD整行验证 | HttpMethodRouting_test.cpp |
| M02 / E02 / AC-02 | 精确POST /api/echo优先同名文件；普通query切离，percent任何位置400优先；GET/HEAD其他路径静态，其他方法405；无/api命名空间 | router单元与线程0/2真实HTTP路由表、唯一Allow和close后无后缀 |
| M03 / E01 / AC-03 | 空、小、NUL/非UTF8、HTTP样式和1MiB原样回显，Content-Type application/octet-stream；响应拥有body副本 | router拥有性、分段HTTP、echo后同socket GET |
| M04 / E03 / AC-04 | 静态HEAD空/小/8MiB与403/404、echo HEAD405；可信HEAD400/413/FIN、异常500均无正文而CL为表示长度 | GET/HEAD头对比、无FileRegion、fd计数、线上HEAD后echo/GET与EOF无多余字节 |
| M05 / E04 / E06 / E07 / AC-05 | provider异常或放宽终止策略500close，分帧错误零provider调用；echo→HEAD→GET→404顺序、绝对期限/0禁用、RST/drain | C++注入、真实HTTP混合pipeline及S2原生命周期回归 |
| M06 / E05 / AC-06 | 405不增加parseErrors；分帧400/413及provider500各归因；HEAD content_bytes0、echo精确长度，日志无body/query、active/pending0 | C++metrics及独立退出metrics/access记录 |
| AC-07/08 | 20项Debug/Release、8项指定sanitizer、C++双配置/HTTP错误预期和失败清理；冻结/索引/用户注释保护 | Builder报告与Reviewer独立PASS |

路径`/api/echo/`按原静态校验返回403，`GET /api/missing`为404，`POST /missing`为405且不做文件存在性探测。POST echo不依赖Content-Type，不解析JSON或query，不反射网络输入构造响应头。HEAD无整文件读取/sendfile；contentBytes与Content-Length是不同口径。读取期限在完整解析/拒绝后清除，不用于抢占同步处理器。仍无全局连接/内存配额，不关闭TD-006或历史性能风险。

S3 Builder当前自测：Debug/Release各20/20，规定8项ASan+UBSan/LSan 8/8，C++双配置错误预期及HTTP错误预期均非零；失败进程正常回收。独立Reviewer同样20/20、20/20、8/8，负对照-6/-6/1有效，73条HTTP正常回收、10份fixture删除；V1.1全部阶段验收完成，合并发布另行处理。
