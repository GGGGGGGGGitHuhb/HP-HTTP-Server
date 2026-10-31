# 测试与开发约定

## 当前验证入口

Bash，仓库根目录；先完成 [构建](../README.md#快速开始)，再执行：

```bash
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost bash tests/http_smoke_test.sh build/hp_http_server
```

CMake当前注册24项：cli_tests、http_server_integration_tests、http_keep_alive_integration_tests、server_integration_tests（HTTP黑盒历史别名）、benchmark_runner_tests、async_logger_batch_tests、r6_callbacks_tests、tcp_nodelay_tests、tcp_nodelay_http_tests、server_metrics_tests、http_observability_tests、observability_http_tests、matrix_benchmark_tests、performance_analysis_tests、http_protocol_baseline_tests、http_protocol_baseline_http_tests、http_request_body_tests、http_request_body_http_tests、http_method_routing_tests、http_method_routing_http_tests、http_chunked_expect_tests、http_chunked_expect_http_tests、http_request_target_tests、http_request_target_http_tests。Python专项含synthetic夹具与子进程/HTTP失败路径，不自动启动正式wrk套。真实socket/HTTP须环境允许；本地Agent另遵守未随Git分发的协作资料及受控执行路线。

历史S1独立12/12和专项ASan+UBSan/LSan2/2属于当时验收，当前新增两项工具测试不能倒写为历史14/14。V1.0/S1只核CMake/命令，不执行CTest、smoke或sanitizer；最终回归属于V1.0/S3。

## V1.1/S1 当前接口协议基线

新增两套测试及[HTTP支持矩阵](HTTP-SUPPORT.md)，原14项保持。2026-10-29 Builder与Reviewer分别在独立构建/夹具目录完成Debug/Release各16/16及新两项ASan+UBSan/LSan 2/2，Reviewer给出PASS。新检查在Debug及NDEBUG Release下的错误预期负对照均有效，HTTP失败场景正常回收自有进程与fixture。Reviewer Debug构建曾出现一条make时钟偏差告警，无C++编译诊断，不描述为全构建零告警。协议、生产代码和冻结旧源码不变；新增测试不等于恢复全部历史覆盖。

```bash
ctest --test-dir build -R '^http_protocol_baseline(_http)?_tests$' --output-on-failure
```

## V1.1/S2 正文与读取期限

新增两项测试验证Content-Length有界正文、拥有关系/分帧、413、provider、绝对读取期限、身份/取消与观测。Builder已执行Debug/Release各18/18、规定六项ASan+UBSan/LSan及C++/HTTP负对照，Reviewer独立PASS，Debug/Release各18/18、规定六项sanitizer 6/6，负对照及失败清理有效。Debug首次make出现一条时钟偏差警告，后续依赖新鲜度检查通过；不能描述为首次全构建零告警。

```bash
ctest --test-dir build -R '^http_request_body(_http)?_tests$' --output-on-failure
```

S2历史精确冻结例外仅把http_keep_alive_integration_tests.cpp的CL9、http_protocol_boundary_cases.h的G03 CL1改为非法+9/+1，并执行受影响文件必要格式化。S1两套测试相应拒绝语料改为非法带符号CL，新合法非零正文由S2套件验证；不删除拒绝后缀断言，原16项注册保持。其余29份冻结文件字节不变，23停用目标不恢复。

## 冻结旧测试与覆盖限制

以下 23 个旧目标因仍引用旧生产头文件或旧接口，从默认构建和 CTest 一起排除；文件保留且内容不变，以后需要时再单独迁移：

- 基础/网络：`base_tests`、`socket_tests`、`network_primitives_tests`、`acceptor_tcp_connection_tests`、`event_loop_channel_tests`、`connection_io_tests`。
- HTTP：`http_connection_callback_tests`、`http_parser_state_tests`、`http_parser_tests`、`static_file_tests`、`http_keep_alive_tests`。
- 线程/资源：`event_loop_thread_tests`、`event_loop_thread_pool_tests`、`multi_reactor_tests`、`timer_queue_tests`、`connection_timeout_tests`、`resource_limits_tests`、`graceful_shutdown_tests`。
- 传输/日志：`sendfile_tests`、`async_logger_tests`、`async_logger_production_tests`、`buffer_tests`、`buffer_backpressure_tests`。

其中 `buffer_backpressure_tests` 经 sendfile/graceful_shutdown/multi_reactor 旧测试 include 链依赖旧接口；connection_timeout/resource_limits 经 multi_reactor；async_logger_production 经 async_logger_test_support 和 main。此列表仅依据接口不兼容，不用于隐藏生产行为失败。原有 parser 全矩阵、定时器/资源穷举及长期并发回归覆盖未在 R6 全部恢复，历史 28/28 不能充当当前验证。


## C++ 格式化与提交检查

使用 Python 3 和固定版本 `clang-format 18.1.3`（优先查找 `clang-format-18`）。
在每个新克隆的仓库中启用一次提交钩子：

```bash
chmod +x .githooks/pre-commit
git config --local core.hooksPath .githooks
```

按受影响文件格式化与只读检查（清单为UTF-8，每行一个仓库相对C++路径，支持空格；空清单不做修改）：

```bash
python3 scripts/format_cpp.py --files-from affected-cpp.txt
python3 scripts/format_cpp.py --check --files-from affected-cpp.txt
```

钩子回归测试：`python3 scripts/test_format_hook.py`，只在临时仓库中创建测试提交。

每次 `git commit` 前，钩子只格式化本次暂存新增、修改或重命名后的 `.cpp`、`.h`、`.cc`、`.hpp`、`.cxx`、`.hxx` 文件，
排除 `build`、`build-*`、`.cache`、`third_party`、`vendor` 和 `generated` 目录。
删除项不检查，范围外未暂存文件不触碰。显式清单允许尚未跟踪的新文件，但拒绝绝对/越界路径、符号链接、排除目录、非C++或不存在的文件；不能与`--hook`组合。手动不带`--files-from`仍为全仓模式，只在授权全仓格式化时使用。格式化产生改动时中止提交，请查看差异、按需重新暂存后再次提交；钩子不会自动暂存。
相关文件存在部分暂存、格式配置尚未暂存、合并冲突、工具缺失或版本不匹配时，先拒绝操作。
提交前还会检查暂存区内容，避免工作区已格式化而提交的仍是旧内容。

格式配置以Google为基础、80列，保留用户的参数/实参不合并、不整体挪到下一行及括号后对齐覆盖项。配置暂存变化不授权hook改动提交范围外文件；历史重构曾分轮迁移；既有用户注释及批准的TD-006格式例外须保护，不据全仓模式扩大编辑范围。
接口职责分组和函数内部逻辑分段仍须人工复核。本地钩子可以被绕过，不能替代服务端 CI 检查。

## V1.1/S3 方法路由与HEAD

新增http_method_routing_tests和http_method_routing_http_tests，保留全部18项原注册；按已批准语义迁移活跃S1/S2方法预期及HEAD读取器，不删除原编号或分帧/终止断言。合法token不再由parser产生405；S2的测试provider现在接收完整POST/HEAD，生产方法限制由router决定。

本阶段冻结例外仅http_keep_alive_integration_tests.cpp和http_server_integration_tests.cpp的静态405 Allow GET→GET, HEAD、对应标签及必要格式；其余29份冻结源码不变，23旧目标不恢复。S2例外不自动延续为本阶段可修改许可。15个受影响C++文件使用clang-format18.1.3格式化和检查。

C++/HTTP负对照显式设置`HP_METHOD_ROUTING_NEGATIVE=1`：仅把真实结果的期望改错，默认不启用；分别运行当前构建的http_method_routing_tests及`python3 tests/HttpMethodRoutingHttp_test.py <server>`。先设置HP_S3_TEST_TMP_ROOT为独立已授权目录；HTTP失败仍回收server、join读取线程、删除业务fixture、保留日志JSON。C++负对照发生在创建fixture/socket之前。Release检查用requireTestCondition而非assert副作用。

S3已获独立Reviewer PASS，测试证据不代表完整HTTP合规、全局内存配额或IO/竞态穷举。TD-006整体仍开放，历史性能限制不变。

S3 Builder最终候选Debug/Release各20/20，规定8项sanitizer 8/8。ASan首次构建有8条Clock skew警告，同候选依赖新鲜度构建通过且无告警；早期C++拼写编译失败和nodiscard警告已修正并保留证据，不描述为首构建零失败。独立Reviewer Debug/Release各20/20、规定sanitizer 8/8，三个构建无告警；新C++双配置负对照-6/-6、HTTP负对照1，73条HTTP记录与10份fixture均正常清理。首次独立Debug未另传--timeout120，套件CTest/Python定义的120秒上界仍适用，无超时或失败；没有据此扩测。

## V1.2/S1 chunked与Expect专项

新增HttpChunkedExpect_test.cpp与HttpChunkedExpectHttp_test.py，注册http_chunked_expect_tests/http_chunked_expect_http_tests，保留旧20身份及23停用目标。活跃P/B/H/M/E六套仅将过时单chunked/单Expect拒绝语料改重复chunked/重复Expect，TE+CL语料保持；原编号和错误/后缀断言不删除。唯一冻结例外为http_keep_alive_integration_tests.cpp的两个同类literal及必要格式，其余30份冻结资料保持；之前例外不续授权。

新C++覆盖精确边界/切分/拥有、真实socketpair预填sndbuf造成EAGAIN和25字节100排队，再解除背压或触发原deadline/drain。HTTP线程0/2显式等待一次100后再发body，Client.continue_response只接受严格的状态行/空行，不吞重复100。场景5秒、套件120秒、响应9MiB及独立fixture/log回收规则沿用。

反证默认关闭，`HP_CHUNKED_EXPECT_NEGATIVE=1`分别运行当前Debug/Release的http_chunked_expect_tests，以及`python3 tests/HttpChunkedExpectHttp_test.py <debug-server>`；设置HP_S3_TEST_TMP_ROOT为独立角色tmp。C++在资源创建前故意错期望，HTTP在实际握手后错body期望并正常stop/wait/join；不改产品。Release使用总执行检查。最终候选独立Reviewer PASS：Debug/Release各22/22、规定sanitizer10/10、15文件格式通过；负对照-6/-6/1，97条HTTP与13份fixture均正常清理。Builder首轮21/22精确消费位置失败及修正保留，末次布局后Reviewer全量通过；不把早期历史写成全过。TD-006整体和历史风险不关闭。

## V1.2/S2 目标解析与参数专项

新增HttpRequestTarget_test.cpp/HttpRequestTargetHttp_test.py，注册http_request_target_tests/http_request_target_http_tests，原22身份保留。T01–T04验证拥有型一次解码、参数/路径边界与直接static/router同一对象；U01–U05真实threads0/2编码文件/路由/关键切分、根外自有secret与symlink、错误关闭、Expect/HEAD/pipeline及metrics/log。aggregate2048受raw2048隐含约束，测试证明可达raw等界及decoded计数，不捏造不可达wire。

批准冻结例外仅三文件及必要格式：http_keep_alive_integration_tests.cpp的bad%20→bad%2G，补充批准normal403循环原穿越目标改现有/escape.txt真实symlink，保留403保活/后200close/EOF；http_protocol_boundary_cases.h G06同bad目标替换；http_server_integration_tests.cpp编码穿越预期400→403。其他28份冻结不变、23停用目标不恢复。活跃P/M/Q旧合法percent拒绝语料改非法hex，P编码点段改403；E02尾slash三个非GET方法由405改403，其他Allow/关闭断言不变。

首次Debug22/24暴露以上尾slash及旧穿越保活预期，原失败保留，获批补充后才修改；不是放宽目标安全关闭。负对照默认关闭，`HP_REQUEST_TARGET_NEGATIVE=1`分别运行Debug/Release http_request_target_tests与`python3 tests/HttpRequestTargetHttp_test.py <debug-server>`，先设HP_S3_TEST_TMP_ROOT为独立角色目录。C++资源创建前错误预期失败，HTTP失败仍stop/wait/join并删除root及root外fixture，保留日志JSON。Release不依赖assert副作用，使用固定clang-format18.1.3的11份受影响C++清单。

独立Reviewer PASS：Debug/Release各24/24、规定12项sanitizer通过，三个构建无告警、11文件格式通过；反证-6/-6/1，128条无客户端close指令的非法目标与2条合法symlink保活对照通过。21项导出逐字节一致且独立HEAD+patch重建一致；28份冻结SHA及三份精确机械重建通过。111条HTTP与17份fixture摘要清理正常；初始22/24失败与批准过程保留。


## V1.2/S3 静态语义验证（已独立验收）

当前26项CTest含新增`http_static_semantics_tests`/`http_static_semantics_http_tests`，旧24身份保留。新HTTP客户端仅本suite按304状态无body定界；旧公共客户端不改。真实threads0/2、5秒场景绝对截止/120秒suite/9MiB响应限制，自有业务fixture删除而保留日志/JSON。日期用固定秒/utime，不依赖sleep跨秒或全局TZ修改；大区间逐字节比较不等于已观测EAGAIN。

```bash
ctest --test-dir build -R '^http_static_semantics(_http)?_tests$' --output-on-failure --timeout 120
```

Debug/Release均全26项；ASan+UBSan/LSan规定14项为原12加新2。新负对照仅环境`HP_STATIC_SEMANTICS_NEGATIVE=1`显式启用：C++在资源创建前错区间起点，HTTP实际206错body且finally回收，不用于常规CI。31冻结源码本阶段全部保持，23停用目标不恢复；唯一旧测试适配为S2 C++/HTTP各自校验合法唯一Date后只排除动态Date值。

S3独立初审FAIL定位RFC850完整timestamp的50年阈值，最小修复与13个确定性回归后Reviewer002 PASS，正式返工1/2。最终Debug/Release26/26、规定sanitizer14/14、三负对照-6/-6/1、13文件格式通过；原反例exit1→0，127条HTTP及19份fixture正常回收。31冻结不变，原失败/首次构建失败保留，无新债。


## V1.3/S1 任务验证（已独立验收）

S1验收时28项CTest保留旧26身份，新增http_task_api_tests/http_task_api_http_tests。Debug/Release各28，规定ASan+UBSan/LSan原14加新2共16。两个新suite覆盖有界SAX/schema/media、容量/ID、8线程固定操作量/8客户端barrier并发、factory共享和真实threads0/2、Expect阶段差异、JSON/no-store与16桶计数。socket场景5秒绝对截止、suite120秒，JSON额外256KiB上限；无性能/TSan结论。

```bash
ctest --test-dir build -R '^http_task_api(_http)?_tests$' --output-on-failure --timeout 120
```

固定third_party/nlohmann/json.hpp 3.12.0及MIT见[来源清单](../third_party/nlohmann/README.md)；CMake只本地PRIVATE SYSTEM include，不下载/安装，第三方原文件不格式化。新负对照只显式设置HP_TASK_API_NEGATIVE=1：C++资源创建前错误TaskInput，HTTP实际POST201后错误title，必须非零且finally回收。原全部旧测试仅一个已批例外：静态S06总桶13→16，旧i<13/索引/计数及其他字节保持；31冻结/23停用目标不变。

S1独立Reviewer PASS：Debug/Release各28/28、规定sanitizer16/16，三负对照-6/-6/1、14项目文件格式通过；151条HTTP及22份fixture清理正常。25项候选/导出同字节且HEAD+patch独立重建一致，52旧测试SHA不变，唯一S06总数13→16机械重建通过，31冻结不变。累计纠正3/3和协调停止记录保留，原缩进warning已修正但不删除历史；官方MIT行尾空格保留，不为格式变更vendor。

## V1.3/S2 新增异步验证（待独立审查）

新增`bounded_task_executor_tests`与`http_async_task_http_tests`，独立夹具`async_task_test_server`只在测试构建使用。Debug/Release要求30项，ASan+UBSan/LSan为原16加新2共18项；原测试与31冻结文件不修改。C++使用NDEBUG有效检查；`HP_ASYNC_TASK_NEGATIVE=1`仅新suite负对照，生产不读取。

HTTP场景5秒、suite120秒，门闩经管道明确放行；日志保存fixture FINAL admitted/released/outstanding与子进程退出，成功和故意失败均收尾。执行器独立验证ready占槽与取消运行不提前释放，真实Registry验证fd复用及timer生命周期；不以忙等或sleep猜测worker是否已进入门闩。唯一sleep_until用于已明确准入job的绝对过期边界。

### V1.3/S2 验收检查点

V1.3/S2经独立Reviewer002复审PASS：Debug30/30（29.17秒）、Release30/30（28.76秒）、规定ASan+UBSan/LSan18/18（22.85秒）；三负对照-6/-6/1、16文件格式通过。55旧测试及31冻结保持；启动异常夹具自清理缺陷已修复，原FAIL与初URL失败保留，累计纠正2/2，无新债。S2已完成，S3未开始，V1.3整体未完成；验收不表示合并或标签发布。

23文件导出由HEAD+stage.patch独立重建验证；20文件与被测候选同字节，3重叠文件仅8处原用户注释差异并人工行为等价审计。export未另编译。5ms非硬实时、逻辑容量非RSS硬限、运行取消不提前释放槽及旧性能/EOF边界保持。

## V1.3/S3 综合应用验收（候选）

当前新增task_application_concurrency_tests与http_application_acceptance_tests，完整CTest为32项；旧30身份保持，全部旧测试只读。新C++固定8生产者×32操作，验证整对象、容量与执行器槽守恒；HTTP组合在threads0/2运行。每响应/客户端操作有5秒界限，suite120秒，日志16MiB。真实启动前后及中途失败必须自身wait/join/关闭管道/删除临时根，外层救援不能计为通过。

标准库演示入口见[DEMO](DEMO.md)。HTTP测试在构建目录test-tmp下保留application-evidence-*，其中summary、每进程JSON、failure-cleanup及门闩FINAL是清理证据；临时静态根删除。所有服务器仍监听0.0.0.0临时端口，需可信开发环境。

规定独立Debug/Release全32项、ASan+UBSan/LSan原18项加新2项共20项。TSan单独构建，不与ASan混用：

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build-tsan -j2 --target http_task_api_tests bounded_task_executor_tests task_application_concurrency_tests
ctest --test-dir build-tsan -R '^(http_task_api|bounded_task_executor|task_application_concurrency)_tests$' --output-on-failure --timeout 120
```

TSan三目标必须实际运行且无race诊断，结合人工同步/所有权审查；初始化不支持应报告BLOCKED，不以没有诊断声称通过，不自动修改系统或关闭检测。有限检查不证明穷举无数据竞争。

新套件反证入口`HP_APPLICATION_NEGATIVE=1`：分别运行Debug/Release的task_application_concurrency_tests，或`python3 tests/HttpApplicationAcceptance_test.py <server> <async_task_test_server> scripts/demo_task_application.py`。后者需显式设置HP_S3_TEST_TMP_ROOT；故意错误预期非零且正常自清理。Release检查不依赖assert。阶段随后Reviewer已独立PASS，V1.3阶段验收完成，历史技术债保持。

2026-10-31 Builder执行Debug32/32、Release32/32和ASan+UBSan/LSan20/20通过。TSan三个目标均在启动时报`FATAL: ThreadSanitizer: unexpected memory mapping`，未进入测试；这不是race检查通过。该次初始路线按批准门槛阻塞，未获Reviewer独立验收，不关闭V1.3。当时没有更换运行配置、关闭检测或修改系统来重试。原全量后仅完善新Python驱动末尾异常保留，并针对受影响HTTP套件、demo和负对照复验，具体最终证据见本阶段实现报告。


2026-10-31 后续一次有界进程布局适配：保留同一GCC TSan二进制、相同源码与3个目标，仅用已安装setarch关闭该CTest及其测试子进程的地址随机化。实际3/3（1.24秒），无TSan race诊断；原裸跑0/3失败保留。此方式只改变测试进程树属性，不写sysctl或修改全机配置，不关闭race检测，也不用于HTTP/demo/生产服务。不保证适用于所有TSan映射错误；失败应保留证据并停止，不循环换参数。

```bash
setarch x86_64 --addr-no-randomize ctest --test-dir build-tsan \
  -R '^(http_task_api|bounded_task_executor|task_application_concurrency)_tests$' \
  --output-on-failure --timeout 120
```

本次完成的是Builder自测环境障碍处理，S3随后已完成Reviewer独立构建与验证，见最终验收检查点。有限TSan结果不证明穷举无竞争；验证程序中使用本地socket，但该路线不启动HTTP监听器或演示服务器。

### V1.3/S3 最终验收检查点

2026-10-31 V1.3/S3独立Reviewer001 PASS：最终候选Debug32/32（31.00秒）、Release32/32（31.31秒）、ASan+UBSan/LSan20/20（25.36秒）、限定setarch进程布局下TSan3/3（1.26秒）；原裸跑0/3初始化失败保留。三负对照-6/-6/1、一键演示exit0，45进程/12故障/18个FINAL资源回收通过。累计纠正2/2，无新增债务；S1–S3阶段验收全部完成，不表示已合并或标签发布，V1.4未开始。

TSan仅改变指定测试进程树地址随机化属性，不改系统全局配置；有限检查不证明穷举无race。保留全接口监听/可信环境、超时不回滚、join可能等待、TD006/WSL2/长尾/EOF风险。


## V1.4/S1 资源预算验证（候选）

新增resource_budget_tests/http_resource_budget_http_tests，完整34项；ASan+UBSan/LSan原20加新2共22项。旧32身份及全部旧测试源码保持，不新增冻结例外。C++固定8×256竞争、default128/61边界、size_t极值和租约移动；真实socketpair小发送缓冲得到EAGAIN并按实际write/sendfile核守恒。新独立测试二进制仅复用既有TcpServerTestAccess友元；新单次operator-new失败注入用于append分配异常回滚，未增加生产seam。

HTTP测试使用独立resource_budget_test_server，小限额构造/控制与快照仅存在测试程序；生产main另验默认边界。stdout控制和stderr日志由两个reader分开收集，避免并发日志拼接控制协议；两reader与三pipe均有清理记录。PORT前/后、中途故障必须自身回收，正常不靠强杀。test-tmp/resource-evidence-*保留日志JSON/summary，合成静态根删除，单suite120秒、socket5秒、总日志16MiB。

TSan沿已验证进程级setarch路线，对独立GCC thread插桩构建运行一次固定四目标：

```bash
setarch x86_64 --addr-no-randomize ctest --test-dir build-tsan \
  -R '^(http_task_api|bounded_task_executor|task_application_concurrency|resource_budget)_tests$' \
  --output-on-failure --timeout 120
```

原裸跑失败历史保留；不改全局ASLR、不使用suppression、不以初始化失败当通过、不反复换参数追绿。有限动态检测不等于穷举竞态。

新反证`HP_RESOURCE_NEGATIVE=1`：Debug/Release resource_budget_tests在R01有效检查后故意失败；HTTP用`python3 tests/HttpResourceBudgetHttp_test.py <server> <resource_budget_test_server>`，先设独立HP_S3_TEST_TMP_ROOT，真实资源恢复后故意错误预期且自身清理。Release使用总执行检查，源码不为反证修改。阶段状态和完整证据待独立审查，不据Builder自测关闭V1.4或原技术债。

2026-10-31 S1首轮Builder矩阵：Debug34/34、Release33/34、ASan+UBSan/LSan22/22、TSan4/4。唯一Release失败为旧TaskApplicationConcurrency_test.cpp:239：waitReady(32)返回后outstanding==32断言失败；失败日志未打印具体outstanding数值。ready计数不必然表示另一个取消中的running已退出是初步机制推断，须Leader核查，不能自行修改旧测试/执行器、重跑追绿或降低矩阵要求。该轮阻塞记录保留，未独立验收。新HTTP驱动初次合并stderr/stdout导致控制行被日志前缀拼接的失败及分流修正均保留，最终三个配置该新套件通过。

2026-10-31 补充001批准后，旧TaskApplicationConcurrency仅waitQuiescentReady及两处调用改为同snapshot联合终态（ready/outstanding=count、queued/running=0），保留5秒上界及原断言；其余59旧文件不变。新resource_budget_tests用两独立门闩实测受控32 ready/1 running/33 outstanding中间态，再释放至32、补槽34及最终36 admitted/released；不把该受控计数冒称原失败日志采集值。最终候选Builder各一次Debug34/34、Release34/34、ASan22/22及既定setarch TSan4/4通过，负对照-6/-6/1，待Reviewer独立审查。产品执行器未改，原Release33/34和原TSan4/4证据保留。

### V1.4/S1 收口检查点

2026-10-31 V1.4/S1独立Reviewer002 PASS，累计纠正2/2，无新增债务。r3独立Debug34/34（34.38秒）、Release34/34（33.13秒）、ASan+UBSan/LSan22/22（27.66秒）、限定进程布局TSan4/4（1.36秒）及三负对照通过；r4只改新Python指标消费者，修后三配置HTTP各1/1（2.11/2.09/2.39秒）、同原真实管道反例及HTTP负对照通过，58进程正常清理。未声称r4重跑全量；Debug构建摘要有warning_present，未声称全零告警。原Release33/34与P2 FAIL保留。S1已完成，S2/S3未开始，V1.4整体未完成；验收不代表合并或标签发布。

128连接、64MiB输入预留/待发输出逻辑预算及EOF/reset过载策略保持；文件剩余字节不是RSS，旧16桶/原指标含义不变。TD006、WSL2/长尾/EOF风险及原P3不关闭或顺手修改。
