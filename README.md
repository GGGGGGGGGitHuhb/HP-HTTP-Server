# HP HTTP Server

HP HTTP Server 是一个面向高性能网络岗学习与简历展示的 Linux C++20 HTTP/1.1 服务器项目。项目将从最小可运行服务器逐步演进到 Reactor、连接复用、并发资源治理、性能优化和可观测性；轻量 L7 Gateway 是 V1.0 之后的可选扩展。

## 当前状态

- 当前阶段：V0.4/S1 EventLoop 线程化，`已完成 / Completed`；设计及审查计划均为 Approved revision 1，PM 已于 2026-09-09 批准。单 loop 线程、异步投递、eventfd 唤醒与停止已实现，Builder001、独立 Reviewer001 PASS 与 Leader003 收口齐备。入口：`docs/leader/designs/V0.4/S1-design.md`、`docs/reviewer/reviews/V0.4/S1-review.md`、`docs/leader/reports/V0.4/S1-report-003.md`。

- S2交付：V0.3/S2 Keep-Alive 连接复用已完成 / Completed；原Approved revision1及Approved S2-rework-001已实现，独立Reviewer唯一PASS。批准见 `docs/leader/reports/V0.3/S2-report-002.md`，补充见 `docs/leader/reworks/V0.3/S2-rework-001.md`。

- V0.3/S3 历史验收：独立全新Debug告警0、CTest15/15（4.88秒）、6REQ/8AC/8RV全部通过，parser状态与keep-alive组件ASan/UBSan/LSan无诊断。交付：`docs/builder/reports/V0.3/S3-report-001.md`、`docs/reviewer/reports/V0.3/S3-report-001.md`、`docs/leader/reports/V0.3/S3-report-004.md`。

- 当前版本：`V0.3 HTTP 状态机与连接复用`已完成，S1/S2/S3均已完成；V0.1/V0.2已完成，V0.4整体尚未完成，S1已完成，S2/S3/S4未开始。本阶段未提交或推送。
- 前置版本状态：`V0.1 最小可运行 HTTP Server` 已完成；S1、S2、S3 均有 Approved 基线、Builder 实现证据与 Reviewer `PASS`。
- 最近完成阶段：`V0.4/S1 EventLoop 线程化`；独立 Debug 告警 0、CTest 16/16（9.69 秒）、curl、TSan、ASan/UBSan/LSan 通过，7 REQ、9 AC、9 RV 全通过。交付：`docs/builder/reports/V0.4/S1-report-001.md`、`docs/reviewer/reports/V0.4/S1-report-001.md`、`docs/leader/reports/V0.4/S1-report-003.md`。
- 真实运行入口现为单线程、单 epoll LT 的最小 HTTP/1.1 静态文件服务：严格要求 `--port` 与 `--root`，支持同连接连续无请求体 `GET`，默认返回 `Connection: keep-alive`，显式 close 优先；响应按请求顺序逐个排空。
- 已实现严格 CRLF/Host/request-line/Header 解析、16 KiB 请求上限、4 KiB 请求行上限、8 MiB 文件上限，以及 `200/400/403/404/405/500`。
- 静态文件访问以启动时打开的 root fd 为锚点，逐组件使用 `openat`、`O_NOFOLLOW|O_CLOEXEC`，中间目录另用 `O_DIRECTORY`；拒绝 raw/encoded traversal、反斜杠、歧义组件与 symlink escape。
- S2 的短写/EAGAIN 续传、半关闭、`EPOLLERR/SO_ERROR` 同批读取、稳定 identity guard（保留原 generation 防复用语义）、先 epoll DEL 后释放 fd 和连接错误隔离仍由回归测试保护。
- V0.1/S3历史验收：Reviewer 已在全新的 `build-review-s3/` 中完成 Debug 独立构建且告警为 0，CTest `9/9`；REQ-01..08、AC-01..10、RV-01..10 全部通过，唯一结论为 `PASS`。真实集成摘要为 `200:35, 400:6, 403:6, 404:1, 405:1`，分段 `NeedMore=1`、生产写 EAGAIN `=1`、accept-drain `8/8`、reset 后续连接 `20/20`、secret 泄露 `0`。
- V0.2/S1 验收：全新 `build-review-v0.2-s1/` Debug 构建告警 0，CTest `10/10`，REQ-01..08、AC-01..10、RV-01..10 全部通过；事件专项 ASan/UBSan 无报告，真实 HTTP 回归保持原行为。
- V0.2/S2 已完成；其批准、实现、独立验收与关闭证据见 `docs/leader/reports/V0.2/S2-report-002.md`、`docs/builder/reports/V0.2/S2-report-001.md`、`docs/reviewer/reports/V0.2/S2-report-001.md`、`docs/leader/reports/V0.2/S2-report-003.md`。
- 当前生产 listener/connection 通过非 fd owner 的 Channel 注册，由 EventLoop wait、按 registration token 分发完整 mask；Acceptor 独占监听与 accept-drain，TcpConnection 独占 ConnectionIo、Channel、事件诊断、interest 与本地关闭；TcpServer 持有连接集合，在回调返回后按稳定 identity 回收。
- V0.2/S2 独立验收：全新 Debug 构建告警 0、CTest `11/11`、REQ-01..08/AC-01..10/RV-01..10 全通过；新增组件专项 ASan/UBSan 无诊断。
- V0.2/S3 历史链路（S2会话已替代done）：TcpConnection 发布累计未消费输入/EOF；app 层 HTTP 适配器每连接独立 done，通过 send/consume/close_after_flush 发送并排空；ConnectionIo 仅管理字节读写和缓冲。实现报告见 `docs/builder/reports/V0.2/S3-report-001.md`。
- V0.3/S1 实现：每连接独立 `RequestParser` 接收新字节，NeedMore 时立即消费已接收字节；RequestLine/Headers/Complete/Error 状态保留跨块 CR。`parse_request` 使用同一算法。请求行内容恰好 4096 字节的未结束前缀为 NeedMore，4097 内容字节为错误；完整请求头上限 16384 字节（含 CRLF）。
- 增量 parser 的终态保持到显式 reset，并在首个请求头结尾精确停止。生产在响应排空后 reset，先处理 transport 中缓存的后缀，最多保留一个未排空响应。待输出时暂停读取，正常空闲 EOF 静默关闭；初始空 EOF 或部分请求 EOF 返回400后关闭。
- V0.1/S3 历史 Approved 权威包：
  - `docs/leader/designs/V0.1/S3-design.md`
  - `docs/reviewer/reviews/V0.1/S3-review.md`
- S3 Builder 报告：
  - `docs/builder/reports/V0.1/S3-report-001.md`
- S3 最终 Reviewer 证据：
  - `docs/reviewer/reports/V0.1/S3-report-001.md`
- S3 Leader 关闭报告：
  - `docs/leader/reports/V0.1/S3-report-003.md`
- V0.2/S1 Approved 权威包：
  - `docs/leader/designs/V0.2/S1-design.md`
  - `docs/reviewer/reviews/V0.2/S1-review.md`
  - `docs/leader/reports/V0.2/S1-report-001.md`
- S2 最终 Reviewer 证据：
  - `docs/reviewer/reports/V0.1/S2-report-002.md`

规划能力与当前已实现能力必须区分。具体版本顺序、完成门槛和禁止范围以 `ROADMAP.md` 为准。

## 技术主线

项目计划覆盖：

- C++20、面向对象、RAII、智能指针和 STL。
- Linux fd、系统调用、文件 IO、信号和线程。
- TCP/IP、Socket、非阻塞 IO、epoll LT/ET 和连接生命周期。
- Reactor、事件循环线程池、定时器、Buffer、日志与资源治理。
- HTTP/1.1 请求解析、状态码、Header、Keep-Alive 和静态资源响应。
- wrk 压测、延迟与错误率记录、perf 热点分析。

以上是路线图目标，不表示当前已经实现。

## 环境要求

S3 基线：

- Linux 或 WSL2。
- CMake 3.20 或更高。
- Clang 14 或更高，或者 GCC 11 或更高。
- C++20、CTest 和 curl。
- 不需要数据库、外部服务、Web 框架、网络下载或第三方 HTTP/网络运行时。

当前工作环境已核对：CMake / CTest `3.28.3`、GCC `13.3.0`、Clang `18.1.3`、Ninja `1.11.1`、curl `8.5.0`。wrk、perf 属于后续阶段，不是 S3 工具或完成条件。

## 快速开始

Shell：Bash

工作目录：

```text
/home/power/projects/HP-HTTP-Server
```

```bash
mkdir -p .cache/v0.3-s3/builder/tmp .cache/v0.3-s3/builder/cache
export TMPDIR="$PWD/.cache/v0.3-s3/builder/tmp" TMP="$PWD/.cache/v0.3-s3/builder/tmp" TEMP="$PWD/.cache/v0.3-s3/builder/tmp"
export XDG_CACHE_HOME="$PWD/.cache/v0.3-s3/builder/cache" HP_S3_TEST_TMP_ROOT="$PWD/.cache/v0.3-s3/builder/tests"
cmake -S . -B build-v0.3-s3 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v0.3-s3 --verbose
ctest --test-dir build-v0.3-s3 --output-on-failure
./build-v0.3-s3/hp_http_server --help
./build-v0.3-s3/hp_http_server --port 8080 --root ./www
```

另开一个终端验证：

```bash
curl --http1.1 -i http://127.0.0.1:8080/
curl --http1.1 -i http://127.0.0.1:8080/missing.txt
curl --http1.1 -i -X POST http://127.0.0.1:8080/
```

预期信号包括 `15/15` CTest 通过、启动输出中的 `V0.1 / S3 minimal HTTP static file server` 与实际端口，以及上述请求分别返回 `200`、`404`、`405`。服务进程通过 `Ctrl-C` 停止。

## 配置说明

当前没有配置文件系统；CLI 必须各提供一次 `--port <0-65535>` 和 `--root <directory>`，二者顺序可交换。

- `--port 8080 --root ./www`：监听显式端口并从 `./www` 只读提供文件。
- `--port 0 --root ./www`：由内核分配临时端口，启动输出报告实际非零端口。
- `--root` 必须在监听前成功打开为目录；缺失、非目录或不可打开时进程非零退出。
- 未知、重复、缺值、非法端口和将 `--help` 与其他参数混用都会受控失败。
- 普通错误不会回显 root 的绝对路径。

每个参数只有在实现、测试和 README 命令同时成立时，才视为可用接口。

## 项目结构

```text
app/                 # 严格 CLI、每连接 HTTP 消息适配器与 http/net 组合
include/base/        # 基础约束
include/http/        # 纯 parser、response 与静态文件服务接口
include/net/         # Socket/Epoller/EventLoop/Channel、Acceptor/TcpConnection/ConnectionIo
src/base/
src/http/            # 严格请求解析、响应构造、fd-relative 文件读取
src/net/             # 事件循环/线程原语、监听交付与单连接输入/输出生命周期
tests/               # S1/S2 回归、HTTP 单元/集成/curl smoke
www/index.html       # 最小示例静态首页
CMakeLists.txt
```

依赖方向保持 `app -> http/net`、`http -> base/标准库/Linux 文件 API`、`net -> base`。网络层不理解 HTTP，HTTP 层不管理 epoll 或连接 fd。

## 测试与验证

可复现的验证命令：

```bash
mkdir -p .cache/v0.3-s3/builder/tmp .cache/v0.3-s3/builder/cache
export TMPDIR="$PWD/.cache/v0.3-s3/builder/tmp" TMP="$PWD/.cache/v0.3-s3/builder/tmp" TEMP="$PWD/.cache/v0.3-s3/builder/tmp"
export XDG_CACHE_HOME="$PWD/.cache/v0.3-s3/builder/cache" HP_S3_TEST_TMP_ROOT="$PWD/.cache/v0.3-s3/builder/tests"
cmake -S . -B build-v0.3-s3 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-v0.3-s3 --verbose
ctest --test-dir build-v0.3-s3 --output-on-failure
./build-v0.3-s3/http_connection_callback_tests
./build-v0.3-s3/http_keep_alive_tests
./build-v0.3-s3/http_keep_alive_integration_tests ./build-v0.3-s3/hp_http_server
./build-v0.3-s3/acceptor_tcp_connection_tests
./build-v0.3-s3/event_loop_channel_tests
./build-v0.3-s3/connection_io_tests
./build-v0.3-s3/http_parser_tests
./build-v0.3-s3/http_parser_state_tests
./build-v0.3-s3/static_file_tests
./build-v0.3-s3/http_server_integration_tests ./build-v0.3-s3/hp_http_server
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost \
  bash tests/http_smoke_test.sh ./build-v0.3-s3/hp_http_server
```

当前 CTest 共 16 项（V0.4/S1 保留原 15 项身份，新增线程专项）：

- `http_keep_alive_tests`：小发送缓冲真实EAGAIN、单响应积压、628请求与容量稳定、framing/close/EOF、服务400终止、策略违规500、非递归drain及回调后销毁；S3新增14个具名首/第二请求拒绝及52个FIN截断位置的单次关闭/provider隔离检查。
- `http_keep_alive_integration_tests`：真实生产binary的逐次/粘包三请求、部分第三请求、FIN、服务400后缀终止、403/404复用、暂停时reset及20个后续连接；S3新增7类拒绝的首/第二位置、52个逐前缀FIN，以及不完整请求RST后20连接/fd回收，等待状态采用截止时间。

- `http_parser_state_tests`：原19类与新增26类 framing/Connection 矩阵的全 split 点/逐字节输入、CRLF 跨块、精确消费、粘包剩余/reset、终态零消费、4 KiB/16 KiB 边界和线性扫描/缓存计数；S3再加入66个具名独立预期、9个长边界样本，共6629种调度（短样本全部两段切分、逐字节、两种固定seed），含多短Header累计上限与pending-CR reset。
- `http_connection_callback_tests`：新增两连接各三段增量 feed/consume 与空 HTTP EOF；保留交错分段/EOF/上限、应用异常500、临时响应所有权与真实EAGAIN、显式close后的pipeline单响应、工厂/消息异常、旧身份及新消息路径真实reset。
- `acceptor_tcp_connection_tests`：真实单轮 8 客户端 accept-drain、交付/注册/MOD 失败与恢复、缓冲 EAGAIN 后逐字节续写、回调后销毁、fd/token/关闭 identity 隔离及经 TcpConnection 的真实 ERR|IN。
- `event_loop_thread_tests`：owner、任务顺序与嵌套异步、真实 eventfd 唤醒、停止竞态、异常取消与清理、100 次资源回收及 token 并发/耗尽。
- `event_loop_channel_tests`：真实 wait、ADD/MOD/DEL、读写 interest、回调后销毁、同批 stale token、fd reuse、失败回滚以及经 Channel 的真实 ERR|IN/SO_ERROR/recv。
- `base_tests`、`socket_tests`、`network_primitives_tests`、`connection_io_tests`：保护 S1/S2 fd、epoll、短写/EAGAIN、EINTR、半关闭和组合错误路径。
- `http_parser_tests`：覆盖分段、严格 CRLF/Host/请求行/Header、4 KiB/16 KiB 边界、单请求 consumed bytes、状态响应与 MIME。
- `static_file_tests`：覆盖 root fd、逐组件 no-follow、文本/二进制、query、traversal/symlink、8 MiB 上限、500 分类、500 次 fd 稳态与只读性。
- `cli_tests`：覆盖帮助、参数唯一性/完整性、root 预检、端口错误和不泄露 root 绝对路径。
- `http_server_integration_tests`：真实生产入口覆盖成功与错误状态、分段、显式close后的pipelining 单响应、半关闭、accept-drain、生产 EAGAIN 和 reset 隔离。
- `server_integration_tests`：保留 S2 CTest 标识，映射到同一套更强的 S3 真实入口回归。
- `tests/http_smoke_test.sh`：真实启动端口 0，并用 curl `--path-as-is` 覆盖 `200/400/403/404/405` 与 secret 不泄露。

V0.3/S3独立验收：Reviewer全新Debug告警0、CTest15/15（4.88秒）、curl及parser/keep-alive两项ASan/UBSan/LSan通过，唯一PASS；额外独立4语料/463调度通过。Builder自测15/15（4.92秒）保留为实现证据。见 `docs/reviewer/reports/V0.3/S3-report-001.md`、`docs/builder/reports/V0.3/S3-report-001.md`。

超限请求达到16 KiB未完成即拒绝；若内核尚有未读输入，完整400响应之后可能是EOF或TCP reset。测试仅对具名总头部超限样本允许这两种结束方式，仍检查完整Content-Length、唯一错误、额外字节为零；其他FIN/close场景保持严格EOF。判据依据 `docs/leader/reports/V0.3/S3-report-003.md`，未改变生产关闭策略。

V0.3/S2 Reviewer已在全新 `build-review-v0.3-s2` 独立构建告警0、CTest15/15（3.92秒）；parser状态与keep-alive组件的ASan/UBSan/LSan无诊断，唯一PASS。Builder独立自测也为15/15。专项复现（先沿用上面的任务临时目录环境）：

```bash
cmake -S . -B build-v0.3-s3-asan -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build-v0.3-s3-asan --target http_parser_state_tests http_keep_alive_tests -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build-v0.3-s3-asan/http_parser_state_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build-v0.3-s3-asan/http_keep_alive_tests
```

V0.3/S1增量解析、生产回调及旧Reactor/HTTP回归已由Reviewer在全新 `build-review-v0.3-s1/` 独立验证，CTest13/13，唯一PASS。解析状态和生产回调两项ASan/UBSan/LSan无诊断，详见 `docs/reviewer/reports/V0.3/S1-report-001.md`。

### V0.4/S1 线程原语与专项验证

`EventLoopThread::start(init, cleanup)` 在 worker 构造 loop 并完成 init 后返回；
`post([](EventLoop& loop) { ... })` 异步投递，`request_stop()` 截止接收并唤醒，
`join()` 等待 owner 清理和线程退出，传播首次工作异常（重复 join 无操作）。
start/join/析构由控制线程串行调用，post/stop 调用者必须在 wrapper 析构前结束。
cleanup 在 init 开始后的退出路径各一次，由 owner 移除外部 Channel、释放 fd；不得抛异常。

仅 `queue_in_loop`、`request_stop` 与不可变线程身份可跨线程访问 EventLoop。
正常停止排空已接收任务，异常则取消未执行任务并在 owner 释放捕获；失败不等于正常排空。
每次 poll 执行当前任务快照，嵌套投递进入后续轮次。终态 `poll_once` 不推进，loop 不可重启。
任务队列**无容量上限**，仅适用于受控有限投递；停止单 loop **不是生产 HTTP 的优雅关闭**。
健康阻塞由 eventfd 唤醒；1000 ms 有限等待只作为坏唤醒的故障兜底，不是定时器。

```bash
mkdir -p .cache/v0.4-s1/local/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.4-s1/local/tmp" TMP="$PWD/.cache/v0.4-s1/local/tmp" TEMP="$PWD/.cache/v0.4-s1/local/tmp"
export XDG_CACHE_HOME="$PWD/.cache/v0.4-s1/local/cache" PYTHONDONTWRITEBYTECODE=1
cmake -S . -B build-v0.4-s1 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v0.4-s1 -j4
ctest --test-dir build-v0.4-s1 --output-on-failure --timeout 60
cmake -S . -B build-v0.4-s1-tsan -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer'
cmake --build build-v0.4-s1-tsan --target event_loop_thread_tests -j4
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R timeout 60s ./build-v0.4-s1-tsan/event_loop_thread_tests
cmake -S . -B build-v0.4-s1-asan -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build-v0.4-s1-asan --target event_loop_thread_tests event_loop_channel_tests -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s ./build-v0.4-s1-asan/event_loop_thread_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s ./build-v0.4-s1-asan/event_loop_channel_tests
```

当前 WSL2/GCC 13 的 TSan 默认地址布局会报 `unexpected memory mapping`，最小 std::thread 程序同样失败；
以上 `setarch -R` 仅关闭测试子进程 ASLR，已在当前环境验证可运行，不改变系统设置。
线程测试利用 syscall wrapper 注入错误和 `/proc/self/task/<tid>/syscall` 确认真正阻塞，
并检查 owner 违约子进程的预期 SIGABRT；这些预期断言不是主测试失败。
Agent 执行真实 socket/HTTP、系统跟踪或受限 sanitizer 时遵守仓库受控提升规则。

## 文档索引

- V0.3/S2交付：`docs/leader/designs/V0.3/S2-design.md`、`docs/reviewer/reviews/V0.3/S2-review.md`、`docs/leader/reworks/V0.3/S2-rework-001.md`及Leader关闭报告 `docs/leader/reports/V0.3/S2-report-004.md`。

- `AGENTS.md`：仓库级 Agent 规则、文档权威和完成门槛。
- `ARCHITECTURE.md`：当前态与长期目标架构、模块职责、数据流和依赖边界。
- `ROADMAP.md`：版本路线、前置条件、阶段范围和完成标准。
- `TECH-DEBT-TRACKER.md`：跨阶段技术债、批准延期和持续风险。
- `CHANGELOG.md`：已完成并得到适当验证的变化。
- `docs/leader/GUIDE.md`：Leader 工作指南。
- `docs/builder/GUIDE.md`：Builder 工作指南。
- `docs/reviewer/GUIDE.md`：Reviewer 工作指南。
- `docs/leader/designs/`：阶段设计。
- `docs/leader/reworks/`：批准后的返工设计。
- `docs/reviewer/reviews/`：审查计划。
- `docs/leader/reports/`、`docs/builder/reports/`、`docs/reviewer/reports/`：历史工作证据。

`docs/` 是本项目的本地协作资料目录；是否纳入版本控制或远程发布不属于 Agent 默认工作范围。

## 开发流程

1. Leader 形成 Draft 设计和审查计划。
2. 用户批准后，文档标记为 `Approved`。
3. Builder 只按最新 Approved 基线实现、测试并创建新报告。
4. Reviewer 按 REQ/AC/RV 独立验证并给出 `PASS`、`PASS WITH DEBT`、`FAIL` 或 `BLOCKED`。
5. 只有允许关闭的 Reviewer 结论出现后，阶段才更新为 `已完成`。
6. Leader 同步路线图、README、CHANGELOG 和技术债，再准备下一阶段。

## 已知限制

- 只服务 HTTP/1.1 无请求体 `GET`；默认保活，合法非GET返回405+Allow并关闭。400、provider异常500或显式close后不处理后缀；正常403/404及有界服务500可继续复用。
- 仅允许无 Content-Length 或唯一十进制零值（如0、00）；重复CL（即使都是0）、列表、非零/非法CL、任意Transfer-Encoding或Expect均400关闭，不等待或丢弃body。此为项目受限兼容策略。
- Connection按ASCII大小写无关token合并，close优先，合法未知token忽略；非法token400。未知Upgrade/Proxy-Connection不改变协议或连接策略。
- 不解析 request body 或 chunked，不支持并发/乱序pipelining、Range、压缩、缓存协商、目录列表或 URL decode。
- 任何 `%` 编码请求返回 `400`；歧义路径、反斜杠和 symlink 返回 `403`。
- 每请求累计上限 16 KiB、请求行上限 4 KiB、文件上限 8 MiB。
- 生产仍为单线程单 epoll LT 服务。EventLoop 绑定构造线程；所有注册、更新、移除、poll、cleanup 设置及销毁始终由该 owner 执行（启动前也不例外）。Channel 不关闭 fd，所有者必须先 remove，回调返回后再销毁 Channel 和 fd owner。
- 消息 span 只在回调期间借用，consume 后不再使用旧视图；send 在返回前复制字节，close_after_flush 立即停止新输入通知并保留待写尾部。HTTP会话在上个响应实际排空后才解析下个请求；pause与永久关闭分离。旧 ApplicationHandler/Result 生产接口已移除。
- 已有 eventfd 唤醒，但没有线程池、定时器、空闲超时、进程优雅关闭、全局连接上限或慢连接治理；空闲保活连接继续占用fd，资源治理仍按V0.4推进。
- 文件采用读入内存后复用输出缓冲，不使用 `sendfile`，不作生产安全、容量或性能承诺。
- 只承诺 Linux / WSL2 方向；HTTP/2、TLS、数据库、代理、L4LB、XDP 和 DPDK 均不在当前范围。

## 许可证

仓库当前未包含 `LICENSE` 文件，尚未授予明确的开源复用许可。
