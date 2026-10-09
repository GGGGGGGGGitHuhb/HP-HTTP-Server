# 测试与开发约定

## 当前验证入口

Bash，仓库根目录；先完成 [构建](../README.md#快速开始)，再执行：

```bash
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost bash tests/http_smoke_test.sh build/hp_http_server
```

CMake当前注册14项：cli_tests、http_server_integration_tests、http_keep_alive_integration_tests、server_integration_tests（HTTP黑盒历史别名）、benchmark_runner_tests、async_logger_batch_tests、r6_callbacks_tests、tcp_nodelay_tests、tcp_nodelay_http_tests、server_metrics_tests、http_observability_tests、observability_http_tests、matrix_benchmark_tests、performance_analysis_tests。Python专项含synthetic夹具与子进程/HTTP失败路径，不自动启动正式wrk套。真实socket/HTTP须环境允许；本地Agent另遵守未随Git分发的协作资料及受控执行路线。

历史S1独立12/12和专项ASan+UBSan/LSan2/2属于当时验收，当前新增两项工具测试不能倒写为历史14/14。V1.0/S1只核CMake/命令，不执行CTest、smoke或sanitizer；最终回归属于V1.0/S3。

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
