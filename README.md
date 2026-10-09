# HP HTTP Server

Linux C++20 静态 HTTP 服务器学习项目：提供受限 HTTP/1.1 GET、Keep-Alive、顺序流水线请求、非阻塞 sendfile、连接超时与优雅关闭。网络层采用 epoll Reactor；默认主线程监听、两个 worker 处理连接，支持单 Reactor 模式。

## 当前状态

V0.6 已完成并发布；V1.0/S1 文档整理与S2展示材料均已完成并通过独立静态审查，V1.0整体未完成，S3最终回归未开始。V0.5.1 **已搁置（未完成）**：局部 TCP_NODELAY 修复证据有效，高并发长尾仍未解决，停工不等于验收。

## 环境要求

Linux / WSL2，支持 C++20 的 GCC 或 Clang、CMake ≥3.20、线程库；默认 `BUILD_TESTING=ON` 还需要 Python 3。curl 用于手工请求，Bash 用于 smoke。核心服务不依赖数据库、Web框架或第三方HTTP运行时。当前CMake项目版本仍为 `0.1.0`，CLI启动文字仍保留 `V0.1 / S3`，不表示发布版本退回。wrk、strace与固定运行库仅属于 [性能工具](benchmark/README.md) 的独立前提。

## 快速开始

Bash，先进入自己的仓库根目录；WSL使用Linux原生路径。下列命令按当前CMake与CLI静态核对，V1.0/S1未在干净环境重新执行。

```bash
mkdir -p .cache/local/tmp .cache/local/cache
export TMPDIR="$PWD/.cache/local/tmp" TMP="$PWD/.cache/local/tmp" TEMP="$PWD/.cache/local/tmp"
export XDG_CACHE_HOME="$PWD/.cache/local/cache" PYTHONDONTWRITEBYTECODE=1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j2
./build/hp_http_server --help
./build/hp_http_server --port 8080 --root ./www --threads 2
```

另一终端执行 `curl --http1.1 -i http://127.0.0.1:8080/`，应返回200与示例首页；不存在文件返回404，合法POST返回405。Ctrl-C或SIGTERM停止监听并排空当前输出，默认截止5000ms；再次观察到信号强关。阻塞stderr可能拖延日志join，HTTP截止不保证整个进程限时退出。

## 配置说明

必须显式提供 `--port <0-65535>` 和 `--root <directory>`；0端口由内核分配并在stdout报告。无配置文件；worker默认2、idle默认30000ms、keep-alive默认15000ms。`--access-log` 与 `--metrics-on-exit` 默认关闭；前者写有界JSON到stderr，后者退出时导出stdout文本快照，无监控HTTP路由。

完整CLI约束、Buffer/背压与观测计数口径见 [运行说明](documentation/RUNNING.md)。

## 测试与验证

```bash
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
```

当前CMake注册14项，含历史别名和synthetic工具测试；本轮未运行。历史S1的12/12、旧重构28/28均只代表各自时点，23个旧接口耦合目标仍冻结停用，不能充当当前全量回归。详见 [测试、smoke与格式化](documentation/DEVELOPMENT.md)。

## 项目结构

- `app/`：CLI、信号与HTTP连接适配，头/源沿用当前组织。
- `include/`、`src/`：按base/net/http/timer/metrics模块分离头文件与实现，保留现有布局。
- `tests/`：当前注册与冻结历史测试；`www/`：示例首页。
- `benchmark/`：独立实验工具、契约和公开结果；`documentation/`：使用与开发支持页；`history/`：历史信息。

## 已知限制

只支持无请求体GET；不支持body/chunked、Range、URL decode、TLS、HTTP/2、目录列表或代理。请求16KiB、请求行4KiB、文件8MiB上限。已有连接与输出边界，但无全局连接/内存配额或总请求时限，owner上的阻塞provider不能被定时器抢占。协议及资源边界见 [架构](ARCHITECTURE.md)。

性能数据限定WSL2同机loopback、closed-loop与热缓存。V0.6/S2矩阵提供条件化基线；S3只有M2小文件跟踪配对与M6未跟踪CPU，大文件syscall未知。strace单次QPS变化-95.85%为巨大观测扰动，不证明稳定瓶颈、恢复或长尾根因。RO-002、TD-001、TD-006保持开放；详见 [风险](TECH-DEBT-TRACKER.md) 与 [公开性能结果](benchmark/README.md)。

## 文档索引

- [架构](ARCHITECTURE.md)：现实现模块、线程、所有权和请求流。
- [路线图](ROADMAP.md)：已交付、搁置与未来阶段。
- [变更记录](CHANGELOG.md)：已验证的历史变化；[技术债](TECH-DEBT-TRACKER.md)：持续风险。
- [性能摘要](documentation/PERFORMANCE.md)：六档基线、历史修复及观测边界；[实验索引](benchmark/README.md)：方法与原结果。
- [项目展示](documentation/PRESENTATION.md)：讲解主线与Server简历条目；[面试讲解](documentation/INTERVIEW.md)：执行流、取舍与追问。
- [文档迁移记录](history/documentation/V1.0-S1/INDEX.md)：旧根文档及锚点去向。
- `AGENTS.md` 与 `docs/` 为本地协作资料，不随Git分发；公开使用与开发说明不依赖它们。

## 开发流程

Leader形成设计，批准后Builder实施并提供证据，Reviewer独立审查，允许关闭后Leader收口；用户自主合并。新功能、实验与目录迁移须单独批准。通用C++规则维护于个人Skill，项目只保留布局及工具差异。

## 许可证

仓库当前未包含 `LICENSE` 文件。

<details>
<summary>历史锚点导航</summary>

以下旧标题保留可追踪入口；内容为迁移前历史时点，不是当前执行指令。

<a id="技术主线"></a>
- [技术主线](history/documentation/V1.0-S1/README.md#技术主线)
<a id="c-格式化与提交检查"></a>
- [C++ 格式化与提交检查](history/documentation/V1.0-S1/README.md#c-格式化与提交检查)
<a id="异步日志"></a>
- [异步日志](history/documentation/V1.0-S1/README.md#异步日志)
<a id="buffer与既有背压"></a>
- [Buffer与既有背压](history/documentation/V1.0-S1/README.md#buffer与既有背压)
<a id="v06s1-指标与访问记录"></a>
- [V0.6/S1 指标与访问记录](history/documentation/V1.0-S1/README.md#v06s1-指标与访问记录)
<a id="历史版本验证记录"></a>
- [历史版本验证记录](history/documentation/V1.0-S1/README.md#历史版本验证记录)
<a id="v04s2-主从-reactor-与专项验证"></a>
- [V0.4/S2 主从 Reactor 与专项验证](history/documentation/V1.0-S1/README.md#v04s2-主从-reactor-与专项验证)
<a id="v04s1-线程原语与专项验证"></a>
- [V0.4/S1 线程原语与专项验证](history/documentation/V1.0-S1/README.md#v04s1-线程原语与专项验证)
<a id="v04s3-历史实现验证"></a>
- [V0.4/S3 历史实现验证](history/documentation/V1.0-S1/README.md#v04s3-历史实现验证)
<a id="v04s4-资源边界与关闭"></a>
- [V0.4/S4 资源边界与关闭](history/documentation/V1.0-S1/README.md#v04s4-资源边界与关闭)
<a id="v05s1-文件传输"></a>
- [V0.5/S1 文件传输](history/documentation/V1.0-S1/README.md#v05s1-文件传输)
<a id="固定版本压测入口"></a>
- [固定版本压测入口](history/documentation/V1.0-S1/README.md#固定版本压测入口)
<a id="v06s2-场景矩阵"></a>
- [V0.6/S2 场景矩阵](history/documentation/V1.0-S1/README.md#v06s2-场景矩阵)

</details>
