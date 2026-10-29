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

当前28项CTest保留旧26身份，新增http_task_api_tests/http_task_api_http_tests。Debug/Release各28，规定ASan+UBSan/LSan原14加新2共16。两个新suite覆盖有界SAX/schema/media、容量/ID、8线程固定操作量/8客户端barrier并发、factory共享和真实threads0/2、Expect阶段差异、JSON/no-store与16桶计数。socket场景5秒绝对截止、suite120秒，JSON额外256KiB上限；无性能/TSan结论。

```bash
ctest --test-dir build -R '^http_task_api(_http)?_tests$' --output-on-failure --timeout 120
```

固定third_party/nlohmann/json.hpp 3.12.0及MIT见[来源清单](../third_party/nlohmann/README.md)；CMake只本地PRIVATE SYSTEM include，不下载/安装，第三方原文件不格式化。新负对照只显式设置HP_TASK_API_NEGATIVE=1：C++资源创建前错误TaskInput，HTTP实际POST201后错误title，必须非零且finally回收。原全部旧测试仅一个已批例外：静态S06总桶13→16，旧i<13/索引/计数及其他字节保持；31冻结/23停用目标不变。

S1独立Reviewer PASS：Debug/Release各28/28、规定sanitizer16/16，三负对照-6/-6/1、14项目文件格式通过；151条HTTP及22份fixture清理正常。25项候选/导出同字节且HEAD+patch独立重建一致，52旧测试SHA不变，唯一S06总数13→16机械重建通过，31冻结不变。累计纠正3/3和协调停止记录保留，原缩进warning已修正但不删除历史；官方MIT行尾空格保留，不为格式变更vendor。
