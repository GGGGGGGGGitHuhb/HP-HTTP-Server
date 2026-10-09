# V1.0 候选发布检查

## 当前结论

2026-10-09：Builder003完整通过，Reviewer006独立**PASS**，末端实际结算条件已核实。V1.0/S1–S3主线阶段验收完成，合并及标签发布另行核实；V1.1不自动启动。

[Builder结果](../benchmark/results/V1.0/S3/builder-rework-003.md) · [Reviewer结果](../benchmark/results/V1.0/S3/reviewer-rework-005.md) · [独立机器证据](../benchmark/results/V1.0/S3/reviewer-rework-005.json) · [实际终点与费用](../benchmark/results/V1.0/S3/leader-close.json) · [性能摘要](PERFORMANCE.md)

固定候选base `20bd03142b4c828a7e939c30b501812da2bd5440` + 四测试文件补丁SHA `6ad80910acf90acc1c76ad3fefb4034864d521ae282f18500c30731acfd52b83`；独立581文件身份与工具/Lua前后保持。三份活跃测试的94个assert条件改用Release仍求值的检查，产品/CMake/NDEBUG不改。原31冻结测试及23停用目标保持；当前14项含HTTP黑盒别名，不能当作历史全部测试覆盖。

两角色各自完整通过Debug/Release14×2、四HTTP冒烟（workers0/2）、CLI与README www/200/404/405、SIGINT/SIGTERM、checker、五ASan/UBSan/LSan（detect_leaks=1）、唯一M2准入、三档三轮九个正式样本及最终公开后验；不拼接失败批子项。每个正式样本前后各三个同socket响应审计，五类客户端测量错误零；服务端指标单独记录，不与客户端错误混为同一口径。

Reviewer完整连续563.570218637秒，最后实际扫描总184,156,160B/capture1,482,752B、peak总559,263,744B；scan耗时/startgap/completegap最大0.065116255/0.419322277/0.430212551秒。Builder完整连续389.554457999秒，末scan总211,488,768B/capture1,609,728B；两角色owned0/forced0。公开角色报告是生成时快照，上述实际终点由母进程stdout独立消费，另存Leader账，不修改冻结角色输出。原失败/诊断/等待及新增额度分别保留。

历史164迁移锚点、五快照/十学习源码保护SHA和独立公共分发/格式通过。RO-002/TD-001/TD-006继续Open，M3高并发长尾未关闭；WSL2有限负载不证明物理机容量/多核线性扩展，本链未采CPU/RSS。原M6 EOF原因未知，本次完整验收通过不等于证明历史根因已修复。

## 历史批次

以下按各批次当时状态保留，失败和未执行事实不覆写；当前结论以上节为准。

2026-10-09 R004：[Reviewer失败结果](../benchmark/results/V1.0/S3/reviewer-rework-004.md)。治理调度/checker/成功与失败终点检查通过；Debug/Release各14/14，但Release文件身份后验时触及功能阶段累计600秒上限。四smoke/CLI/五san/perf/package均NotRun；不能拼接旧批次来通过。实际连续743.805646360秒，terminal_complete=true、首错phase deadline、其余错误为空、owned0。末次实扫总101,138,432B/capture1,187,840B，全部自有资源正常回收。当时独立审查FAIL、V1.0未完成；Builder完整通过证据保留。后续按批准R005修正等价扫描实现并完整重验。

[当前R003 Builder结果](../benchmark/results/V1.0/S3/builder-rework-003.md) · [原R001失败](../benchmark/results/V1.0/S3/builder-rework-001.md) · [首次失败](../benchmark/results/V1.0/S3/builder-run-001.md)

2026-10-09 R003：Builder完整动态流程通过，固定base与四文件补丁不变，正式9 valid/0 invalid/0 NotRun。[Reviewer独立审查FAIL](../benchmark/results/V1.0/S3/reviewer-rework-003.md)：Debug/Release各14/14、四smoke/CLI/checker/五san通过，但M2准入测量命令返回后实际扫描开始间隔1.054941209秒超出1秒门槛，post_audit和九正式未执行；自有资源全部正常回收。这次失败不是HTTP EOF。S3/V1.0仍未关闭；旧失败与费用保留，未发布、提交或打标签。当前14/23冻结覆盖及RO-002/TD-001/TD-006边界不变。

## 已保留的历史执行与命令说明


[项目入口](../README.md) · [性能摘要](PERFORMANCE.md) · [当前执行结果](../benchmark/results/V1.0/S3/builder-rework-001.md) · [首次失败](../benchmark/results/V1.0/S3/builder-run-001.md)

当前候选为base `20bd03142b4c828a7e939c30b501812da2bd5440` 加四文件批准测试补丁SHA `6ad80910acf90acc1c76ad3fefb4034864d521ae282f18500c30731acfd52b83`。2026-10-09 Builder R001完整功能回归通过，但第4正式压测样本的请求前审计失败；**最终验收未通过，V1.0尚未完成**。原Release测试失败及费用完整保留，这页不是发布宣告。

## 已执行的构建与功能验证

从候选 `git archive` 导出，构建放在独立导出源码根内，`TMPDIR/TMP/TEMP/XDG_CACHE_HOME` 明确指向本批临时目录与缓存，禁止用原工作树或旧二进制代替被测候选。下列是本次实际命令形式，不授权自动重跑失败批次：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure -j1
HP_S3_TEST_TMP_ROOT="$PWD/.validation/smoke" HP_HTTP_TEST_THREADS=0 \
  bash tests/http_smoke_test.sh "$PWD/build/hp_http_server"
HP_S3_TEST_TMP_ROOT="$PWD/.validation/smoke" HP_HTTP_TEST_THREADS=2 \
  bash tests/http_smoke_test.sh "$PWD/build/hp_http_server"
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG'
cmake --build build-release -j2
ctest --test-dir build-release --output-on-failure -j1
```

初次候选Debug14/14、Release12/14：两个活跃测试把socketpair调用放在assert内，NDEBUG删除调用并继续使用未初始化pair。R001仅修改R6Callbacks/HttpObservability/ServerMetrics三个活跃测试94个检查并新增TestCheck.h，表达式、顺序和故障注入保持，产品/CMake/NDEBUG/共享头与31冻结测试不变。新候选须独立导出base并应用该精确补丁，不能拼用旧结果。

R001实际结果：Debug/Release各14/14、四次threads0/2 smoke、README实际8080/www首页200/404/POST405、CLI必需参数及SIGINT退出均通过；ASan/UBSan五专项5/5，LSan detect_leaks=1保持。checker在-O3 -DNDEBUG下成功路径无输出、无分配、条件求值一次，失败路径准确callsite+SIGABRT。独立Reviewer已静态认可补丁，尚未给最终动态验收结论。

## 压测失败与关闭门槛

- 唯一M2准入smoke有效，r1-M2/M3/M6三个正式样本有效。
- 第4正式样本r2-M6在pre_audit报header EOF，warmup/measurement未启动，最终invalid；后5样本NotRun。
- 正式3valid/1invalid/5NotRun，无三轮完整聚合，无补样本或混旧数据。已回收全部自有进程；另获批准的一次有限诊断未复现EOF，根因仍未知。
- Reviewer独立完整动态验收：R003已执行至M2准入后治理失败，九正式和最终封包验收未执行；不用Builder或局部诊断结果替代。
- 最终提交、推送、合并、阶段/整体tag：本页不声称完成；分别以实际发布证据核实。

阶段关闭要求两角色各Debug/Release14项、四smoke、五sanitizer专项、唯一准入smoke及九个有效正式样本全部满足，并完成公开文件和保护检查；当前AC03未满足，需要Leader提出有界后续设计。R001实际475.213秒，旧失败146.073秒独立保留，不自动扩预算、重跑、补样本或修改产品。

## 有限诊断与未关闭事项

2026-10-09已按批准范围完成一次[Reviewer有限诊断](../benchmark/results/V1.0/S3/reviewer-diagnostic-001.md)：原Release/1MiB/2workers/15s Keep-Alive条件下，同连接三个请求均正常，未复现EOF。耗时36.381秒，其中55次原目录扫描累计35.860秒；原扫描最大0.786秒、guard最大间隔0.787秒，正常退出并回收全部自有资源。旧失败目录1921文件前后SHA保持。

扫描输入为失败清理后的快照，不能称为原失败现场；服务端只有聚合时延，缺少各响应完成时刻。扫描观察成本已确认，EOF根因仍未知，未复现不等于修复。原正式3valid/1invalid/5NotRun及Reviewer003 FAIL保持。后续编排调度修正和完整两角色复验需要具体批准，不能用局部诊断替代最终验收。

当前14项中 `server_integration_tests` 是HTTP黑盒别名；[23个冻结目标与覆盖限制](DEVELOPMENT.md#冻结旧测试与覆盖限制)保持，不能用历史28/28或当前14项替代。RO-002高并发长尾、TD-001 WSL2容量代表性、TD-006覆盖债务继续Open。只有S3独立审查及Leader收口之后，V1.0主线才具备收尾条件；V1.1不是自动启动内容。
