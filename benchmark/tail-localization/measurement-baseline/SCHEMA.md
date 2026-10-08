# baseline-v1 原料契约（静态候选）

状态：候选未封存、未执行；实现须对照本表，不把文字代替校验。

`sample.json` 顶层 `schema=baseline-v1`，`status=valid|invalid`、首失败 `first_error` 和后续 `cleanup_errors` 分开。`identity` 包含 package canonical SHA、relative input-lock SHA、server/client SHA、role/run、实际根路径；内容身份不包含可重定位 absolute path。

`window`：warm_start_ns、T0_ns、T1_ns、duration_ns、clock=CLOCK_MONOTONIC；唯一共同窗发布，duration_ns严格T1-T0。`readiness`：两线程、128连接各 lifetime/owner/ready_ns，所有 ready 早于 warm_start；连接不得重连。`runtime`：进程实际启动/退出/reap、join/drain界限，不能代替 measurement duration。

`completed` mutually exclusive：warmup、cross_warmup_to_measurement、measurement_start_to_measurement、after_measurement。主N等于中间两类和；start>=T1为invalid，completion==T0入测量，completion==T1入after。`pending_at_T1`是时点快照，可能与最终after completion有交集，不能和completed相加声称总请求。`censored`按warm/measurement start和unsent/partial/full_sent分别计数；每个pending记录保留request life/seq/first_attempt/T1以及等待下界。正常停止截尾不伪装timeout/完成零延迟，实际测量错误invalid。

`latency`明确 main raw、measurement_start_only raw、cross_warmup/warmup/after raw以及两组 synthetic corrected。每组字段：count/min_us/max_us/nonzero_bin_count/catalog_file_sha；0us是合法桶，单请求延迟floor(ns/1000)，范围0..2,000,000us（含上界）。全部线程停写后原 raw 冻结，original stats_correct读取该副本并产生corrected。`correction`记录整数duration_us、N、N//128、interval_us；分母或interval零invalid。measurement_start_only独立用其N重算，不选择较低结果。

`histogram`文件：二进制非零bin升序记录，每条uint64 little-endian value_us/count，完整记录禁止重复/零count/越界/count溢出，不能输出部分后标全量。每文件非零bin容量262144（4MiB），七文件最大28MiB；若真实非零桶超过声明容量，保留真实总数/首因，输出invalid resource/evidence_overflow，不缩小2s范围、不删稀有桶。该上限约束导出证据，不更改内存全2s histogram；正式冻结前须对stats_correct count增长再次核uint64上界。

`slow`主raw中每个>=50,000,000ns的完整completion都有定长record：owner thread、life、seq、start_ns、completion_ns、class、HTTPstatus、body_bytes。每线程16384，总32768；overflow使invalid。不能只保留top5掩盖容量不足。

`errors`：HTTPstatus/body/protocol/connect/read/write/request_timeout等真实错误；停止截尾另列。`body`固定长度1024及全部byte(index%256)校验，server fixture内容SHA与实际document root绑定。

`catalog`逐文件相对路径/SHA/bytes、输出根绝对路径另列；拒绝escape/symlink/旧cache依赖。离线只读该schema与包内算法参数，独立Python从raw重建所有corrected bins，再比对count/每bin/P50/P99/max；不能调用C percentile或读取client摘要直接当复核。

内存估计与磁盘分开：每线程5个(2,000,001*8) histogram约80MiB，两线程约160MiB；merge五组约80MiB，corrected两组约32MiB，慢记录约2MiB，总上界约274MiB加官方wrk/parser/socket/control。优先复用不再写的thread bins减少副本，但raw immutable copy必须保留。此为固定分配RAM；512MiB计划是新磁盘累计，不能混算。

smoke原料七hist最多28MiB、慢record约2MiB、pending/identity/metadata小于1MiB，smoke/check/offline/tool/log整体64MiB；构建receipt日志属320MiB。正式raw上限128MiB含INFO日志，stdout/stderr均计量；所有限制到达即invalid并收尾，不改INFO/null或吞日志。

原算法额外元数据：`raw_min_us`/`percentile_scan_min_us`固定为原raw最小桶。stats_correct补入更低 synthetic 桶但不更新stats.min；corrected的actual最小非零桶与rank扫描下界须分别记录。原stats_percentile rank为round(p/100*corrected_count+0.5)，从保留raw_min扫描，不能换常规ceil或从synthetic最小桶扫描。独立Python既复核完整bins，又按这一原算法边界验证client百分位，不静默“修正”原定义。

最终连接枚举：end_reason=1 表示 incomplete_send_censor（sent_bytes=0 为unsent，>0为partial，active=true/full_sent=false/pending=true）；2 idle_at_T1（active=false/pending=false）；3 completed_after_T1（active=false/full_sent=true/pending=true）；4 deadline_censor_after_T1（active=true/full_sent=true/pending=true，绝对request deadline>=T1）。active字段名固定`active`。sequence在首次write尝试时递增，idle保留最后实际请求序号，不预递增。`censored_by_start_phase`二维数组行warm/measurement，列unsent/partial/full_sent_deadline；与逐connection终态复算相等。`pending_warm`按pending请求firstwrite<T0复算。截尾不伪完整时长，after-completion仍进after桶且不进N。

sample原料包括实际build-receipt副本、固定document-root/payload bytes及SHA、C实际request bytes/SHA、ownPID UID/GID、dladdr实际LuaJIT路径和实际LD/LUA环境、已封buildreceipt的DSO identity。CPU/RSS字段为固定ownPID/starttime的/proc样本包络，至少两个样本，CPU按真实first/last时刻tick差额计算，sampledRSSmax而非内核peak；退出后无final /proc样本如实标记，不假称完整measurement-only CPU。
