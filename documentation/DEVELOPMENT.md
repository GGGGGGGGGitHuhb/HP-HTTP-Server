# 测试与开发约定

## 当前验证入口

Bash，仓库根目录；先完成 [构建](../README.md#快速开始)，再执行：

```bash
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost bash tests/http_smoke_test.sh build/hp_http_server
```

CMake当前注册20项：cli_tests、http_server_integration_tests、http_keep_alive_integration_tests、server_integration_tests（HTTP黑盒历史别名）、benchmark_runner_tests、async_logger_batch_tests、r6_callbacks_tests、tcp_nodelay_tests、tcp_nodelay_http_tests、server_metrics_tests、http_observability_tests、observability_http_tests、matrix_benchmark_tests、performance_analysis_tests、http_protocol_baseline_tests、http_protocol_baseline_http_tests、http_request_body_tests、http_request_body_http_tests、http_method_routing_tests、http_method_routing_http_tests。Python专项含synthetic夹具与子进程/HTTP失败路径，不自动启动正式wrk套。真实socket/HTTP须环境允许；本地Agent另遵守未随Git分发的协作资料及受控执行路线。

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
