# 当前 HTTP 支持与回归矩阵

V1.1/S1已验收的是原零正文协议基线；V1.1/S2当前实现扩展有界Content-Length正文及读取期限，Builder自测及Reviewer独立PASS。S3方法路由已获独立Reviewer PASS；本文维护已验收行为，末节S3已获独立复审PASS，原S1设计/报告不改写。

## 运行入口与计数

按README构建后：

```bash
ctest --test-dir build -R '^http_protocol_baseline(_http)?_tests$' --output-on-failure
```

- `http_protocol_baseline_tests`：直接使用当前 `RequestParser::feedRequestBytes/resetRequestParser`；短语料每个切分点及逐字节、长边界整包/逐字节/127字节块。每次核acceptedBytes、累计requestBytes、终态及reset，不访问私有状态。
- `http_protocol_baseline_http_tests`：真实启动服务器，分别threads=0/2，解析完整响应及正文；同一socket验证连续请求，正常SIGTERM回收；只使用Python标准库。
- CMake共30项，S2前28项全部保留；server_integration_tests仍是历史别名，不能当作新增独立覆盖。现有23个冻结目标及31份相关源码仍不迁移（S2拒绝语料例外保留，V1.1/S3两份Allow历史例外保留；S1历史一份两literal例外保留；S2仅三份精确例外及已批symlink literal补充，其余28份不改），见[开发说明](DEVELOPMENT.md#冻结旧测试与覆盖限制)。
- 新HTTP测试在`HP_S3_TEST_TMP_ROOT`（CTest设为构建目录的test-tmp）下建立唯一运行目录，删除业务fixture，保留有界服务器日志、每进程JSON回收记录及summary.json。失败亦保留证据。每次socket操作服从同一场景5秒绝对截止，套件120秒，每响应最多9MiB；不依赖短sleep确认事件。

## 支持/拒绝及覆盖映射

| 编号 | 当前行为 | 当前源码入口 | 当前可执行用例 |
| --- | --- | --- | --- |
| P01 | 接受HTTP/1.1 origin-form与合法方法token；Host唯一且非空。非法method token、版本、空白、控制字符、折行、头名拒绝。合法POST/HEAD/扩展方法完整解析后交路由；非法framing优先400 | [HttpRequest](../src/http/HttpRequest.cpp) validateRequestLine/validateHeader/finishLine | 新C++ P01-get/post/head/extension/line-*/missing-host/empty-host、P01-P02-header-*；新HTTP P01-*及P02-post-bad-body |
| P02 | 无CL为零或单个合法十进制CL≤1MiB可接受；支持外围OWS/前导零。重复、列表、符号/非数字/空CL、uint64溢出、TE/CL冲突、重复TE/Expect均400；合法超限完整头校验后413，后续非法头优先400；关闭不推进后缀 | 同上validateHeader；[Session](../app/HttpConnectionHandler.cpp) onMessage | 新C++ P02-zero/zeros/duplicate-cl/cl-te/post-framing-first、P01-P02-header-*；新HTTP P02-*，每条附有效后缀并检查终止后无多余响应 |
| P03 | 请求行最多4096内容字节（不含CRLF）；请求头累计最多16384字节（含CRLF且终止空行LF必须在上限内完成），正文独立最多1MiB，消费计数含头+正文。只消费第一请求，reset后消费下一请求，结果拥有method/target | HttpRequest feedRequestBytes/buildFeedResult/resetRequestParser | 新C++ P03-line-equal/over、total-equal/over、bare-lf/cr-not-lf/nul、owning-pipeline；全部短语料切分含CR/LF位置；新HTTP P03-segmented-and-P04-sequential |
| P04 | 默认保活，Connection大小写无关、close优先；当前允许空token和空列表项。顺序流水线，一次只生成一个响应，close/400/405不处理后缀 | HttpRequest validateHeader；Session onMessage/onWriteComplete | 新C++ P04-close/repeated/empty-tokens；新HTTP P04-pipeline-200-404-200、valid-invalid-second、empty-tokens、sequential；原http_keep_alive_integration_tests |
| P05 | 首页/小文本/二进制/8MiB文件返回200；超8MiB、危险路径、反斜杠、symlink返回403；不存在404。原始组件分离后一次解码，query不参与文件定位；目标语法400/结构403均关闭，合法目标的文件symlink403沿原策略 | [StaticFileService](../src/http/StaticFileService.cpp) 公共RequestTarget/prepareFileResponse/onValidatedTarget；[ConnectionIo](../src/net/ConnectionIo.cpp) | 新HTTP P05-*核完整长度/正文SHA、二进制NUL、等于/超过文件上限、普通和目录symlink、query/percent；原http_server_integration_tests |
| P06 | 空FIN及不完整请求FIN为400并关闭；idle/keep-alive到期静默关闭；RST连接不影响新连接。EOF与RST不互相替代 | [TcpConnection](../src/net/TcpConnection.cpp)、Session onMessage及连接超时链 | 新HTTP P06-fin-0/fin-21、rst-isolation、idle-timeout、keepalive-timeout及P08-after-rst；原keep_alive集成专项 |
| P07 | 当前输出/背压/文件发送机制保持；SIGTERM触发受控停机 | ConnectionIo、[TcpServer](../src/net/TcpServer.cpp)、[SignalWatcher](../app/SignalWatcher.cpp) | 新HTTP每个server作用域的SIGTERM/退出0/未强杀/已wait记录、完整8MiB传输；原r6_callbacks_tests与http_observability_tests保留 |
| P08 | provider异常/放宽终止策略生成500且计错误，日志失败不改变响应；不同连接隔离 | Session及[AccessLog](../app/AccessLog.cpp)、[ServerMetrics](../src/metrics/ServerMetrics.cpp) | 原http_observability_tests的testProvidersAndPipeline及日志异常专项、r6_callbacks_tests会话隔离、observability_http_tests、server_metrics_tests；新HTTP P08-after-rst |

错误表示为状态码与短语加换行；HEAD不发送该正文但保留表示长度。405必须且仅带一条Allow：echo资源为POST，静态fallback为GET, HEAD，非405不带Allow。

## S2新增矩阵

- B01：短正文全部切分/逐字节、二进制、1MiB整包/127字节及不规则块；头16KiB+正文独立限额、reset/结果拥有、兼容parseRequest及scanSteps线性界。
- B02：上限+1、uint64最大合法值413/溢出400、后续TE冲突/重复Expect/重复CL/非法头优先400，超限未完成头仍NeedMore；无预分配超限body由源码无reserve/resize路径核对。
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
- 不实现响应chunked、运行期路由注册、JSON解析、Unicode规范化、multipart多Range、TLS或数据库。未来规划不能作为本阶段缺陷或当前功能。
- TD-006、TD-001、RO-002及旧EOF未知根因保持；无新性能采集、容量或长尾修复结论。

S2已验收：Debug/Release各18项，S1两项/正文两项/r6_callbacks/http_observability六项sanitizer通过；S2 C++错误body预期在Debug/Release非零，HTTP错误预期失败正常清理。Reviewer独立Debug/Release各18/18、规定sanitizer 6/6通过；18次滴流在302–306ms触发300ms绝对期限，51条服务器记录全部正常回收；10秒默认值经CLI/help/config核对，未运行10秒实等待。产品正文逻辑长度1MiB，parser及完整返回结果会有常数倍临时持有，不宣称RSS硬上限。

## S3新增矩阵（已独立验收）

| 编号 | 当前契约 | 证据入口 |
| --- | --- | --- |
| M01 / AC-01 | 任意合法token大小写保留、正文完整才Complete；所有切分/逐字节/reset/拥有性、可信HEAD整行验证 | HttpMethodRouting_test.cpp |
| M02 / E02 / AC-02 | 精确POST /api/echo优先同名文件；query有界解析，非法目标400/403优先方法（S2迁移）；GET/HEAD其他路径静态，其他方法405；无/api命名空间 | router单元与线程0/2真实HTTP路由表、唯一Allow和close后无后缀 |
| M03 / E01 / AC-03 | 空、小、NUL/非UTF8、HTTP样式和1MiB原样回显，Content-Type application/octet-stream；响应拥有body副本 | router拥有性、分段HTTP、echo后同socket GET |
| M04 / E03 / AC-04 | 静态HEAD空/小/8MiB与403/404、echo HEAD405；可信HEAD400/413/FIN、异常500均无正文而CL为表示长度 | GET/HEAD头对比、无FileRegion、fd计数、线上HEAD后echo/GET与EOF无多余字节 |
| M05 / E04 / E06 / E07 / AC-05 | provider异常或放宽终止策略500close，分帧错误零provider调用；echo→HEAD→GET→404顺序、绝对期限/0禁用、RST/drain | C++注入、真实HTTP混合pipeline及S2原生命周期回归 |
| M06 / E05 / AC-06 | 405不增加parseErrors；分帧400/413及provider500各归因；HEAD content_bytes0、echo精确长度，日志无body/query、active/pending0 | C++metrics及独立退出metrics/access记录 |
| AC-07/08 | 20项Debug/Release、8项指定sanitizer、C++双配置/HTTP错误预期和失败清理；冻结/索引/用户注释保护 | Builder报告与Reviewer独立PASS |

路径`/api/echo/`由公共目标结构校验对全部方法返回403并关闭，`GET /api/missing`为404，`POST /missing`为405且不做文件存在性探测。POST echo不依赖Content-Type，不解析JSON或使用query业务含义，不反射网络输入构造响应头。HEAD无整文件读取/sendfile；contentBytes与Content-Length是不同口径。读取期限在完整解析/拒绝后清除，不用于抢占同步处理器。仍无全局连接/内存配额，不关闭TD-006或历史性能风险。

S3 Builder当前自测：Debug/Release各20/20，规定8项ASan+UBSan/LSan 8/8，C++双配置错误预期及HTTP错误预期均非零；失败进程正常回收。独立Reviewer同样20/20、20/20、8/8，负对照-6/-6/1有效，73条HTTP正常回收、10份fixture删除；V1.1全部阶段验收完成，合并发布另行处理。

## V1.2/S1 chunked与Expect已验收

最终候选已获独立Reviewer PASS：Debug/Release各22/22、规定sanitizer10/10，三项负对照-6/-6/1有效；真实100背压/EAGAIN三条、30条滴流0.3006–0.3027秒，97条HTTP正常回收及13份fixture删除。

| 编号/AC | 当前契约 | 验证入口 |
| --- | --- | --- |
| C01/Q03、AC01 | 唯一chunked，TECL/重复/畸形400；gzip,chunked501；Expect重复/空400、未知417；400→413→501→417→头准入优先级 | parser矩阵，POST/HEAD仅发头无需body即final，严格禁止早100 |
| C02/C04、AC02 | 任意切分/逐字节，hex/扩展token/quoted/BWS/escape/obs-text；1MiB单块/总量、size行1024、元数据65536等界及+1，wire计数/拥有/reset | 新C++；HTTP Q01二进制/1MiB分段 |
| C03/C04/Q04、AC03 | 零块后必须trailer终止；仅X-Checksum/X-Trace-Id，重复/未声明可接受，声明严格；trailer8192及+1、伪造敏感字段拒绝、FIN各截断前缀400 | 新两suite与精确后缀回显 |
| C05/Q01–Q03、AC04 | RequestHead拥有；事件一次，兼容入口继续解析；等待100/同送/无正文不100、早405/非法target400和HEAD零正文 | 新parser/真实HTTP，Expect早405与无Expect非法body400对照 |
| C06/Q05/Q07、AC05 | 真实sndbuf EAGAIN/pending25、同步writeComplete、interim期限不清不续、drain不生成final；header/extension/data/trailer滴流、0禁用/RST/SIGTERM/其他连接 | C++真实Registry/socketpair及HTTP线程0/2；不是mock或性能采集 |
| C06/Q06、AC06 | 100不算final/access，超时unknown/aborted一次；417/501尾桶、HEAD0、payload/元数据/query不记录，active/pending0 | 实际metrics/access与清理JSON |
| AC07/08 | 22项Debug/Release、10项指定sanitizer、负对照/格式/保护；chunked echo→CL echo→HEAD→GET真实pipeline | Builder报告与后续独立Reviewer；旧20注册保留 |

资源与拒绝规则详见[运行说明](RUNNING.md#v12s1-chunked与expect已验收)。仅解码数据进入body，扩展/trailer丢弃；无响应chunked/解压/通用trailer；S2目标一次解码见下。Expect早方法拒绝不等待body，但无Expect仍先完整校验body。100从不清除首字节绝对期限，不增加response桶。逻辑限额不是全局内存上限，不关闭TD-006及历史性能风险。

V1.2/S1 Builder自测最终Debug/Release各22/22，指定ASan+UBSan/LSan 10/10；新C++双配置和HTTP反证均非零，HTTP失败正常回收。初次全量的TE/CL精确拒绝位置回归已修复并保留原失败证据，原断言未放宽；该S1最终候选独立验收结果见本节开头。

## V1.2/S2 目标解析（已独立验收）

| AC / 用例 | 契约与证据 |
| --- | --- |
| AC01 / T01 U01 | 原始组件先分再一次解码、hex/控制/高字节/空格、结果拥有/raw不改；真实编码请求行每切分位置验证，不把send边界当recv边界 |
| AC02 / T02 U02 | 编码分隔符/双编码%400，点段/反斜杠/空段/尾slash403，坏query优先400；每类拒绝附有效后缀并核close |
| AC03 / T03 U03 | raw4096/4097、query2048/2049、64/65对、key128/129、value1024/1025；plus/重复顺序/编码数据不拆分，聚合预算数学约束与显式计数 |
| AC04 / T04 U01 U02 U04 | 编码文件名/等价echo/Allow/同名shadow、direct static与validated入口/router一致；真实文件/父目录symlink与穿越不泄漏根外secret，旧symlink403保活回归 |
| AC05 / U04 U05 | 合法编码Expect CL/chunked先100，非法目标早400/403无100；HEAD错误零body、encoded echo→HEAD→GET顺序；目标错误不记parse/providerErrors，raw path日志无query |
| AC06/07 | 24+24+12及反证/格式/清理/保护；私有构造的单一验证对象，未扩Session/net/metrics/未来缓存Range |

资源政策与示例见[运行说明](RUNNING.md#v12s2-请求目标与参数已验收)。原始目标保留，头与最终可各从同一raw解析；已解码对象绝不再当raw解析，静态入口不再剥query。字节级文件名不保证Unicode规范化，未来业务不可再次解码query字面%。旧HTTP/资源/性能限制和TD-006整体继续保留。

S2 Builder最终自测Debug/Release各24/24、规定12项ASan+UBSan/LSan 12/12，三个负对照有效且失败清理正常；首轮22/24旧语义冲突与批准后的定向4/4证据保留。独立Reviewer PASS：Debug24/24（19.59秒）、Release24/24（19.55秒）、sanitizer12/12（12.77秒），负对照-6/-6/1；另以128条未携带Connection: close的非法目标验证服务端主动关闭，2条合法symlink403保活对照通过。111条HTTP记录及17份fixture摘要均正常回收。


## V1.2/S3 条件与单区间（已独立验收）

- 四头拥有/重复策略、16KiB、逐字节/所有切分/reset/framing/trailer优先：S04。
- 弱ETag、INM列表含quoted逗号/*/128上界、无效仍抑制IMS；三日期/星期/闰年/50年规则/未来截断：S02/S03、H01。
- 单Range闭区间/开尾/suffix、零/空文件/超界/uint64溢出；重复/畸形/多段忽略，If-Range全部回200，HEAD忽略：S01/S03、H02。
- 304无CL/Type/body，206实际offset/pread/sendfile字节、416保活，304→206→HEAD→416→GET，Expect/完整body/FIN/首字节绝对期限：S05、H03/H04/H06。
- 原10桶索引不变，末尾206/304/416；准确content_bytes/错误归因/无敏感日志/active及pending归零：S06、H05。
- 新两套`http_static_semantics_tests`和`http_static_semantics_http_tests`；当前26项目标身份保留，31份冻结源码本阶段全部不改。只按批准清单适配S2两测试的动态Date比较，保留唯一合法Date和其余全部预期。

不提供内容缓存、强ETag或文件一致快照；If-Range不支持校验器驱动安全续传，所有存在值均回完整200（304条件优先）。无新性能结论，不关闭TD-006或历史EOF风险。

Builder最终自测：Debug26/26（22.13秒）、Release26/26（21.95秒）、指定sanitizer14/14（15.19秒）；三个负对照非零且清理有效。以上为首轮Builder证据；独立初审发现RFC850完整timestamp的50年边界缺陷并给出FAIL，修复及13条回归后独立复审PASS：Debug26/26（22.56秒）、Release26/26（22.21秒）、sanitizer14/14（15.41秒），原反例exit1→0、负对照-6/-6/1、13文件格式通过，127条HTTP及19份fixture清理正常。S3及V1.2阶段验收完成，尚未合并发布。


## V1.3/S1 任务API（已独立验收）

| 契约 | 验证 |
| --- | --- |
| CRUD、完整PUT、ID字符串/规范十进制、query拒绝、两种Allow、HEAD错误无body、路径优先与静态兼容 | A03/A05、B01/B02 |
| strict SAX、root深度1、两必填字段、重复/escaped key、UTF-8/控制/NUL/BOM、8192/8193、title256/257、Content-Type | A01/A02、B03 |
| 容量128/129、释放后ID递增、UINT64_MAX耗尽、无副作用、8线程整对象写/读/快照 | A03/A04、B05/B06 |
| 实际factory连接共享/独立、多worker和重启空状态，失败清理 | A06、B04/B05 |
| no-store唯一、JSON不反射输入、16桶、精确body字节/HEAD0、无敏感日志 | A05/A07、B06/B07 |
| Expect非零CL头-only拒绝400/404关闭，无100/副作用；完整同错误及合法100后JSON400仍保活 | A05、B07 |

任务错误统一JSON仅限任务适配层，公共target/framing/静态/echo不改格式。HEAD任务接口不支持，405close且表示CL正确；任务413/500close，完整请求400/404/415/503沿policy保活。所有任务成功/预期错误和受控500含唯一Cache-Control: no-store，任意条件/Range不改变任务结果。

新增http_task_api_tests/http_task_api_http_tests，原26身份保留。31冻结源码不变；旧活跃HttpStaticSemantics_test.cpp仅批准总桶size==13→16一个literal，原13桶循环/计数和其余旧测试字节保持。源码随附固定nlohmann/json3.12.0，离线构建，不格式化第三方原文件；原风险不因任务功能关闭。

Builder本候选自测Debug28/28（24.03秒）、Release28/28（22.83秒）、规定sanitizer16/16（17.17秒）；三个负对照非零且清理有效。独立Reviewer PASS：Debug28/28（22.53秒）、Release28/28（22.48秒）、sanitizer16/16（16.26秒），负对照-6/-6/1、14文件格式通过，151条HTTP及22份fixture正常回收。两个批准补充均验证，累计纠正3/3，历史停止和Builder早期warning保留；仅S1已完成。

## V1.3/S2 异步任务覆盖（已独立验收）

- E01–E04：2/32/34与ready占槽、取消守恒、100/1000ms排队过期、异常及取消/领取竞争。
- E05：真实Registry一次性timer/回调重入、关闭取消、同fd新identity拒绝旧回调、drain零timer。
- A01：threads0/2门闩确认worker阻塞时其他连接静态/echo可用、同连接pipeline顺序、100后完整FIN响应。
- A02：运行已提交再超时与排队取消无副作用；A03：34槽边界/35次503；A04：关闭后新连接、网络drain先于业务join、门闩放行后槽守恒。
- 真实main复用CRUD检查，原28项身份/所有旧测试字节和16状态桶不改；新增2项，总30，sanitizer规定18项。无性能、TSan、线程抢占或全局内存配额结论。

### V1.3/S2 验收检查点

V1.3/S2经独立Reviewer002复审PASS：Debug30/30（29.17秒）、Release30/30（28.76秒）、规定ASan+UBSan/LSan18/18（22.85秒）；三负对照-6/-6/1、16文件格式通过。55旧测试及31冻结保持；启动异常夹具自清理缺陷已修复，原FAIL与初URL失败保留，累计纠正2/2，无新债。S2已完成，S3未开始，V1.3整体未完成；验收不表示合并或标签发布。

23文件导出由HEAD+stage.patch独立重建验证；20文件与被测候选同字节，3重叠文件仅8处原用户注释差异并人工行为等价审计。export未另编译。5ms非硬实时、逻辑容量非RSS硬限、运行取消不提前释放槽及旧性能/EOF边界保持。
