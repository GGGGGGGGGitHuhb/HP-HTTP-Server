# V0.5.1 / S3 固定性能回归入口

本工具比较不可变提交 C=`942f72cd9cea58e097025c3b9dd660f4132a1ffb`、D=`69424e6ab057bba2950c018e34c5695a4dc74f22`，独立 git archive 后构建 Release `-O3 -DNDEBUG`，无 sanitizer/LTO/native。正式源码不使用含用户注释的工作树。工具不下载依赖、重试失败或修改系统参数。

## 准备与正确性

从仓库根执行。Builder使用以下路径；Reviewer将本节所有`builder`替换为`reviewer`，独立导出，不复制Builder构建或结果。Debug必须放在各自D/source内，冻结benchmark测试要求其test-tmp位于导出REPO内。生成的Debug目录不是manifest固定源文件清单成员。

```bash
mkdir -p .cache/v0.5.1-s3/builder/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.5.1-s3/builder/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR" XDG_CACHE_HOME="$PWD/.cache/v0.5.1-s3/builder/cache"
export PYTHONDONTWRITEBYTECODE=1 HP_S3_TEST_TMP_ROOT="$TMPDIR"
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
python3 benchmark/regression/regression.py --role builder build
cmake -S .cache/v0.5.1-s3/builder/D/source -B .cache/v0.5.1-s3/builder/D/source/build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build .cache/v0.5.1-s3/builder/D/source/build-debug -j4
python3 benchmark/regression/regression.py --role builder unit --output .cache/v0.5.1-s3/builder/run-unit-001
python3 benchmark/regression/regression.py --role builder check --output .cache/v0.5.1-s3/builder/run-check-001
```

unit是单独Python工具反例（含真实自有后代进程，无socket）；check执行独立D的现有8项CTest，CMake自行设置build内test-tmp。普通构建和unit在沙箱；check及正式HTTP第一次按仓库规则窄提升。S2 sanitizer只能在核验D与`ab9b360c0d61d97a2b057d6a333ac88b02ece773`的app/src/include/tests/CMakeLists.txt完全一致后继承；本阶段不修改生产或测试。

## 完整矩阵

```bash
python3 benchmark/regression/regression.py --role builder run --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/builder/run-matrix-001
python3 benchmark/regression/collect.py --role builder --output benchmark/results/V0.5.1/S3/builder-001
```

|场景|字节|server workers|wrk threads|connections|
|---|---:|---:|---:|---:|
|P1|1024|0|1|1|
|P2|1024|2|2|32|
|P3|1024|4|2|128|
|P4|65536|2|2|32|
|P5|1048576|2|2|32|

每样本5s预热+20s测量，默认ACK、keepalive、热缓存。第一轮P1→P5，各C→D；第二轮P5→P1，各D→C；第三轮P3/P4/P5/P1/P2，各C→D，共30样本。完整schedule在第一个server之前保存。实际worker/wrk参数写入process及sample记录；超时沿用S2：idle30000ms、keepalive15000ms、shutdown5000ms、wrk2s，日志保持默认。

每版本/场景三轮QPS跨度`(max-min)/median <= 0.20`。P1/P2/P3要求QPS中位D/C≥10、P99中位D/C≤0.25；P4/P5分别≥0.90、≤1.25。完整零错误、正请求/字节下限、前后精确HTTP200/长度/hash/keepalive/尾字节审计及身份核验均必需。aggregate再次解析原summary字段并核对正有限QPS/P99、审计、回收和参数，不能靠`status=valid`跳过检查。

运行期间status预置invalid；只有完成且全部通过才valid并返回0。错误、漂移、noisy或数值失败返回非零，保留原始run/sample及observed_comparison，不写成功summary；不可删样、改门槛或自行重跑。失败样本在其目录sample.json保留，成功完成的样本同时列于run.json。新编号重测需Leader另行授权。

## 链路 smoke 与未来候选

```bash
python3 benchmark/regression/regression.py --role builder smoke --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/builder/run-smoke-001
```

smoke仅D/P2，1s预热+1s测量，`kind=smoke`且performance_acceptance为NOT_APPLICABLE_SMOKE，不是性能验收。完整矩阵已验证时无需重复运行smoke。

未来候选检查须在新的独立本地clone/workspace中，使用本入口源码且不复制旧.cache；在该工作区根首次执行`python3 benchmark/regression/regression.py --role builder build --ref <明确的本地ref>`。工具仅支持builder/reviewer两个固定角色根；本仓库该角色已有refs.json后不能再次build或切换候选。ref一次解析为完整commit并保存，后续运行读取固定值。C不变，D非本设计提交时kind为ad-hoc（smoke为ad-hoc-smoke），不产生正式PASS。已有构建禁止覆盖；独立工作区不构成重置本阶段1800s预算的授权，本阶段已有角色证据和累计账本必须保留，任何额外阶段运行仍须合并计费并由Leader决策。

## 预算、证据与限制

每角色动态累计1800s；正式每套900s，smoke60s，正确性/unit各预留180s。持久ledger在启动运行子进程前原子写入并持有独占锁，完成后按实际时间结算；失败同计，崩溃/未完成保留全部预留。递归识别历史run.json，改名/嵌套不重置消耗，损坏记录fail closed。新输出只能是角色根直接run-*子目录，禁止已有、符号链接、嵌套或越界路径。不要编辑ledger以重置预算。

阶段watchdog为duration+10s；整套/日志持续检查。启动前磁盘≥4GiB、MemAvailable≥1GiB、nofile≥1024；累计stdout/stderr≤2GiB，轮询边界并非内核硬配额。只终止持有PID/starttime的子进程；7s未退出才强杀，强杀或未回收使样本invalid。异常保留日志并清理本次fixture。

记录完整commit/tree/archive/source/binary/编译参数及工具/库hash，套前后比较忽略ASLR地址。CPU按单核100%，同时披露server/wrk CPU和RSS；结果只适用于本机WSL2 loopback、热缓存、闭环负载，计时响应不逐个审计body，不承诺物理机/公网容量。

公开收集器只复制原始记录、wrk stdout/stderr、审计/回收/进程参数和检查输出；大体积server stdout/stderr留本地，以原始字节数/hash建立索引。构建产物、依赖、私有docs和工作树注释不打包。原始wrk输出足够独立重算QPS/P99；资源时间线在run/sample记录中。

## S3 执行中的监督修复

F-S3-01：首次check/unit wrapper直接使用subprocess.run，不能证明异常后的孙进程回收，也缺少运行中日志/完整启动资源检查。修复仅涉及该入口及新增supervision.py/反例：独立session、process-local subreaper，跟踪PID/starttime/父子关系，识别另建session及被收养的后代；启动前资源检查、循环日志与超时守卫、7s TERM后必要KILL，回收证据写入checks.cleanup.json。预先存在的旁侧进程不属于本次Scope。原正式executor/矩阵/门槛未改变。

Builder首套run-matrix-001在14-P4-C停止：32次timeout且wrk duration54.488s与配置20s不符。全套UTC390.108s、monotonic355.725s；未证明具体环境或产品根因。原始invalid证据保留，不继承13个已完成样本。Leader授权账本增加34.383s保守计费调整，未覆盖原字段。任何新整套均须Leader明确授权并用新编号，不能单样本补齐。

F-S3-02：公开运行副本统一命名archive-run.json，避免未来候选导出源码里的历史公开结果被递归角色账本误计。真实角色run.json与ledger扫描不变。首次未发布builder-001公开副本已仅改名并保留迁移清单/字节hash，旧S1/S2公开命名本来不使用精确run.json。反例把含历史公开结果的目录复制到D/source，验证不新增计费；真实运行记录改名/嵌套仍计费。

当前Builder正式结果为两套invalid：001完成13样本、第14样本P4/C失败；Leader批准的002完成6样本、第7样本P4/C再次失败。两次timeout32且wrk时长不符，未获得30完整样本或性能通过结论，无第三套。详见[Builder最终阻塞摘要](../results/V0.5.1/S3/S3-builder-004.md)。
