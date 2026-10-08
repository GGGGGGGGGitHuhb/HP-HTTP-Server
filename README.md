# HP HTTP Server

## 当前状态

2026-10-08：V0.5.1 **已搁置（未完成）**。这是用户决定的管理停工，不是验收通过。S1/S2已完成，TCP_NODELAY局部修复已验证；S3已终止且高并发P3验收FAIL，S4停工，长尾尝试修复/定位未果，RO-002仍开放；S5/S6未开始。V0.6已完成：S1/S2及S3（用户批准R002有限三样本范围）均独立PASS并收口；不再以V0.5.1验收完成为前置条件。S3仅小文件系统调用热点/跟踪扰动与小、大文件线程CPU证据，大文件syscall未知、无函数profile或长尾根因结论。

[停工结论与有限验证](benchmark/results/V0.5.1/SHELVED.md)；[历史检查点](history/V0.5.1/README-checkpoints.md)；[诊断资料边界](benchmark/README.md)。现有诊断候选归档保留，不是生产功能或可恢复执行入口。

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

格式配置以Google为基础、80列，保留用户的参数/实参不合并、不整体挪到下一行及括号后对齐覆盖项。配置暂存变化不授权hook改动提交范围外文件；重构分五轮迁移，未涉及文件可能尚不符合此配置，R5再要求全仓检查通过。
接口职责分组和函数内部逻辑分段仍须人工复核。本地钩子可以被绕过，不能替代服务端 CI 检查。

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

预期信号包括当前S1 `12/12` CTest 通过、启动输出中的 `V0.1 / S3 minimal HTTP static file server` 与实际端口，以及上述请求分别返回 `200`、`404`、`405`。服务进程通过 `Ctrl-C` 停止。

## 配置说明

当前没有配置文件系统；CLI 必须各提供一次 `--port <0-65535>` 和 `--root <directory>`，二者顺序可交换。

- `--threads <0-64>`：可选，默认2个worker；0为单Reactor，主线程不计入worker数。严格十进制，重复、缺值、符号、非数字、越界退出2；资源启动错误退出1。
- `--shutdown-timeout-ms <0-60000>`：默认5000。SIGINT/SIGTERM触发停止监听和当前输出排空，0立即关闭；再次观察到信号强关，不延长第一次观察时确定的steady_clock绝对截止。参数重复、缺值、符号、非数字或越界退出2；正常信号退出（含期限截断）退出0，资源/worker异常退出1。
- `--idle-timeout-ms <0-86400000>`：默认30000。从 owner 注册连接开始，只按实际 recv/send 正数字节刷新；EAGAIN、伪事件和只入缓冲均不刷新。
- `--keep-alive-timeout-ms <0-86400000>`：默认15000。一个响应实际排空、无缓存后缀且 parser 无部分下一请求时开始等待；新输入退出等待，重复等待通知不延长截止。
- 两项0分别禁用对应策略，两项都0恢复无超时；同时适用取较早截止。严格无符号十进制；符号、空值、重复、缺值、溢出/越界退出2。到期静默关闭，不发送408，未排空响应可能截断。C++ `TcpServer` 末尾 `ConnectionTimeouts` 默认0/0，app明确传入上述CLI默认值。

- `--port 8080 --root ./www`：监听显式端口并从 `./www` 只读提供文件。
- `--port 0 --root ./www`：由内核分配临时端口，启动输出报告实际非零端口。
- `--root` 必须在监听前成功打开为目录；缺失、非目录或不可打开时进程非零退出。
- 未知、重复、缺值、非法端口和将 `--help` 与其他参数混用都会受控失败。
- 普通错误不会回显 root 的绝对路径。

每个参数只有在实现、测试和 README 命令同时成立时，才视为可用接口。

### 异步日志

生产启动后保留 `info/warn/error` 和 `[LEVEL] message` stderr格式，当前R005修复候选由唯一后台消费者每批最多64条，拼接后一次写出并flush，不等待凑批；批内日志在批次完成时计成功，可见时间可能晚于旧版逐条flush。固定1024个槽位、正文最多1024字节，超长在上限内追加 `...[truncated]`；消息复制后提交，所有等级队列满时丢新，无ERROR同步回退。统计快照提供提交、接受、丢弃、停止拒绝、截断、成功、失败及含在途记录的pending。队列外最多64条在途记录，pending上界1088；批量临时存储固定132672字节（当前ABI），sink写入或flush失败时整批计failed，不重试。R005完整性能矩阵P3未达标，独立Reviewer009为FAIL，不代表S3已完成。

LoggerSession先于服务对象启动，server/worker/callback销毁后停止接收、排空并join。可返回的sink错误计failed，不递归记录、不无限重试。健康stderr保证排空；**阻塞stderr可能拖延最终日志join，HTTP `--shutdown-timeout-ms` 不保证整个进程限时退出**。不修改共享stderr标志，也不强制取消/分离线程。启动失败在监听前退出非零；CLI帮助、参数错误及stdout就绪行通道不变。没有会话的旧库调用仍同步；生产会话停止后不会自动恢复同步。

### Buffer与既有背压

ConnectionIo使用单owner连续Buffer维护读写游标：consume不搬移后缀，仅尾空间不足时整理或增长；recv直接写入懒分配的持有尾区，取消栈中转复制。生产未消费输入仍最多16KiB；通用库显式/默认max_input=0仍无输入硬上限。临时view在consume或下一次修改后失效。

输出仍按“内存可读字节+文件remaining”执行9MiB逻辑上限。小file header复用已有空Buffer；只有内存及文件都排空时，**容量>64KiB的输出释放，≤64KiB保留**。64KiB是空闲保留门槛，不是响应拒绝或读暂停阈值；重复大内存响应会重新分配。单次增长临时旧+新存储上界为生产输入32KiB/内存输出18MiB，另计调用方响应、parser及其他资源，不是进程RSS上限。

Writing暂停读取、一次只生成一个响应、文件未排空时不推进pipeline、idle/drain截止都是原有背压规则，本轮保持。机制测试证明减少搬移/分配及空闲大容量保留，不宣称吞吐提升；S4压测尚未实施。

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

## V0.6/S1 指标与访问记录

当前实现提供基础统计、可选访问记录和退出快照，S1已独立验收，不包含性能验收或长尾修复。启动方式沿用上述二进制，可追加两个无值开关；重复开关或给开关传值返回2：

```bash
.cache/v0.6-s1/builder/build/hp_http_server --port 8080 --root www --threads 2 --access-log --metrics-on-exit
```

`--access-log` 默认关闭。启用时通过原有异步日志向stderr写入 `[INFO] ` 前缀的完整JSON payload，`event=http_access`；其他启动/诊断日志仍是文本。记录仅含method、path、status、content_bytes、duration_us、outcome、path_truncated。method最多16原始字节，path最多96原始字节，去除query/fragment，不记录headers/body/peer；非ASCII逐字节以 `\u00XX` 转义，不进行URL decode。path可能包含业务标识，请按需求选择开启。每条payload最多1024字节，极长path明确标记截断，JSON保持完整；日志队满、sink写失败或观测异常不改变HTTP响应，不增加同步fallback。

`--metrics-on-exit` 默认关闭。正常或受控关闭时，在所有worker join、registry/Session销毁、异步日志排空后，stdout的唯一 `HP_METRICS_BEGIN`/`HP_METRICS_END` 段导出文本 `key integer`。没有自动写文件或监控HTTP路由；stdout写失败返回非零。CLI参数错误不导出。关闭后 `requests_started_total=responses_completed_total+requests_aborted_total`、`latency_count=requests_started_total`、`connections_active=logger_pending=0`。

请求从首次非空parser feed计起；初始空FIN仍保留旧400协议，但不产生请求计数或访问记录。completed表示内存/sendfile输出已被kernel接收，不证明peer完整接收；aborted表示开始后尚未排空即关闭。`responses_status_<200/400/403/404/405/500/unknown>_total` 是已构造响应的状态（也含aborted），未构造响应用unknown；`content_bytes` 是计划正文长度。`errors_total` 每请求至多一次，表示aborted或status>=400；parse/provider原因分项可能重叠，不能相加。steady_clock整数微秒延迟包含该请求解析/provider/输出排队到终结，completed和aborted都入count/sum/max，不是RTT/P99。runtime逐原子快照非事务一致；uint64增量与sum饱和，超出范围不回绕，饱和后总账等式不再保证精确数量。

最终 `logger_submitted/accepted/dropped_full/rejected_stopped/truncated/written/failed/pending` 属于全部共享日志；它们不是精确访问记录丢失数。`access_log_failures_total` 只记录格式化/提交异常；默认关闭无访问记录格式化或提交。HTTP关闭截止不限制阻塞stderr造成的日志join等待，沿用原日志限制。

## 测试与验证

当前S1共12个活跃CTest（原9项加 `server_metrics_tests`、`http_observability_tests`、`observability_http_tests`）。使用Bash，在仓库根执行：

```bash
mkdir -p .cache/v0.6-s1/builder/tmp
export TMPDIR="$PWD/.cache/v0.6-s1/builder/tmp" TMP="$TMPDIR" TEMP="$TMPDIR" PYTHONDONTWRITEBYTECODE=1
cmake -S . -B .cache/v0.6-s1/builder/build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build .cache/v0.6-s1/builder/build -j2
ctest --test-dir .cache/v0.6-s1/builder/build --output-on-failure
```

真实socket/HTTP及LSan按仓库AGENTS受控执行。新测试在build下自行创建 `test-tmp/s1-observability-*` 干净夹具，保留stdout/stderr/result.json并回收自己启动的进程；无需旧benchmark入口。纯统计测试覆盖原子并发/饱和；Session专项覆盖provider/policy异常500、明确未排空中断、registry销毁、观测异常与日志队满/写失败；黑盒覆盖0/2worker、JSON消费者、CLI、sendfile/pipeline/错误/空连接/FIN/RST与关闭。独立Reviewer002：12/12 CTest与ASan+UBSan/LSan专项2/2通过。首审发现的优雅排空分类错误已修复并复验：真实pending→SIGTERM→读满8MiB/EOF记录completed，同时禁止pipeline推进；真正截断仍记aborted。首审FAIL及返工证据保留。


R6 历史交付命令（真实 socket/HTTP 与 sanitizer 测试须使用仓库规定的受控执行路线）：

```bash
mkdir -p .cache/refactor-r6/builder/{tmp,logs,build}
export TMPDIR="$PWD/.cache/refactor-r6/builder/tmp" TMP="$PWD/.cache/refactor-r6/builder/tmp" TEMP="$PWD/.cache/refactor-r6/builder/tmp"
export HP_S3_TEST_TMP_ROOT="$PWD/.cache/refactor-r6/builder/test-tmp" PYTHONDONTWRITEBYTECODE=1
cmake -S . -B .cache/refactor-r6/builder/build -DCMAKE_BUILD_TYPE=Debug
cmake --build .cache/refactor-r6/builder/build -j 6
ctest --test-dir .cache/refactor-r6/builder/build --output-on-failure --timeout 60
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost bash tests/http_smoke_test.sh .cache/refactor-r6/builder/build/hp_http_server
rg --files include src app > .cache/refactor-r6/builder/files.txt
printf '%s\n' tests/R6Callbacks_test.cpp >> .cache/refactor-r6/builder/files.txt
python3 scripts/format_cpp.py --files-from .cache/refactor-r6/builder/files.txt
python3 scripts/format_cpp.py --check --files-from .cache/refactor-r6/builder/files.txt
```

R6交付时默认 CTest 为6项（当前12项见上）：保留 `cli_tests`、`http_server_integration_tests`、`server_integration_tests`（同一黑盒的历史别名）、`http_keep_alive_integration_tests`、`benchmark_runner_tests`；新增 `r6_callbacks_tests`，源码为 `tests/R6Callbacks_test.cpp`。

新增专项验证注册拒绝时机、默认/空消息回调 echo、任务成功/停止/队满、共享槽与函数存储及入队分配失败、失败捕获析构重入、即时完成任务的 owner 析构与配额时序、HTTP 连接状态隔离/延迟 Session/阻塞写续传/关闭寿命。`EventLoopThread::post` 每次新增共享任务槽分配与引用计数成本，没有性能收益声明。

以下 23 个旧目标因仍引用旧生产头文件或旧接口，从默认构建和 CTest 一起排除；文件保留且内容不变，以后需要时再单独迁移：

- 基础/网络：`base_tests`、`socket_tests`、`network_primitives_tests`、`acceptor_tcp_connection_tests`、`event_loop_channel_tests`、`connection_io_tests`。
- HTTP：`http_connection_callback_tests`、`http_parser_state_tests`、`http_parser_tests`、`static_file_tests`、`http_keep_alive_tests`。
- 线程/资源：`event_loop_thread_tests`、`event_loop_thread_pool_tests`、`multi_reactor_tests`、`timer_queue_tests`、`connection_timeout_tests`、`resource_limits_tests`、`graceful_shutdown_tests`。
- 传输/日志：`sendfile_tests`、`async_logger_tests`、`async_logger_production_tests`、`buffer_tests`、`buffer_backpressure_tests`。

其中 `buffer_backpressure_tests` 经 sendfile/graceful_shutdown/multi_reactor 旧测试 include 链依赖旧接口；connection_timeout/resource_limits 经 multi_reactor；async_logger_production 经 async_logger_test_support 和 main。此列表仅依据接口不兼容，不用于隐藏生产行为失败。原有 parser 全矩阵、定时器/资源穷举及长期并发回归覆盖未在 R6 全部恢复，历史 28/28 不能充当当前验证。

### 历史版本验证记录

以下旧目标清单与命令只用于对应历史版本复现，在当前 R6 默认构建不可直接执行；当前命令以本节上方为准。

历史验证命令：

```bash
mkdir -p .cache/v0.4-s2/builder/tmp .cache/v0.4-s2/builder/cache
export TMPDIR="$PWD/.cache/v0.4-s2/builder/tmp" TMP="$PWD/.cache/v0.4-s2/builder/tmp" TEMP="$PWD/.cache/v0.4-s2/builder/tmp"
export PYTHONDONTWRITEBYTECODE=1 XDG_CACHE_HOME="$PWD/.cache/v0.4-s2/builder/cache" HP_S3_TEST_TMP_ROOT="$PWD/.cache/v0.4-s2/builder/tests"
cmake -S . -B build-v0.4-s2 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-v0.4-s2 --verbose
ctest --test-dir build-v0.4-s2 --output-on-failure
./build-v0.4-s2/http_connection_callback_tests
./build-v0.4-s2/http_keep_alive_tests
./build-v0.4-s2/http_keep_alive_integration_tests ./build-v0.4-s2/hp_http_server
./build-v0.4-s2/acceptor_tcp_connection_tests
./build-v0.4-s2/event_loop_channel_tests
./build-v0.4-s2/connection_io_tests
./build-v0.4-s2/http_parser_tests
./build-v0.4-s2/http_parser_state_tests
./build-v0.4-s2/static_file_tests
./build-v0.4-s2/http_server_integration_tests ./build-v0.4-s2/hp_http_server
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost \
  bash tests/http_smoke_test.sh ./build-v0.4-s2/hp_http_server
```

R5 历史 CTest 共28项（含V0.5/S4新增的benchmark runner回归）：

- `benchmark_runner_tests`：预算、测量审计、尾字节、失败退出及子进程回收；临时输出必须位于源码树内，因此导出基线时将构建目录放在导出的源码树内。

- `buffer_tests`：游标/边界/移动/异常、直接recv地址、输出容量及瞬时分配、小header1000轮复用、大容量释放和文件失败所有权。
- `buffer_backpressure_tests`：0/1/2生产HTTP以peer+fd/identity及owner握手验证真实EAGAIN、同owner健康请求/控制、Writing期间无额外recv、恢复pipeline，以及每模式100轮资源回收。

- `async_logger_tests`：固定容量与消息边界、所有权、4×1000条FIFO、满队列丢新、write/flush/异常失败、100轮生命周期、并发stop与精确启动失败。
- `async_logger_production_tests`：真实main组合的0/1/2 worker，握手阻塞消费者期间完整HTTP/EOF、控制退出、worker先join、唯一消费者写日志、fatal及信号mask恢复；实例测试不替代该生产路径。

- `sendfile_tests`：真实sendfile/offset/短写/预算、默认SIGPIPE与mask/pending、文件fd身份、生产无read正文、0/1/2 worker及100轮未完成文件回收。

- `timer_queue_tests`：单调截止、稳定ID、取消/续期、重入、异常安全、100000次更新占用与EventLoop调度。
- `connection_timeout_tests`：实际IO进展、复用等待、静默超时/截断、0/1/2 owner、失败取消与资源回收。

- `http_keep_alive_tests`：小发送缓冲真实EAGAIN、单响应积压、628请求与容量稳定、framing/close/EOF、服务400终止、策略违规500、非递归drain及回调后销毁；S3新增14个具名首/第二请求拒绝及52个FIN截断位置的单次关闭/provider隔离检查。
- `http_keep_alive_integration_tests`：真实生产binary的逐次/粘包三请求、部分第三请求、FIN、服务400后缀终止、403/404复用、暂停时reset及20个后续连接；S3新增7类拒绝的首/第二位置、52个逐前缀FIN，以及不完整请求RST后20连接/fd回收，等待状态采用截止时间。

- `http_parser_state_tests`：原19类与新增26类 framing/Connection 矩阵的全 split 点/逐字节输入、CRLF 跨块、精确消费、粘包剩余/reset、终态零消费、4 KiB/16 KiB 边界和线性扫描/缓存计数；S3再加入66个具名独立预期、9个长边界样本，共6629种调度（短样本全部两段切分、逐字节、两种固定seed），含多短Header累计上限与pending-CR reset。
- `http_connection_callback_tests`：新增两连接各三段增量 feed/consume 与空 HTTP EOF；保留交错分段/EOF/上限、应用异常500、临时响应所有权与真实EAGAIN、显式close后的pipeline单响应、工厂/消息异常、旧身份及新消息路径真实reset。
- `acceptor_tcp_connection_tests`：真实单轮 8 客户端 accept-drain、交付/注册/MOD 失败与恢复、缓冲 EAGAIN 后逐字节续写、回调后销毁、fd/token/关闭 identity 隔离及经 TcpConnection 的真实 ERR|IN。
- `resource_limits_tests`：9MiB输出边界、1000轮真实EAGAIN/存储压缩、1024含batch/执行者、分配失败/析构重入及连接隔离。
- `graceful_shutdown_tests`：0/1/2 worker排空、8MiB完整字节/截断前缀、provider后缀隔离、满队列控制、100轮回收及真实生产SIGINT/SIGTERM。
- `event_loop_thread_pool_tests`：固定0/1/2、启动回滚/就绪、1024含执行者精确上限、异常取消归还及析构重入。
- `multi_reactor_tests`：同进程真实生产factory/service上的owner路由、Session生命周期、并发HTTP、安全路径、交接失败、满worker故障通知及100轮回收；CTest另检查生产CLI默认/0/1/2的OS线程数。
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

### V0.4/S2 主从 Reactor 与专项验证

main执行accept和callback factory；未注册Socket按0→1→…固定轮转交给worker。
连接、Channel、Session/parser及连接表在该owner创建/访问/销毁，连接不迁移；HTTP Session在首次owner回调时惰性构造。
共享StaticFileService只读root fd，每请求独立打开文件；service及自定义provider依赖必须活到所有worker join和callback释放之后。
自定义共享provider/stats由调用者同步，生产默认factory不共享可变统计对象。

池每worker最多1024个已接受但未结束的任务（**含正在执行的任务**）。满/停止时拒绝，交接socket关闭，不重试其他worker或发送额外HTTP状态。
这是固定交接边界；活跃连接仍没有总量上限；S3已提供普通idle及keep-alive等待超时，S4已将所有普通EventLoop入口统一限制为1024个尚未完成/释放的任务，含batch和执行者。
S1独立EventLoopThread接口现在也遵守1024上限，返回false后调用者可在自身截止内重试或放弃。

`TcpServer::RequestStop()` 可跨普通线程调用，立即停止接收并回收所有worker/活动连接，不保证响应排空，也不是信号安全或进程优雅关闭。
server构造/Run/析构由main owner执行，stop调用者需在server析构前结束。任意worker致命失败会通过独立停止通知唤醒main，所有worker join后Run再上报错误。
C++ TcpServer末尾worker_count默认0，旧组件调用保持单Reactor；app显式传入默认2。

```bash
HP_HTTP_TEST_THREADS=0 ctest --test-dir build-v0.4-s2 --output-on-failure --timeout 60 -R '^(http_server_integration_tests|server_integration_tests|http_keep_alive_integration_tests)$'
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost bash tests/http_smoke_test.sh ./build-v0.4-s2/hp_http_server
HP_HTTP_TEST_THREADS=0 NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost bash tests/http_smoke_test.sh ./build-v0.4-s2/hp_http_server
cmake -S . -B build-v0.4-s2-tsan -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer'
cmake --build build-v0.4-s2-tsan --target event_loop_thread_pool_tests multi_reactor_tests event_loop_thread_tests -j4
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R timeout 60s ./build-v0.4-s2-tsan/event_loop_thread_pool_tests
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R timeout 60s ./build-v0.4-s2-tsan/multi_reactor_tests
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R timeout 60s ./build-v0.4-s2-tsan/event_loop_thread_tests
cmake -S . -B build-v0.4-s2-asan -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build-v0.4-s2-asan --target event_loop_thread_pool_tests multi_reactor_tests event_loop_channel_tests -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s ./build-v0.4-s2-asan/event_loop_thread_pool_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s ./build-v0.4-s2-asan/multi_reactor_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s ./build-v0.4-s2-asan/event_loop_channel_tests
```

先沿用上节任务本地tmp/cache环境。`HP_HTTP_TEST_THREADS`只由旧测试fixture和smoke脚本读取，生产程序不读取；不设置时验证真实默认2。
`multi_reactor_tests`不带参数执行完整同进程生产server/factory/service插桩；可选传入server二进制再检查CLI子进程。
CTest自动传入当前server，sanitizer命令保持整个同进程网络/HTTP路径插桩，不把仅客户端检测冒充生产检测。

### V0.4/S1 线程原语与专项验证

`EventLoopThread::Start(init, cleanup)` 在 worker 构造 loop 并完成 init 后返回；
`Post(named_loop_task)` 异步投递，`RequestStop()` 截止接收并唤醒，
`Join()` 等待 owner 清理和线程退出，传播首次工作异常（重复 join 无操作）。
start/join/析构由控制线程串行调用，post/stop 调用者必须在 wrapper 析构前结束。
cleanup 在 init 开始后的退出路径各一次，由 owner 移除外部 Channel、释放 fd；不得抛异常。

仅 `QueueInLoop`、`RequestStop` 与不可变线程身份可跨线程访问 EventLoop。
正常停止排空已接收任务，异常则取消未执行任务并在 owner 释放捕获；失败不等于正常排空。
每次 poll 执行当前任务快照，嵌套投递进入后续轮次。终态 `PollOnce` 不推进，loop 不可重启。
S1历史上普通任务队列无容量上限；当前S4每loop限制1024个未完成任务。单loop的普通RequestStop仍排空已接受任务，HTTP优雅关闭使用独立控制路径。
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

- V0.4/S3批准包：`docs/leader/designs/V0.4/S3-design.md`、`docs/reviewer/reviews/V0.4/S3-review.md`、`docs/leader/reports/V0.4/S3-report-003.md`（Approved，已完成）。

- V0.4/S2批准包：`docs/leader/designs/V0.4/S2-design.md`、`docs/reviewer/reviews/V0.4/S2-review.md`、`docs/leader/reports/V0.4/S2-report-003.md`（Approved，已完成并发布v0.4-s2标签）。

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
- 生产默认main监听、两个worker处理连接，显式 `--threads 0` 保留单 Reactor。EventLoop 绑定构造线程；所有注册、更新、移除、poll、cleanup 设置及销毁始终由该 owner 执行（启动前也不例外）。Channel 不关闭 fd，所有者必须先 remove，回调返回后再销毁 Channel 和 fd owner。
- 消息 span 只在回调期间借用，consume 后不再使用旧视图；send 在返回前复制字节，close_after_flush 立即停止新输入通知并保留待写尾部。HTTP会话在上个响应实际排空后才解析下个请求；pause与永久关闭分离。旧 ApplicationHandler/Result 生产接口已移除。
- 已有 owner 定时队列、连接超时、输出上限和进程优雅关闭；尚无全局连接/内存配额或总请求时限。持续发送少量字节可刷新 idle，阻塞 provider 不能被同 owner timer 抢占，因此这些超时不等同于完整 slowloris/慢读防护。
- 生产静态文件采用小内存响应头与独占fd的sendfile正文；错误/自定义内存响应仍使用输出缓冲。`Handle`/`HandleResponse`是显式物化兼容入口，生产factory使用`PrepareResponse`。不据机制验证承诺性能涨幅。
- 只承诺 Linux / WSL2 方向；HTTP/2、TLS、数据库、代理、L4LB、XDP 和 DPDK 均不在当前范围。

## 许可证

仓库当前未包含 `LICENSE` 文件，尚未授予明确的开源复用许可。

## V0.4/S3 历史实现验证

新增 `timer_queue_tests` 与 `connection_timeout_tests`，保留原18个CTest身份。TimerQueue 使用稳定ID和两个有序索引，一连接最多一个活跃timer；取消/续期不积累旧记录。EventLoop 等待取最近截止向上取整毫秒、调用者限制和1000ms兜底的最小值；同轮先IO与任务，再检查timer，新timer留下一轮。

```bash
mkdir -p .cache/v0.4-s3/builder/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.4-s3/builder/tmp" TMP="$PWD/.cache/v0.4-s3/builder/tmp" TEMP="$PWD/.cache/v0.4-s3/builder/tmp"
export XDG_CACHE_HOME="$PWD/.cache/v0.4-s3/builder/cache" PYTHONDONTWRITEBYTECODE=1
cmake -S . -B build-v0.4-s3 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v0.4-s3 -j4
ctest --test-dir build-v0.4-s3 --output-on-failure --timeout 60
```

真实socket/HTTP和sanitizer测试按仓库AGENTS使用受控提升。S3独立插桩目标：TSan为timer_queue、connection_timeout、multi_reactor与event_loop_thread；ASan/UBSan/LSan为timer_queue、connection_timeout与event_loop_channel。具体构建参数、单进程 `setarch x86_64 -R` TSan路线和原始日志见 `docs/builder/reports/V0.4/S3-report-001.md`。Builder验证不替代Reviewer结论。

## V0.4/S4 资源边界与关闭

每连接最多9437184字节（9 MiB）逻辑待发送输出；恰好上限可用，超限整次拒绝并关闭该连接，不追加错误响应。追加前压缩已发送前缀，vector存储受固定上限约束。合法8 MiB静态文件仍可完整发送。临时provider结果、内核缓冲、活跃连接总量及任意任务捕获对象另计，这不是进程RSS配额。

每EventLoop最多1024个已接受且未执行/释放完的普通任务，包括本地batch和执行者；直接QueueInLoop、EventLoopThread与pool均受限。drain/force/完成通知使用固定控制状态，不占普通任务额度。HTTP仍一次一个响应并在Writing停读。

优雅关闭停止并关闭listener，未adopt的handoff释放fd。owner观察drain后不再读取/解析新请求或调用后续provider；idle/partial立即关闭，已有响应排空后EOF，不继续缓存pipeline。排空期间取消idle/keep-alive计时，所有owner共享关闭绝对截止；到期可能截断原响应。已发送的keep-alive头不改写。同步provider或用户回调必须有限返回，截止不能抢占阻塞代码，也不承诺硬实时或完整抗DoS。

库调用 `TcpServer::RequestGracefulShutdown(steady_clock::time_point)` 发起排空，`ForceShutdown()`强关；原`RequestStop()`继续表示立即停止。库不接管宿主信号；应用的SignalWatcher在创建worker前阻塞信号，server回收Channel并join后恢复原mask。

```bash
mkdir -p .cache/v0.4-s4/builder/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.4-s4/builder/tmp" TMP="$PWD/.cache/v0.4-s4/builder/tmp" TEMP="$PWD/.cache/v0.4-s4/builder/tmp"
export XDG_CACHE_HOME="$PWD/.cache/v0.4-s4/builder/cache" PYTHONDONTWRITEBYTECODE=1
cmake -S . -B build-v0.4-s4 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v0.4-s4 -j4
ctest --test-dir build-v0.4-s4 --output-on-failure --timeout 60
```

新增 `resource_limits_tests`、`graceful_shutdown_tests`，保留原20个身份。真实网络/自有子进程signal测试在受控提升下运行；sanitizer必须同时使用同构建的服务二进制。首轮实现命令、退出码、资源计数和各AC证据见 `docs/builder/reports/V0.4/S4-report-001.md`；线程身份基线、信号故障负对照和最终返工复测见 `docs/builder/reports/V0.4/S4-report-002.md`。当前Builder验证不代替独立Reviewer与Leader收口。


## V0.5/S1 文件传输

生产静态正文通过 Linux `sendfile` 发送，先排完响应头，再按拥有型文件区域推进。每个响应独立打开CLOEXEC文件fd；文件上限仍为8MiB，头与未发送文件的逻辑总量仍受9MiB上限约束。正文不进入用户输出vector，输出缓冲只保存小响应头。一次 `WriteAvailable` 最多推进256KiB文件并限制调用次数，回调重入不能继续消耗下一份文件预算。

文件传输期间禁止再追加输出；文件和头全部排空后才推进pipeline、write-complete与keep-alive等待。实际sendfile正字节刷新idle，EAGAIN不刷新；现有SIGINT/TERM排空、统一截止和强关语义保持。不支持sendfile或传输错误会关闭当前连接，不自动read降级，也不在已开始的响应后追加500。没有新增CLI开关。

传输期间文件内容必须保持稳定。部署更新应写入新文件再原子替换名称：已打开响应继续使用原inode，之后请求取得新文件。原地增长不会超过初始Content-Length；截短可能提前EOF并截断响应。不承诺原地并发写的内容快照，也不承诺冷文件缺页不会阻塞owner。

沿用本仓库任务局部tmp/cache及受控真实网络执行路线，专项命令如下（完整命令、负对照和证据映射见 `docs/builder/reports/V0.5/S1-report-001.md`；共享夹具身份握手返工见 `docs/builder/reports/V0.5/S1-report-002.md`）：

```bash
cmake -S . -B build-v0.5-s1 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v0.5-s1 -j4
ctest --test-dir build-v0.5-s1 --output-on-failure --timeout 60
./build-v0.5-s1/sendfile_tests
cmake -S . -B build-v0.5-s1-tsan -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer'
cmake --build build-v0.5-s1-tsan -j4
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R timeout 60s ./build-v0.5-s1-tsan/sendfile_tests
cmake -S . -B build-v0.5-s1-asan -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build-v0.5-s1-asan -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s ./build-v0.5-s1-asan/sendfile_tests
```

这一步验证传输机制与资源边界；不提供QPS结论，V0.5/S2日志已完成，Buffer已完成独立验收及Leader收口，wrk固定基线已由Builder003和Reviewer002独立完成测量并通过验收。

## 固定版本压测入口

`benchmark/` 提供仓库内工具准备、两个固定提交的独立 Release 构建和受控 localhost wrk 对比。完整命令、失败处理、统计口径与 WSL 限制见 [benchmark/README.md](benchmark/README.md)，实测记录见 [V0.5/S4 基线](benchmark/results/V0.5-S4-baseline.md)。快速 runner 测试已登记 CTest；正式 12 样本约五分钟，需显式运行，不属于默认测试。本阶段已由Reviewer002独立PASS，完整验收与版本收口见本地Leader S4-report-005；数值有效性不代表性能改善。

## V0.6/S2 场景矩阵

新增独立 [矩阵工具与命令](benchmark/matrix/README.md)，固定已验收S1源码，覆盖正文大小、连接模式和worker/连接组合；[本轮结果](benchmark/results/V0.6/S2-matrix.md)保留首次工具超时的2有效/1无效/15未执行；R002新增 [CPU原始证据齐备的Builder18结果](benchmark/results/V0.6/S2-builder-r002.md)（Reviewer002独立PASS）。[Reviewer独立18结果](benchmark/results/V0.6/S2-reviewer-001.md)与逐CPU原始证据复算已通过，S2已完成。新专项 `matrix_benchmark_tests` 是功能/合成反例，新增注册1项且保留S1原12项；本轮只运行工具专项，不复跑整套，不能替代正式18样本；S2数据不据此宣称修复长尾；V0.6最终有限分析见下方S3。

V0.6/S3已在Approved R002有限范围内由Reviewer001独立PASS。工具与命令见 [analysis](benchmark/analysis/README.md)，[Reviewer独立结果](benchmark/results/V0.6/S3-reviewer-limited.md)给出M2未跟踪/跟踪与M6未跟踪三条；新专项performance_analysis_tests验证工具与失败路径，本轮未复跑整套产品CTest/sanitizer。

[Builder有限材料](benchmark/results/V0.6/S3-builder-limited.md)与原四套3valid/1invalid历史保持；系统调用汇总仅M2完整server生命周期，M6 syscall未知。M2单次strace相对QPS变化-95.85%仅证明强观测扰动，不能据排名断言生产瓶颈、稳定性能或原长尾根因；WSL2与既有RO-002/TD-001/TD-006边界保留。
