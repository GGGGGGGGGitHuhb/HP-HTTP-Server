# V0.6/S3 有限性能分析

独立薄入口只读各角色S2固定1340f5b artifact/source/ELF/compiler/wrk/库，免build、免安装。只采M2/M6（1KiB/1MiB，2workers、wrk2/32，keepalive）各untraced再traced，共4条，各1s预热+5s测量。不是S2基线追加、函数CPU热点或长尾定位；没有性能门槛。

Linux/WSL仓库根，角色目录必须已存在；准备者只建tmp/cache。测试driver首次拥有budget/fastchecks，probe/formal driver拥有未来probe/suite/payload/sample；旧存在输出拒绝不覆盖。真实socket/ptrace第一次最小提升仅自有PID：

```bash
mkdir -p .cache/v0.6-s3/builder/rework-001/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.6-s3/builder/rework-001/tmp" TMP="$TMPDIR" TEMP="$TMPDIR"
export XDG_CACHE_HOME="$PWD/.cache/v0.6-s3/builder/rework-001/cache" PYTHONDONTWRITEBYTECODE=1
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
python3 tests/PerformanceAnalysis_test.py --root .cache/v0.6-s3/builder/rework-001
python3 benchmark/analysis/Run.py --root .cache/v0.6-s3/builder/rework-001 --mode probe
python3 benchmark/analysis/Run.py --root .cache/v0.6-s3/builder/rework-001 --mode formal
python3 benchmark/analysis/Recompute.py --suite .cache/v0.6-s3/builder/rework-001/suite/result.json --output .cache/v0.6-s3/builder/rework-001/recompute.json
```

每角色唯一180s绝对窗口由首次fastcheck开始，fast≤30/probe≤20/formal≤120共享剩额并含10s cleanup；期间等待也扣剩额，不暂停/重试。probe只一次微小HTTP自有server附着，能力失败即停止4条NotRun，不改系统设置/ptrace策略、不换工具。新task256MiB/raw64MiB按logical/allocated较大值及stdout/stderr/log，每秒检测式计量，可能overshoot则保存失败。旧S2artifact只读不复制不重计，旧ledger不消费。

进程所有者以PID/starttime+自有process group控制TERM/KILL/wait4。R001 traced改为`strace -f -c -o <summary> <fixed server argv>`父启动，不使用-p/-D/sysctl/prctl/sudo。核wrapper直接child与实际server的PPid/TracerPid/exe/argv/starttime，监听端口属于server；CPU来自server，不能用wrapper代替。控制器仅对wrapper直接wait4，server由wrapper回收，保留终结/消失证明，不伪称非child直接reaped。

schema2 trace_scope=server_lifecycle：summary包含初始化、pre/post审计、预热、measurement与正常退出，默认system time不是wall/函数/用户态CPU，calls/time不能机械除5s测量请求。保存launch/确认/ready/warmup/measurement/postAudit/TERM/wrapperWait/traceeDisappear clock。正常TERM实际server→wrapper自然summary退出→控制器reap wrapper→核tracee/thread消失与端口关闭；异常仅清理已确认自有身份树，forced/无法证明终结使invalid。首次sibling attach EPERM及旧180s账保留，R001只有一次新180s窗口，空间按旧+新累计。Reviewer用自己原未用180s与原role根。

thread before/after保存/proc/PID/task/TID/stat真实utime/stime ticks、Hz、PID/TID/starttime、comm和逐线程读取clock；CPU除各自read_after-read_before。进程总CPU与client wait4沿S2 R002原始输入/包围wall，读取边界略有偏差，不强行让线程和等于总CPU。只有主TID=PID可明确main reactor；其他comm未区分worker/logger，角色未知，不按CPU排名猜。固定源码EventLoopThreadPool创建2workers、AsyncLogger消费者另有thread只解释结构，不绑定未知TID。

schema1 suite保存4互斥valid/invalid/NotRun、HTTP pre/post各3正确body/header/SHA、五wrk errors0、S1最终active0/总账、线程和进程CPU原值/原stdout、完整生命周期trace摘要及身份树回收。独立消费者不import分析Run公式，逐thread ticks/clock重算、复用S2已验证独立CPU消费者；按stderr独立核syscall calls/errors/system seconds、排序和舍入百分比。配对只单次观测扰动，不做显著性结论或混S2三轮分位。

新performance_analysis_tests CTest为更小功能/合成子集；上述--root fastchecks含真实Run dispatch→server/HTTP/finally第三synthetic失败2/1/1、probe拒绝不创建suite。mock负载与trace明确未采，不是正式槽。各角色使用自己目录/旧artifact，Reviewer独立。公开报告区分测量观察、固定源码机制、未证实假设，WSL2 loopback/closed-loop/hot-cache、MiB/s含headers、校正分布人口未采、P99不合并与RSS sampled-max限制保持；RO-002/TD-001/TD-006和V0.5.1未完成不关闭。

## R002最后有限范围

用户已批准仅三样本AC：M2 untraced/traced、M6 untraced；系统调用/单次扰动仅支持M2。旧R001四套保持3valid/1invalid/0NotRun，第4M6 traced缺after CPU/cleanup/最终metrics不补造；不能称大文件syscall已验证。

Builder不再probe/formal/build/wrk/trace重采，仅最后唯一60s fast（≤50+cleanup10）与旧前三只读复算：

```bash
python3 tests/PerformanceAnalysis_test.py --root .cache/v0.6-s3/builder/rework-002
python3 benchmark/analysis/Recompute.py --suite .cache/v0.6-s3/builder/rework-001/suite/result.json --selection r001-valid-three --output .cache/v0.6-s3/builder/rework-002/selected-recompute.json
```

该Builder新根的tmp/cache用于上述环境；累计空间含原两轮，旧账只读。S3本地load_phase在任何已创建child路径finally幂等close/persist，cached单wait4不再次回收；after缺失记录null/error/invalid，不生成CPU百分比。快检以真实小Python child验证已poll退出与仍活两种异常，保存exit/usage/identity/wait/cleanup clock；有限HTTP synthetic第三失败按新3计划2/1/0、历史4计划2/1/1各自核验，没有正式负载。Reviewer仍用独立原root/180s，只probe与新三样本。derived结果明确source_counts/selection/omitted_invalid及source hashes，旧suite并未通过；无函数栈/大文件syscall/长尾根因结论。
