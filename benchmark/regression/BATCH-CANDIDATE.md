# R005 日志批量修复候选

本入口落实已批准R005，候选E为固定D `69424e6ab057bba2950c018e34c5695a4dc74f22` 加精确日志补丁，不是新的Git提交。原固定C/D、旧工具、旧结果及旧累计账本保持。正式候选汇总明确为C/E，沿用原五场景/30样本/轮次/负载/门槛，不能冒称旧C/D通过。

运行前必须具备本地Approved R005及`.cache/v0.5.1-s3/leader/r005/authorization.json`。授权记录批准时各角色旧动态秒数和日志字节，补充额度各1800秒及2GiB；采用同一旧ledger继续记账，原剩余额度暂不消耗。守卫核对历史不倒退、增量上限、多时钟最大耗时、原资源条件和进程回收。新授权文件不是随意可改的测试参数，不能清账、改变root或删旧日志来扩大额度。

生产修改限定AsyncLogger头/实现，CMake仅新增测试注册；新增测试独立，不改冻结测试。freeze由Leader/Builder一次生成候选描述、完整patch和seal。角色build从Git独立导出并应用补丁，Release源码、配置、编译指令、二进制和依赖库在运行前后核验。网络工作区改动及学习注释不自动进入候选。debug/asan/tsan构建在独立候选source内部，与Release可变构建目录分开。

## 真实入口

在仓库根目录、原生WSL执行。tmp/cache分别指向`.cache/v0.5.1-s3/<role>/{tmp,cache}`，禁止压测同时构建/测试。freeze和已有构建不允许覆盖；以下build仅在角色尚未构建对应模式时运行。

```bash
export TMPDIR="$PWD/.cache/v0.5.1-s3/builder/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR" XDG_CACHE_HOME="$PWD/.cache/v0.5.1-s3/builder/cache"
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
python3 -B benchmark/regression/batch_candidate.py --role builder build --mode release
python3 -B benchmark/regression/batch_candidate.py --role builder build --mode debug
python3 -B benchmark/regression/batch_candidate.py --role builder check \
  --output .cache/v0.5.1-s3/builder/run-r005-correctness-001 -- \
  ctest --test-dir .cache/v0.5.1-s3/builder/E-r005/source/build-debug \
  --output-on-failure --timeout 60
```

correctness已实际执行9/9；新增adapter八项反例已通过。ASan/UBSan/LSan需按已知沙箱ptrace边界窄提升；TSan初始化受本环境memory mapping限制，失败保留，不能声明无数据竞争。不得改系统设置或取消有效检查伪造通过。

真实socket/HTTP及以下性能运行第一次申请窄提升。每角色一次smoke、至多一次matrix；已有同kind记录即拒绝再跑，无自动重试。

```bash
python3 -B benchmark/regression/batch_candidate.py --role builder smoke \
  --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk \
  --output .cache/v0.5.1-s3/builder/run-r005-smoke-001
python3 -B benchmark/regression/batch_candidate.py --role builder matrix \
  --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk \
  --output .cache/v0.5.1-s3/builder/run-r005-matrix-001
```

性能命令仅为批准入口，在对应run.json实际完成前不能声称已执行。smoke不判性能PASS。matrix的status表示证据有效性，performance_acceptance单独为PASS/FAIL；性能FAIL或invalid均返回非零。记录所有样本与失败、schedule、原始wrk输出、精确HTTP审计及回收；守卫最终失败移除性能结论，不把中途结果作为成功。客户端wrk直方图校正、WSL同机负载、热缓存和旧计时异常限制保持。

Reviewer将角色改为reviewer，使用自己的独立构建和结果；Builder矩阵失败后只读复算及Correctness独立复核，不自动追加矩阵追求通过。最终审查按R005与原S3要求给唯一结论。
