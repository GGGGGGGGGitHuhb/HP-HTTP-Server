# 固定双版本构建与短HTTP冒烟

> 2026-10-08归档：V0.5.1已搁置（未完成）。以下命令/批准说明仅为历史记录，当前候选非生产、非就绪入口；依赖本地冻结资料，未完成正式采集。不得按旧命令恢复，见benchmark/results/V0.5.1/SHELVED.md。

当前为R028获批实现候选，**尚未执行验证，以下接口不是已验证公共命令**。本轮只检查固定源码导出、Release构建、1KiB响应审计与回收，不比较性能，不替代完整CTest回归，不关闭S4/RO-002。原失败、占额和工具原字节均保留。

C为`942f72cd9cea58e097025c3b9dd660f4132a1ffb`（tree `b407f052c7a974ae4fff4976c8275fb5905cf036`）；D为`69424e6ab057bba2950c018e34c5695a4dc74f22`（tree `826e20166caa334c95a1c6fdc957a128d5aee568`）。新模块从本地git archive导出，不checkout/fetch/复制脏工作树。CMake固定Release/C++20、`-O3 -DNDEBUG`、关闭测试/LTO，g++，`-j2`。

每角色独立根`.cache/v0.5.1-revalidation/{builder,reviewer}`，协调者先建空`control/tmp/cache`。按契约30秒→C构建240秒→D构建240秒→C冒烟45秒→D冒烟45秒顺序，每项一次；Builder全过才Reviewer独立重建。首失败停止，没有补轮。本轮每角色新600秒/2GiB额度与历史账本分开，普通日志累计64MiB；运行前要求空余磁盘4GiB、MemAvailable1GiB、nofile256。

接口从仓库根调用，角色、标签和输出必须精确：

```text
python3 -B benchmark/revalidation/test_revalidate.py --output <角色根>/contract-001
python3 -B benchmark/revalidation/revalidate.py --role <角色> build --label C
python3 -B benchmark/revalidation/revalidate.py --role <角色> build --label D
python3 -B benchmark/revalidation/revalidate.py --role <角色> smoke --label C --output <角色根>/smoke-C-001
python3 -B benchmark/revalidation/revalidate.py --role <角色> smoke --label D --output <角色根>/smoke-D-001
```

执行清单必须展开所有占位符。TMPDIR/TMP/TEMP指本角色tmp，XDG_CACHE_HOME指本角色cache，`PYTHONDONTWRITEBYTECODE=1`、`NO_PROXY=127.0.0.1,localhost`，LD_LIBRARY_PATH只指已有wrk局部库目录`.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu`。仅依赖Python标准库、本地Git对象、GCC/binutils、CMake/Make、ldd及已有官方wrk/LuaJIT；不下载/安装依赖。

外层由Leader在control发布一次意图（完整argv/environment/input SHA），再调用`/usr/bin/time -f %e -o <wall> /usr/bin/timeout --signal=TERM --kill-after=7s <上限减7>s <完整命令>`。不用foreground或setsid。内部工作期限再提前3秒；TERM进入finally，收尾阶段重复TERM不覆盖首因，绝对清理期限不延长。外部wall和进程组清理核对后Leader写结束记录，占额按原始wall向上取整；缺wall/清理不明为unknown、保守占满上限，不能以内部耗时替代。

build创建C/D叶、source/archive/build，成功才发布manifest。manifest保存固定commit/tree、归档清单、source SHA、实际编译配置/编译依赖、工具与动态库SHA；smoke前后重核。smoke创建run与document父目录，旧`run_sample`创建sample叶；固定2workers/wrk32connections、1秒预热+1秒采样，前后各5次200/长度/hash/keepalive审计。文件fixture结束删除，原始样本、日志和清理工件保留。

只读复用`benchmark/run.py/build.py/summary.lua`。新模块仅在独立加载的run实例装配TrackedProcess和统一log_guard；不调用旧main/身份CLI、不修改build的COMMITS/TREES/FLAGS。TrackedProcess使server/wrk注册先于身份读取、共享监督进程组、绝对期限回收，close错误转显式invalid以保留sample保存路径。构建CMake子树也登记有限身份并保存清理结果。目录扫描使用nofollow FD，普通文件按st_blocks实际分配与逻辑日志统一计量；构建期消失另记、最终不稳定树拒绝。轮询是检测而非物理配额。

每步`control/<build|smoke>-<C|D>.result.json`为内部退出证据（xmode不覆盖）；契约为`contract-001/contract-result.json`。它们不是外层费用结算。`manifest.json`、`sample/sample.json`与原始日志供独立Reviewer核验；未执行/失败步骤不宣称通过。

## R029监督候选（仅静态交付，动态尚未授权）

R028首contract的9项PASS仍保留；旧外组结束证据unknown、保守占30秒也保留。R029新增监督入口，不修改旧适配器/once材料，不允许重跑contract或继续构建冒烟。静态审查通过只能用于提出下一次动态申请。

```text
python3 -B benchmark/revalidation/supervise.py --role <role> --step <fixed-step> --table <frozen-table> --table-sha256 <SHA> --row-sha256 <canonical-row-SHA> --output-control-root <role/control> --maximum-seconds <approved-upper> --inputs <sealed-inputs> --inputs-sha256 <SHA>
```

完整角色/步骤row按原argv/environment逐项验证，不接受任意shell/commit/URL。新监督父创建`control/r029-<step>-001`，exclusive intent发布即消耗一次；旧意图/result/wall不覆盖。父启动time→timeout→同文件私有worker。worker先报告实际pid/starttime/pgid/sid、timeout父身份及PID namespace，并等待管道ACK；父在**同一次exec/同一namespace**核timeout最终pgid等于timeout pid、worker实际加入该组、group不同于父自身group、time→timeout→worker亲缘链后才放行。私有worker也重核冻结row/spec路径，不能用内部入口执行任意命令。

time/timeout仍不foreground、不setsid、TERM后7秒KILL。完整起点在解析/资源检查/意图之前；为准备/结束观察留在同一上限内，普通步骤的TERM线提前15秒（7秒升级+8秒最终观察预留）。 prospective小夹具每case12秒，TERM线提前10秒（7秒升级+3秒收尾）；这不是另批预算。父wait后实际扫描本namespace `/proc`，活成员、zombie、消失及读取unknown分列；仅登记并复核pid/starttime/pgid/sid/namespace的对象可被signal，绝不信号父组。group leader已退出后未知新成员不被认领。僵尸/权限不明/缺握手/缺wall/缺必要保存均保持unknown占满上限。

正常、非零、握手拒绝、time/timeout TERM/KILL、父TERM、扫描/证据错误都进入有限finally；每个FD/stream/进程操作分别留证，首因不被后续错误覆盖。`exit.json`是尚未finalized的保存证据；`settlement.json`才是结束观察与清理后结算，且消费者必须同时核真实CLI退出/必需工件/完整组核。完整elapsed采用CLOCK_MONOTONIC从入口起点到exit证据保存后，向上取整；time.wall单独只覆盖time子树，不混同；最终settlement序列化仍必须在总deadline内，失败或越界由stderr/非零退出指示unknown占满，不能读取partial文件声称valid。

空间仍属于R028角色2GiB/普通日志64MiB，无新增预留。监督父自身源码、JSON、work stdout/stderr、握手/扫描/退出均计入角色树；FD nofollow扫描不跟随link。能力边界：冻结适配器和本子链没有setsid/foreground；一次group扫描不能证明任意恶意逃组后代不存在，不据此扩大权限。

`test_supervise.py`为**未获动态预算**的独立短本地验证驱动：6项静态/故障单元契约与6个真实CLI前瞻case（正常、非零、timeout、已回收小子树、握手拒绝、父TERM）；整驱动拟30秒/4MiB，每角色一次。driver自己的PID/starttime/group/亲缘登记、逐对象收尾独立于被测监督函数，不用被测入口为自身收费；具体独立外层调用/结算还需Leader明确批准与Reviewer审查。不要直接运行本节命令。

### R030限定后继（planned / unexecuted）

旧三源已逐文件保存于Builder control/r030-preserved-001，旧r029封印不覆盖；历史核验按原SHA→副本映射。新候选仅补两异常接缝：证据publish和stderr同时失败仍保首因、继续wall/space/input/结算尝试；time启动后先实际登记timeout及可取得worker父链，再读握手。创建至登记无法确认则unknown；未确认group不宣称complete。放行端先关闭，登记对象信号前复核PID/starttime/group/namespace，重挂父不丢身份；同监督父wait后逐对象核存活。

未来具名入口：`python3 -B benchmark/revalidation/test_supervise.py --role <role> --inputs <r030-inputs> --inputs-sha256 <SHA> --r030-only`。只选三个场景：正常真实CLI、精确group-observations发布及stderr组合失败、实际timeout已登记但pipe_json精确EOF导致工作未放行。注入子入口仍走candidate.main完整argv及真实time/timeout，不替换spawn/身份/清理。独立驱动每场景最多4秒工作，累计工作最多12秒；首次超时停止，整步20秒保留8秒自有收尾。三短命令无sleep负载；扫描/平台过慢仍停止，不保证机器速度，不预跑。每role最多4MiB/一次的20秒计划尚未批准，不能使用旧30秒方案或旧600余槽替代。

### R031完整外层入口

R031已批准两role各20秒的三个关键场景；须先通过整链静态审查并由Leader生成独立r031-admission-001.json，不能直接使用旧r030命令。新execute_check不导入supervise，也不调用其身份/清理/结算；只执行固定test_supervise --r030-only。两完整time/timeout argv及role环境在r031-commands-001.json，可信基础为固定SHA系统GNU time/timeout、Linux /proc与Python标准库。

外链固定time→timeout（TERM18秒、kill-after2秒）→execute_check→driver→三场景。execute_check核approval独立原SHA、准入schema r031-execution-admission-v1（role/run_id/approved/static_review/approval_sha256及inputs/commands/seal路径与SHA、canonical row_sha256），完整argv/environment和闭包后exclusive提交意图。准入是未来Leader材料，不作为必需已存在封存输入，避免自引用。

共同起点以实际timeout父starttime与BOOTTIME/MONOTONIC转换保守包含入口加载；工作不晚于起点+10秒，独立收尾与durable结果不晚于+16秒，为外TERM18/KILL20留余量。driver接受两个绝对共享deadline，不重新获得20秒；三项各4秒仅是单项上限，总工作仍受10秒门。角色outer与driver材料合计4MiB，失败即停。

execute_check实际登记driver与子孙，来源PPID单列，reparent仍按pid/starttime/pgid/sid/namespace核；GNU timeout初次setpgid仅在原实际父链仍在且namespace一致时记录身份转移。收尾不kill组、不setsid，只逐对象复核后信号；同一次exec wait后核已登记对象及实际组未知成员，alive/zombie/unknown不为complete。结果、原始身份/扫描与错误都在退出前durable发布；信号/写出/stderr失败不覆盖首因。系统time.wall从启动timeout到整个execute_check子树结束，time最终wall发布及Coordinator静态消费明确是观察层，不递归增加监督器。外returncode非零、wall缺失/超界、outer未知或driver任一场景失败即占20并停止，不恢复R028。
# R032：原九个未消费准备槽

当前入口为 `execute_check.py --role ROLE --step STEP --approval PATH --admission PATH --output PATH`。完整九条绝对 argv、环境和 cwd 见 Builder control 下 `r032-commands-001.json`；它依次为 Builder build-C/build-D/smoke-C/smoke-D，Reviewer contract/build-C/build-D/smoke-C/smoke-D。每槽一次，任何失败或 unknown 停止全部后续。旧 Builder contract 的九项功能 PASS 与 cleanup unknown/保守30秒保留，不重跑；Reviewer 合同保留原九项。

外 time/timeout 的 M 分别为240/45/30，TERM=M-2、KILL=M。真实 timeout 起点建立共同 work=M-10、outer cleanup=M-4；适配器自身 cleanup 截止为 outer cleanup 再提前2秒。所有启动、导入、资源核验和收尾都在同一槽中，不重新取得完整预算。外层核整个 role 树2GiB及64MiB逻辑日志，最后静态消费还须纳 time wall 和外日志。

Leader 仅在独立静态 PASS 后 exclusive 发布单槽 `r032-STEP-admission-001.json`：schema=`r032-execution-admission-v1`，role/step/run_id，approved=true，static_review=PASS，approval_sha256，inputs/commands/seal 各 path/sha256，row_sha256（排序键、紧凑 JSON，无换行）。首槽 r031_actual_verified=true；其余槽绑定前驱 result path/sha256、time wall sha256 与 exit_code=0。未来 admission 不属于当前 required inputs。不能一次提前发布全部 admission。

构建消费者核真实 manifest commit/tree/role/binary SHA 与 Scope 清理；冒烟消费者核原 sample valid/reaped/no forced；合同消费者核9项、无失败错误。旧 run/build/Lua、COMMITS/TREES/FLAGS 不变。准备阶段只有静态实现，尚未执行 R032；后续首 Builder build 是全部新接口的首次真实执行。原 C/D 输出、样本叶和 result 已存在均拒绝覆盖。

