# HP HTTP Server

Linux C++20 HTTP 服务器学习项目：提供受限 HTTP/1.1 GET/HEAD 静态服务、POST /api/echo 二进制回显、有界Content-Length/chunked请求正文和Expect: 100-continue、完整请求读取期限、Keep-Alive、顺序流水线请求、非阻塞 sendfile、连接超时与优雅关闭。网络层采用 epoll Reactor；默认主线程监听、两个 worker 处理连接，支持单 Reactor 模式。

## 后续方向

2026-10-29调整：取消本仓库原L7网关扩展，后续依次规划V1.1请求正文与基础路由、V1.2 HTTP兼容与静态资源语义、V1.3无数据库动态接口、V1.4运行保障。V1.1/S1协议与回归基线已完成独立验收并合并，S2正文与读取期限已完成独立验收，S3方法路由已完成独立验收，V1.1三个阶段均已验收完成，S3已合并origin/main（标签发布未核实）；V1.2/S1已完成独立验收并合并，S2目标解析已独立验收（Approved revision1及补充，Reviewer001 PASS），S3条件请求与单区间下载已独立复审PASS，V1.2三个阶段均已验收完成；S3已合并（本阶段起点PR40），发布标签未核实；V1.3/S1内存任务API已独立验收PASS，S2已独立复审PASS且合并、S3已独立PASS，V1.3阶段验收完成（S3未合并、标签未发布）；当前仍是受限HTTP服务，详见[路线图](ROADMAP.md#后续路线与共同门槛)。数据库、代理及推理网关不在本轮范围。

## 环境要求

Linux / WSL2，支持 C++20 的 GCC 或 Clang、CMake ≥3.20、线程库；默认 `BUILD_TESTING=ON` 还需要 Python 3。curl 用于手工请求，Bash 用于 smoke。核心服务不依赖数据库、Web框架或第三方HTTP运行时；任务JSON使用随源码保存的nlohmann/json v3.12.0（MIT），无需安装、构建不联网。当前CMake项目版本仍为 `0.1.0`，CLI启动文字仍保留 `V0.1 / S3`，不表示发布版本退回。wrk、strace与固定运行库仅属于 [性能工具](benchmark/README.md) 的独立前提。

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

另一终端执行 `curl --http1.1 -i http://127.0.0.1:8080/`，应返回200与示例首页；不存在静态文件GET/HEAD返回404；POST /api/echo返回原始正文，POST其他路径返回405。Ctrl-C或SIGTERM停止监听并排空当前输出，默认截止5000ms；再次观察到信号强关。阻塞stderr可能拖延日志join，HTTP截止不保证整个进程限时退出。

## 配置说明

必须显式提供 `--port <0-65535>` 和 `--root <directory>`；0端口由内核分配并在stdout报告。无配置文件；worker默认2、idle默认30000ms、keep-alive默认15000ms；`--request-read-timeout-ms`默认10000ms，0禁用，当前请求首个非空输入起绝对计时，进度不续期，到期静默关闭。`--access-log` 与 `--metrics-on-exit` 默认关闭；前者写有界JSON到stderr，后者退出时导出stdout文本快照，无监控HTTP路由。

完整CLI约束、Buffer/背压与观测计数口径见 [运行说明](documentation/RUNNING.md)。

## 测试与验证

```bash
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
```

当前CMake注册34项，含已验收S3静态语义两套及目标解析两套及已验收chunked/Expect两套、历史别名和synthetic工具测试；历史V1.1/S3增加方法路由C++与真实HTTP两套，Builder及Reviewer独立通过Debug/Release各20/20、规定sanitizer 8/8，Reviewer结论PASS；错误预期和失败清理反证有效。当前行为见[HTTP支持矩阵](documentation/HTTP-SUPPORT.md)，测试入口与冻结覆盖边界见[开发说明](documentation/DEVELOPMENT.md)。23个旧目标仍停用；S3仅定向更新两份旧测试的静态405 Allow断言，其他冻结约束保持。

## 项目结构

- `app/`：CLI、信号与HTTP连接适配，头/源沿用当前组织。
- `include/`、`src/`：按base/net/http/timer/metrics模块分离头文件与实现，保留现有布局。
- `tests/`：当前注册与冻结历史测试；`www/`：示例首页。
- `benchmark/`：独立实验工具、契约和公开结果；`documentation/`：使用与开发支持页；`history/`：历史信息。

## 已知限制

当前GET完整消费固定长度或chunked解码正文但不解释其业务含义，文件响应仍由路径决定；HEAD返回同GET表示头但不发送正文；POST /api/echo回显原始正文，其他资源方法组合405。请求头16KiB、行4KiB、正文1MiB、文件8MiB独立上限，合法正文超限413，非法framing400；仅支持单个chunked编码及100-continue期望，trailer仅X-Checksum/X-Trace-Id；不支持响应chunked、multipart多Range、Unicode规范化、TLS、HTTP/2、目录列表或代理。生产输入窗口仍16KiB，不是正文总上限。无全局连接/内存配额；任务API已有1秒业务期限，owner上的同步provider仍不能被定时器抢占。协议及资源边界见 [架构](ARCHITECTURE.md)。

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

V1.2/S1已交付有界chunk解码和显式100中间响应，沿原路由处理解码body。新两套测试与现有20项共同组成22项CTest；独立Reviewer PASS：Debug/Release各22/22、规定sanitizer10/10，负对照及失败清理有效。TE/Expect分类、限额及早拒绝语义见[HTTP支持矩阵](documentation/HTTP-SUPPORT.md#v12s1-chunked与expect已验收)。

V1.2/S2已验收统一目标一次percent解码：`/space%20name.txt`访问编码文件名，`/%61pi/echo`匹配同一echo路由；query解析为有序拥有型参数但本阶段业务不使用。目标结构错误403关闭，合法目标的文件symlink/权限403仍可保活；请求目标及查询限额见[运行说明](documentation/RUNNING.md#v12s2-请求目标与参数已验收)。独立Debug/Release各24/24、规定sanitizer12/12通过；V1.2整体仍未完成。


### V1.2/S3 条件请求与单区间（已独立验收）

静态GET/HEAD提供Date、元数据弱ETag与Last-Modified；If-None-Match优先If-Modified-Since，命中304无正文且无Content-Length。GET支持单bytes Range，206只发送指定区间；合法不可满足返回416且可保活。HEAD忽略Range，任意If-Range均保守回完整200，条件304仍优先；不宣称安全断点拼接。多Range/畸形Range忽略，未缓存文件内容或建立文件快照。细节及curl例见[运行说明](documentation/RUNNING.md#v12s3-条件请求与单区间已独立验收)，独立复审PASS：Debug/Release各26/26、规定sanitizer14/14；首次RFC850完整时间50年边界FAIL及修复记录保留。


### V1.3/S1 内存任务API（已独立验收）

新增`GET/POST /api/tasks`和`GET/PUT/DELETE /api/tasks/{id}`。POST/PUT必须提供JSON对象`{"title":"demo","completed":false}`及Content-Type: application/json；PUT完整替换，ID为字符串，最多128条，重启清空。任务命名空间优先同名静态文件，HEAD返回405无正文，其他/API路径和echo保持原规则。所有任务JSON响应带唯一no-store，错误与连接策略见[运行说明](documentation/RUNNING.md#v13s1-内存任务api已独立验收)。无数据库、鉴权、持久化、PATCH或异步业务线程池；V1.3未验收完成。

## V1.3/S2 异步任务（已独立验收）

生产入口将完整任务请求交给固定2线程执行器：最多排队32个，包含运行和待领取结果共34槽。静态文件与echo继续由连接owner处理；原无异步注入的factory保持同步兼容。任务从准入到owner交付最多1000ms；过载或超时返回JSON 503并关闭连接。输入验证、100 Continue与请求读取期限仍在owner。

新增`bounded_task_executor_tests`、`http_async_task_http_tests`，CTest共30项；详细行为与退出限制见[运行说明](documentation/RUNNING.md)。S2已独立验收，未宣称性能提升。

V1.3/S2经独立Reviewer002复审PASS：Debug30/30（29.17秒）、Release30/30（28.76秒）、规定ASan+UBSan/LSan18/18（22.85秒）；三负对照-6/-6/1、16文件格式通过。55旧测试及31冻结保持；启动异常夹具自清理缺陷已修复，原FAIL与初URL失败保留，累计纠正2/2，无新债。S2已完成，S3未开始，V1.3整体未完成；验收不表示合并或标签发布。

## V1.3/S3 综合应用验收（已独立验收）

新增有界并发与真实HTTP组合两套测试，保留原30项注册，当前共32项。提供[一键演示与curl示例](documentation/DEMO.md)，展示静态文件、echo、任务CRUD和重启清空；演示只用临时合成数据，监听0.0.0.0临时端口，客户端连127.0.0.1，须在可信开发环境运行。S3已独立验收PASS，V1.3阶段验收完成，尚未合并或标签发布。

Builder规定Debug/Release各32项及ASan+UBSan/LSan20项通过；TSan初次初始化因unexpected memory mapping失败，随后同二进制在限定进程布局路线下3/3通过。原失败保留，S3已独立验收PASS，详见[验证路线与限制](documentation/DEVELOPMENT.md#v13s3-综合应用验收候选)。

### V1.3/S3 最终验收检查点

2026-10-31 V1.3/S3独立Reviewer001 PASS：最终候选Debug32/32（31.00秒）、Release32/32（31.31秒）、ASan+UBSan/LSan20/20（25.36秒）、限定setarch进程布局下TSan3/3（1.26秒）；原裸跑0/3初始化失败保留。三负对照-6/-6/1、一键演示exit0，45进程/12故障/18个FINAL资源回收通过。累计纠正2/2，无新增债务；S1–S3阶段验收全部完成，不表示已合并或标签发布，V1.4未开始。

TSan仅改变指定测试进程树地址随机化属性，不改系统全局配置；有限检查不证明穷举无race。保留全接口监听/可信环境、超时不回滚、join可能等待、TD006/WSL2/长尾/EOF风险。


## V1.4/S1 共享逻辑资源预算（实现候选）

生产main现在向所有网络worker注入同一预算：最多128个已准入连接，64MiB输入逻辑预留，64MiB待发送输出。每个连接先保留16KiB传输窗口，每个活跃请求另保留1040KiB；小请求也整额预留，128连接同时存在时最多61个活跃请求。连接、输入或输出预算不足时只关闭相关连接（客户端见EOF/reset），不额外构造可能再次超额的错误响应；已有业务队列过载仍为503。

输出配额包括尚未发送的文件剩余字节，实际send/sendfile成功即归还；这限制并行下载工作量，不代表文件内容全部驻留内存。预算不是RSS硬限：Buffer容量、请求副本、JSON临时、线程栈、内核socket和文件缓存等不在其内。资源表、9项退出指标及兼容边界见[运行说明](documentation/RUNNING.md#v14s1-共享逻辑资源预算)。新增两套测试，原32目标和旧测试不改；本阶段仍待独立验收，V1.4整体未完成。

S1首轮Builder验证中，Debug34/34、ASan+UBSan/LSan22/22、指定TSan4/4通过，Release33/34：未改动的旧task_application_concurrency_tests在ready等待后检查outstanding处失败。原反例保留。补充001批准仅修旧测试等待helper及两调用，并新增独立门闩回归：受控ready32/running1/outstanding33后等待退槽至32，保留原容量断言。修后Builder完整Debug/Release各34/34、ASan22/22、既定setarch TSan4/4各一次通过；现已独立复审PASS，最终分层证据见下方收口检查点。

### V1.4/S1 收口检查点

2026-10-31 V1.4/S1独立Reviewer002 PASS，累计纠正2/2，无新增债务。r3独立Debug34/34（34.38秒）、Release34/34（33.13秒）、ASan+UBSan/LSan22/22（27.66秒）、限定进程布局TSan4/4（1.36秒）及三负对照通过；r4只改新Python指标消费者，修后三配置HTTP各1/1（2.11/2.09/2.39秒）、同原真实管道反例及HTTP负对照通过，58进程正常清理。未声称r4重跑全量；Debug构建摘要有warning_present，未声称全零告警。原Release33/34与P2 FAIL保留。S1已完成，S2/S3未开始，V1.4整体未完成；验收不代表合并或标签发布。

128连接、64MiB输入预留/待发输出逻辑预算及EOF/reset过载策略保持；文件剩余字节不是RSS，旧16桶/原指标含义不变。TD006、WSL2/长尾/EOF风险及原P3不关闭或顺手修改。

## V1.4/S2 配置与独立管理（已验收）

`--config examples/server-config.json`读取严格有界JSON；默认值<文件<CLI，各来源独立校验。现有CLI兼容，增加资源预算参数与布尔反向开关。`--admin-port 0`启用实际IPv4回环管理服务，stdout报告端口；提供healthz/readyz/metrics，独立owner/预算，不改变业务路由或计数。默认关闭、无鉴权，仅可信本机诊断；详细字段与命令见[配置说明](documentation/CONFIGURATION.md)。

管理退出先于业务/logger最终join。日志仍异步stderr，轮转交外部接收者；rename不重开、copytruncate可能丢行/重复，网络期限不能保证阻塞sink的进程退出期限。当前新增运维两suite，CTest注册36项。Builder最终Debug/Release各36/36、ASan24/24、既定setarch TSan5/5及三负对照通过，现已独立PASS；首次help兼容标识遗漏导致Debug35/36及修正历史保留，不宣称V1.4整体完成。

### V1.4/S2 收口检查点

2026-10-31 V1.4/S2独立Reviewer001 PASS：最终r2候选Debug36/36（35.36秒）、Release36/36（34.82秒）、ASan+UBSan/LSan24/24（31.13秒）、限定setarch进程布局TSan5/5（3.49秒）；负对照-6/-6/1、14份C++格式通过，32份长寿命fixture、12个故障和4个临时root正常清理。63旧tests及31冻结保持。当前纠正1/2，无新增债务；原JSON include/help失败历史保留。S2已完成、S3未开始，V1.4整体未完成；阶段验收不表示已合并或标签发布。

管理默认关闭、实际回环绑定且无鉴权，readiness不保证容量；独立逻辑预算不等于全局RSS。日志由外部接收者轮转，网络drain期限不等于整个进程退出期限；不合作业务或sink仍可能延长join。TD006/WSL2/EOF/长尾及S1 warning/P3保持。
