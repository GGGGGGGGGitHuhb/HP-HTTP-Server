# HP HTTP Server

HP HTTP Server 是一个面向高性能网络岗学习与简历展示的 Linux C++20 HTTP/1.1 服务器项目。项目将从最小可运行服务器逐步演进到 Reactor、连接复用、并发资源治理、性能优化和可观测性；轻量 L7 Gateway 是 V1.0 之后的可选扩展。

## 当前状态

- 当前结论：V0.2/S3 及整个 V0.2 已完成 / Completed；S3 Reviewer 独立 PASS，V0.3 未开始。批准与关闭见 `docs/leader/reports/V0.2/S3-report-002.md`、`docs/leader/reports/V0.2/S3-report-003.md`。

- 当前版本：`V0.2 Reactor 抽象重构`，已完成；S1/S2/S3 均有 Approved 基线、实现与独立 Reviewer PASS。
- 前置版本状态：`V0.1 最小可运行 HTTP Server` 已完成；S1、S2、S3 均有 Approved 基线、Builder 实现证据与 Reviewer `PASS`。
- 最近完成阶段：`V0.2/S3 HTTP 链路重接与回归验证`，已完成；独立全新 Debug 告警 0、CTest `12/12`、REQ-01..08/AC-01..10/RV-01..10 全通过。
- 真实运行入口现为单线程、单 epoll LT 的最小 HTTP/1.1 静态文件服务：严格要求 `--port` 与 `--root`，只处理每连接一个 `GET` 请求，并统一返回 `Connection: close`。
- 已实现严格 CRLF/Host/request-line/Header 解析、16 KiB 请求上限、4 KiB 请求行上限、8 MiB 文件上限，以及 `200/400/403/404/405/500`。
- 静态文件访问以启动时打开的 root fd 为锚点，逐组件使用 `openat`、`O_NOFOLLOW|O_CLOEXEC`，中间目录另用 `O_DIRECTORY`；拒绝 raw/encoded traversal、反斜杠、歧义组件与 symlink escape。
- S2 的短写/EAGAIN 续传、半关闭、`EPOLLERR/SO_ERROR` 同批读取、稳定 identity guard（保留原 generation 防复用语义）、先 epoll DEL 后释放 fd 和连接错误隔离仍由回归测试保护。
- Reviewer 已在全新的 `build-review-s3/` 中完成 Debug 独立构建且告警为 0，CTest `9/9`；REQ-01..08、AC-01..10、RV-01..10 全部通过，唯一结论为 `PASS`。真实集成摘要为 `200:35, 400:6, 403:6, 404:1, 405:1`，分段 `NeedMore=1`、生产写 EAGAIN `=1`、accept-drain `8/8`、reset 后续连接 `20/20`、secret 泄露 `0`。
- V0.2/S1 验收：全新 `build-review-v0.2-s1/` Debug 构建告警 0，CTest `10/10`，REQ-01..08、AC-01..10、RV-01..10 全部通过；事件专项 ASan/UBSan 无报告，真实 HTTP 回归保持原行为。
- V0.2/S2 已完成；其批准、实现、独立验收与关闭证据见 `docs/leader/reports/V0.2/S2-report-002.md`、`docs/builder/reports/V0.2/S2-report-001.md`、`docs/reviewer/reports/V0.2/S2-report-001.md`、`docs/leader/reports/V0.2/S2-report-003.md`。
- 当前生产 listener/connection 通过非 fd owner 的 Channel 注册，由 EventLoop wait、按 registration token 分发完整 mask；Acceptor 独占监听与 accept-drain，TcpConnection 独占 ConnectionIo、Channel、事件诊断、interest 与本地关闭；TcpServer 持有连接集合，在回调返回后按稳定 identity 回收。
- V0.2/S2 独立验收：全新 Debug 构建告警 0、CTest `11/11`、REQ-01..08/AC-01..10/RV-01..10 全通过；新增组件专项 ASan/UBSan 无诊断。
- V0.2/S3 新链路：TcpConnection 发布累计未消费输入/EOF；app 层 HTTP 适配器每连接独立 done，通过 send/consume/close_after_flush 发送并排空；ConnectionIo 仅管理字节读写和缓冲。实现报告见 `docs/builder/reports/V0.2/S3-report-001.md`。
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
mkdir -p .cache/olympus-v0.2-s3/builder/tmp .cache/olympus-v0.2-s3/builder/cache
export TMPDIR="$PWD/.cache/olympus-v0.2-s3/builder/tmp" TMP="$PWD/.cache/olympus-v0.2-s3/builder/tmp" TEMP="$PWD/.cache/olympus-v0.2-s3/builder/tmp"
export XDG_CACHE_HOME="$PWD/.cache/olympus-v0.2-s3/builder/cache" HP_S3_TEST_TMP_ROOT="$PWD/.cache/olympus-v0.2-s3/builder/tests"
cmake -S . -B build-v0.2-s3-builder -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v0.2-s3-builder --verbose
ctest --test-dir build-v0.2-s3-builder --output-on-failure
./build-v0.2-s3-builder/hp_http_server --help
./build-v0.2-s3-builder/hp_http_server --port 8080 --root ./www
```

另开一个终端验证：

```bash
curl --http1.1 -i http://127.0.0.1:8080/
curl --http1.1 -i http://127.0.0.1:8080/missing.txt
curl --http1.1 -i -X POST http://127.0.0.1:8080/
```

预期信号包括 `12/12` CTest 通过、启动输出中的 `V0.1 / S3 minimal HTTP static file server` 与实际端口，以及上述请求分别返回 `200`、`404`、`405`。服务进程通过 `Ctrl-C` 停止。

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
src/net/             # 单线程事件核心、监听交付与单连接输入/输出生命周期
tests/               # S1/S2 回归、HTTP 单元/集成/curl smoke
www/index.html       # 最小示例静态首页
CMakeLists.txt
```

依赖方向保持 `app -> http/net`、`http -> base/标准库/Linux 文件 API`、`net -> base`。网络层不理解 HTTP，HTTP 层不管理 epoll 或连接 fd。

## 测试与验证

可复现的验证命令：

```bash
mkdir -p .cache/olympus-v0.2-s3/builder/tmp .cache/olympus-v0.2-s3/builder/cache
export TMPDIR="$PWD/.cache/olympus-v0.2-s3/builder/tmp" TMP="$PWD/.cache/olympus-v0.2-s3/builder/tmp" TEMP="$PWD/.cache/olympus-v0.2-s3/builder/tmp"
export XDG_CACHE_HOME="$PWD/.cache/olympus-v0.2-s3/builder/cache" HP_S3_TEST_TMP_ROOT="$PWD/.cache/olympus-v0.2-s3/builder/tests"
cmake -S . -B build-v0.2-s3-builder -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-v0.2-s3-builder --verbose
ctest --test-dir build-v0.2-s3-builder --output-on-failure
./build-v0.2-s3-builder/http_connection_callback_tests
./build-v0.2-s3-builder/acceptor_tcp_connection_tests
./build-v0.2-s3-builder/event_loop_channel_tests
./build-v0.2-s3-builder/connection_io_tests
./build-v0.2-s3-builder/http_parser_tests
./build-v0.2-s3-builder/static_file_tests
./build-v0.2-s3-builder/http_server_integration_tests ./build-v0.2-s3-builder/hp_http_server
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost \
  bash tests/http_smoke_test.sh ./build-v0.2-s3-builder/hp_http_server
```

当前 CTest 共 12 项：

- `http_connection_callback_tests`：交错分段/EOF/上限、应用异常500、临时响应所有权与真实EAGAIN、pipeline单响应、工厂/消息异常、旧身份及新消息路径真实reset。
- `acceptor_tcp_connection_tests`：真实单轮 8 客户端 accept-drain、交付/注册/MOD 失败与恢复、缓冲 EAGAIN 后逐字节续写、回调后销毁、fd/token/关闭 identity 隔离及经 TcpConnection 的真实 ERR|IN。
- `event_loop_channel_tests`：真实 wait、ADD/MOD/DEL、读写 interest、回调后销毁、同批 stale token、fd reuse、失败回滚以及经 Channel 的真实 ERR|IN/SO_ERROR/recv。
- `base_tests`、`socket_tests`、`network_primitives_tests`、`connection_io_tests`：保护 S1/S2 fd、epoll、短写/EAGAIN、EINTR、半关闭和组合错误路径。
- `http_parser_tests`：覆盖分段、严格 CRLF/Host/请求行/Header、4 KiB/16 KiB 边界、单请求 consumed bytes、状态响应与 MIME。
- `static_file_tests`：覆盖 root fd、逐组件 no-follow、文本/二进制、query、traversal/symlink、8 MiB 上限、500 分类、500 次 fd 稳态与只读性。
- `cli_tests`：覆盖帮助、参数唯一性/完整性、root 预检、端口错误和不泄露 root 绝对路径。
- `http_server_integration_tests`：真实生产入口覆盖成功与错误状态、分段、pipelining 单响应、半关闭、accept-drain、生产 EAGAIN 和 reset 隔离。
- `server_integration_tests`：保留 S2 CTest 标识，映射到同一套更强的 S3 真实入口回归。
- `tests/http_smoke_test.sh`：真实启动端口 0，并用 curl `--path-as-is` 覆盖 `200/400/403/404/405` 与 secret 不泄露。

V0.2/S3 新消息回调、S1/S2 Reactor 核心及 V0.1 回归均已由 Reviewer 在全新 `build-review-v0.2-s3/` 独立验证，CTest `12/12`，唯一结论 `PASS`；新回调专项 ASan/UBSan 无诊断。旧 11 项按场景映射保留，详见 `docs/reviewer/reports/V0.2/S3-report-001.md`。

## 文档索引

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

- 每个连接只处理一个 HTTP/1.1 Header block，只支持 `GET`；响应后始终关闭。
- 不解析 request body 或 chunked，不支持 keep-alive、pipelining 第二响应、Range、压缩、缓存协商、目录列表或 URL decode。
- 任何 `%` 编码请求返回 `400`；歧义路径、反斜杠和 symlink 返回 `403`。
- 请求累计上限 16 KiB、请求行上限 4 KiB、文件上限 8 MiB。
- 当前是单线程单 epoll LT 的 EventLoop/Channel；所有注册、更新、移除及回调必须在 loop 线程或启动前执行。Channel 不关闭 fd，所有者必须先 remove，回调返回后再销毁 Channel 和 fd owner。
- 消息 span 只在回调期间借用，consume 后不再使用旧视图；send 在返回前复制字节，close_after_flush 立即停止新输入通知并保留待写尾部。HTTP 每连接只发送一次，旧 ApplicationHandler/Result 生产接口已移除。
- 没有线程池、wakeup、定时器、优雅关闭、全局连接上限、高水位或慢连接治理。
- 文件采用读入内存后复用输出缓冲，不使用 `sendfile`，不作生产安全、容量或性能承诺。
- 只承诺 Linux / WSL2 方向；HTTP/2、TLS、数据库、代理、L4LB、XDP 和 DPDK 均不在当前范围。

## 许可证

仓库当前未包含 `LICENSE` 文件，尚未授予明确的开源复用许可。
