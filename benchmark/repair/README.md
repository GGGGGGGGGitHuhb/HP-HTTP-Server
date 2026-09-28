# V0.5.1 / S2 TCP_NODELAY 修复验证

生产策略为在Acceptor交付新连接之前设置TCP_NODELAY；失败只关闭该局部Socket，继续接受下一连接。文件响应仍使用sendfile。本入口按批准范围比较固定C与未提交候选D，旧S1工具、结果和冻结测试保持原样。

## 身份与独立目录

C固定为 `942f72cd9cea58e097025c3b9dd660f4132a1ffb`。D显式标记 `uncommitted-workspace-snapshot`，`commit/tree=null`、`base_commit`指向C；绝不将工作区快照当成Git提交。

构建器从C的git archive取得基础，再覆盖工作区 `app/src/include/tests/CMakeLists.txt` 的完整实际字节及两个新增测试，确定性生成D archive。候选包含用户已有学习注释；它们是身份的一部分，不等同本次产品逻辑改动。每套前后重新生成该快照并校验源、工具、wrk/库、编译参数、manifest及二进制身份。候选冻结后不可修改这些文件再沿用旧manifest。

Reviewer使用独立角色目录、独立导出/构建；先核对自己生成的 `candidate-spec.json` archive SHA256和workspace_hashes与Builder候选相同，并检查实际差异，再执行测量。不得复制Builder可变build或原始结果作为复现。

Builder提供 `.cache/v0.5.1-s2/builder/production-only.patch`，这是对阶段起点的三生产文件补丁，Acceptor不夹带用户注释；发布由Leader处理，不能直接暂存整份含用户注释的Acceptor.cpp。实际工作区注释保持原文和语义位置。

## 本地准备与正确性

从仓库根执行。以下命令属于Builder；Reviewer使用下一节的独立候选快照命令，不能仅替换角色名后继续从共享仓库 `-S .` 构建。普通构建在沙箱，socket/HTTP/strace首次按仓库规定窄提升。固定wrk及库复用历史已校验缓存，无安装或下载。

```bash
mkdir -p .cache/v0.5.1-s2/builder/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.5.1-s2/builder/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR" XDG_CACHE_HOME="$PWD/.cache/v0.5.1-s2/builder/cache"
export PYTHONDONTWRITEBYTECODE=1 HP_S3_TEST_TMP_ROOT="$TMPDIR"
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
cmake -S . -B .cache/v0.5.1-s2/builder/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build .cache/v0.5.1-s2/builder/debug -j4
ctest --test-dir .cache/v0.5.1-s2/builder/debug --output-on-failure --timeout 60
cmake -S . -B .cache/v0.5.1-s2/builder/sanitizer -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build .cache/v0.5.1-s2/builder/sanitizer --target tcp_nodelay_tests -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .cache/v0.5.1-s2/builder/sanitizer/tcp_nodelay_tests
python3 benchmark/repair/test_repair.py
```

当前CTest为原6项加2项。新C++专项使用始终执行的require，Release也不会丢失connect/poll等操作；验证true/false读回、无效fd/errno、callback前已设置、单个已接受fd的setsockopt失败隔离/关闭一次、后续健康连接及小发送缓冲真实EAGAIN/完整恢复。故障包装仅在测试目标链接，生产没有开关。黑盒覆盖threads0/2、1KiB/1MiB每连接10次keepalive、空文件/8MiB、Connection close、FIN/RST与SIGTERM。

## Reviewer独立候选构建路线

2026-09-28路线勘误：冻结的benchmark_runner_tests从自身文件位置确定REPO（独立运行时是D/source），并要求其临时输出留在该REPO内。因此Reviewer的Debug和sanitizer构建目录必须放在D/source内部；外层reviewer/debug会让CTest配置的test-tmp落在REPO外。保留首次路线失败，纠正目录即可，不修改冻结测试或放宽输出约束。

以下仍从共享仓库根发出命令，但CMake源码明确指向Reviewer独立导出的D/source。首次运行先独立导出/Release构建C/D；若已经完成该步骤，核对candidate-spec后直接继续Debug命令，不重复创建已有构建目录。

```bash
mkdir -p .cache/v0.5.1-s2/reviewer/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.5.1-s2/reviewer/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR" XDG_CACHE_HOME="$PWD/.cache/v0.5.1-s2/reviewer/cache"
export PYTHONDONTWRITEBYTECODE=1 HP_S3_TEST_TMP_ROOT="$TMPDIR"
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
python3 benchmark/repair/repair.py --role reviewer build
cmake -S .cache/v0.5.1-s2/reviewer/D/source -B .cache/v0.5.1-s2/reviewer/D/source/build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build .cache/v0.5.1-s2/reviewer/D/source/build-debug -j4
ctest --test-dir .cache/v0.5.1-s2/reviewer/D/source/build-debug --output-on-failure --timeout 60
cmake -S .cache/v0.5.1-s2/reviewer/D/source -B .cache/v0.5.1-s2/reviewer/D/source/build-sanitizer -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build .cache/v0.5.1-s2/reviewer/D/source/build-sanitizer --target tcp_nodelay_tests -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .cache/v0.5.1-s2/reviewer/D/source/build-sanitizer/tcp_nodelay_tests
python3 benchmark/repair/test_repair.py
```

CTest仍由CMake把HP_S3_TEST_TMP_ROOT设为各自build目录内的test-tmp；不要覆盖为外层路径。候选源码身份/注释保护以导出时manifest和candidate-spec的固定源文件清单核对，不把后续生成的build-debug、build-sanitizer及test-tmp纳入源码身份或保护清单；原有受保护源文件仍须逐项核验。两类构建均与正式C/D Release目录分开。

## 正式与机制验证

```bash
python3 benchmark/repair/repair.py --role builder build
python3 benchmark/repair/repair.py --role builder run --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s2/builder/run-CD-001
python3 benchmark/repair/mechanism.py --role builder --label C --output .cache/v0.5.1-s2/builder/run-timeline-C
python3 benchmark/repair/mechanism.py --role builder --label D --output .cache/v0.5.1-s2/builder/run-timeline-D
python3 benchmark/repair/collect.py --role builder --output benchmark/results/V0.5.1/S2/builder-001
```

目录必须新建；失败保留，不自动重试。先通过全部正确性、sanitizer和工具负例，再执行固定12正式样本；期间禁止其他构建/测试/压测。奇数轮C→D、偶数轮D→C，文件顺序交替；每样本warmup5s+measurement20s、2workers、wrk -t2 -c32 --timeout 2s --latency。默认客户端ACK，Release -O3 -DNDEBUG，无sanitizer/LTO/native。

成功要求每组三轮跨度≤20%，1KiB QPS D/C≥10、P99 D/C≤0.25；1MiB QPS D/C≥0.90、P99 D/C≤1.25，全部错误零及前后HTTP审计通过。noisy或门槛未达即整套invalid/非零，原始样本和observed_comparison保留作诊断，不生成成功summary，也不筛选重试。

每套运行中status默认invalid；只有全部采样、身份和门槛通过才valid，以结束时间/error/退出码判断最终结果。逐样本保存QPS、P99、CPU/RSS、日志量、五类错误和回收。计时body不逐个审计，WSL2同机loopback/热缓存不能作为物理机或公网容量承诺。

默认客户端机制C/D各一组，每组三连接×32请求，逐body校验；D各连接正文等待中位数<5ms。每组≤10s，最多两组；跟踪与无插桩QPS分开，未设置QUICKACK。只对自启动server跟踪；Yama=1时仅tracee在exec前允许同用户ptracer，系统配置不变。

每角色真实压测/机制累计≤1800s（所有失败也计入run-*/run.json），每套≤600s、阶段≤duration+10s。累计stdout/stderr/trace≤2GiB；启动free disk≥4GiB、MemAvailable≥1GiB、nofile≥256。预算为轮询，不是硬配额。终止只针对所拥有PID/starttime，7s后必要强杀会使整套invalid；fixture由本次拥有者清理，日志保留。

## 当前结果与账本纠错

[Builder S2结果](../results/V0.5.1/S2/S2-builder-001.md)列出正式四门槛、默认客户端机制、CPU/RSS代价及历史失败。最终工具负例为40项。F-S2-01修复后，输出严格要求角色根的直接子目录，时间账本递归计入历史run.json，机制按实际trace/experiment记录（包括失败）计两组，不依赖目录名称。实际正式套在修复前的合法直属路径执行，保留原工具身份与明确继承依据，未自动重跑。
