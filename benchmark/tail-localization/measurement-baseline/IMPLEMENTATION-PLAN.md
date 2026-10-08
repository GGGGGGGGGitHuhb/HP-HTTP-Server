# R015 baseline-v1 静态实施清单

状态：实现准备；仅依据 Approved R015 revision1 / review-amendment-003。此文件不是执行许可，当前没有编译、测试、下载或新 HTTP 样本。

## 相对路径与依赖职责

- `inputs/E-S3.tar`：固定 acda3f92d42a36d0b0554e185bc6f4155b4e5889 archive；SHA256 30060ccd70acf23e54559733d5050589ed16d76282a7ca91dd6a43934b490ba6；6,246,400 bytes。构建期间安全解包，保留 CMake 和实际编译输入，不复制旧已污染导出目录。
- `inputs/wrk.orig.tar.gz`、`inputs/wrk.debian.tar.xz`：官方 4.1.0 / Ubuntu 4.1.0-4build2；SHA256 d8f1ba8b70ae5e10aef63ef4d5b6520bc0da103f7f1c59c79f50e2efd6810f8f / 3bbc69e3f1a50086110632f0e8e329a4240388e2e2477e70087a50f4b644c54e。Debian patch 明确逐项应用，禁隐式下载。
- `inputs/luajit-dev.deb`、`inputs/luajit-tool.deb`：2.1.0+git20231223.c525bcb+dfsg-1ubuntu0.1；293,160 / 280,972 bytes；SHA256 ef46b0ec8984a6cc00120e156d5670a3f2aff448ec31dc17222c14f0a2248aaf / c7667a3db792eeb16acbac1a9d304e39da2605e5835feaca77e96208e9aa6090。
- `runtime/libluajit-5.1.so.2.1.1703358377`、`runtime/lua/`、`licenses/`：既有同版本官方运行库、全部实际字节码模块和对应 copyright；复制真实常规文件、构建自己的相对链接，禁旧 cache 绝对链接。逐文件 SHA/来源许可写入 `inputs-lock.json`。
- `config/payload-1024.bin`、`config/request.bin`、`config/workload.json`：固定 1 KiB 模式 bytes(range(256))*4、GET/pipeline1、4 server workers、2 client threads/128 conn、2s timeout、TCP_NODELAY/默认 ACK/INFO。
- `client/MeasurementWindow.h`、`client/MeasurementWindow.c`：唯一 MONOTONIC 窗、请求状态、完成分类、计数/桶和私有慢记录；无运行时 Python 或旧 observer 依赖。
- `client/patch_wrk.py`：对官方源施加严格唯一匹配的 main/thread/connect/write/parse/timer/export 接缝；补丁失败即停，不吞版本漂移。原 stats.c/h 不改变算法。
- `build_package.py`：安全解包、依赖校验、Release E target、client、实际 C ASan/UBSan unit；系统工具显式 allowlist，不探测下载。安装位置仅本包和 own output。
- `owned_process.py`、`execute_sample.py`：最小自己子进程身份/逐项回收和固定样本入口；自身代码闭合，不通过 sys.path 导入旧 tail-localization 平台。
- `verify_sample.py`：独立 Python 重建全部 corrected bins/百分位、字段与原料绑定；只包内输入和显式 output，无阶段账本依赖。
- `tests/MeasurementWindow_test.c`、`tests/test_package.py`：实际 C seam 和包路径/治理反例；不导入旧 rXXX tests。
- `check_package.py`、`manifest.json`、`README.md`：先完整 Python closure compile 再 tests；相对内容身份与实际 receipt 路径分开；命令封存后才标可执行。
- 包外新具名 adapter：只增 R015 exact role/run/kind/limit 准入，沿原 wrapper/Reservation/watchdog/保护；不进入统计包闭包，不重置旧 HTTP count。

当前这份是逐文件规划，文件尚未全部实现或复制，不能声称闭合清单已验收。

## wrk 实际接缝

1. 固定 IPv4 literal 取消原 bootstrap connect，仅保留目标地址校验。正式 128 socket 全部 worker ready 后 main 发布共同 warmStart/T0/T1；worker 未见共同 gate 不发 request。
2. socket_writeable 在第一次 syscall 尝试前调用 beginRequest(ns)。EAGAIN 和部分写保留同一 life/seq/start；后续 write 不重新取请求开始时间。每次 write 前核 T1，T1 后禁止继续未发送字节。
3. 完整 parser/body callback 核 status 200、Content-Length 1024、实际 payload 模式，再取完成 MONOTONIC ns；唯一 classifyCompletion 同时决定 N、main raw、measurement-start-only raw、cross-warmup 桶。warmup/after-end completion 不进入 N。
4. T1 已发完整 request 的在途 response 可收至其原 2s deadline；未发完整 request 仅正常截尾，不能伪 timeout/0 latency。pending-at-T1 与最终 censored 按 start phase/write state 保留，不与 mutually-exclusive completed 类别混加。
5. 每线程停止写后 merge，冻结原 raw 的不可变副本，再用原 stats_correct。interval_us = duration_us // (N // 128)，任一分母/interval 为零即 invalid。stats_record 零桶、2s 上界、整数 count/bin 溢出显式校验，拒绝静默丢样。
6. main raw 每个 >=50ms 样本存 per-thread 16384 条定长私有记录（总 32768），life/seq/start/end/class/status/bodybytes；溢出 invalid。不在热路径写文件。

## schema 计划

`baseline-v1` 导出：source/package/binary identity；单一 warmStartNs/T0Ns/T1Ns/durationNs；线程/连接 ready 和实际退出时刻；各 completed 分类及 pending/censored 状态；N、固定窗 QPS；main raw 和 measurement-start-only raw 的非零 bins/count/min/max/overflow；cross-warmup、warmup、after-end 边界桶；两套独立 correction interval 和 corrected bins；HTTP/socket/protocol/timeout errors；slow-record capacity/count/overflow；实际子进程身份、reap/cleanup 及原料 catalog SHA。

ns floor 到 us；durationNs 精确 T1-T0。启动、join、drain、cleanup 不进入 QPS 分母。raw 和 synthetic corrected 明确分字段；Python 重建参考不复用 C 百分位函数或旧 summary。

## 全计划容量与时间边界

每角色全部新增 bytes 上限 512MiB：package/prepare/build/真正重定位副本合计 320MiB；Builder 正式原料 128MiB；check/smoke/offline/tool/log 合计 64MiB。Reviewer 没有正式样本也不挪用其 128MiB 额度。

320MiB 分配：不可变输入/开发与 runtime 展开/许可及代码 24MiB；native E Release build 64MiB；native client/unit 24MiB；独立路径 package 副本 24MiB；独立路径 E Release build 64MiB；独立 client/unit 24MiB；构建/依赖 receipts/log/临时解包峰值 96MiB。合计 320MiB。不要同时保留无用双重完整 E 展开；各阶段峰值及实际所有路径仍计量，不凭这个估计放行。

旧 fresh Debug+Release+诊断 client+sanitizer 的计费构建约 33.3s，仅作同主机可行性参照；本次 180s 包含 7s cleanup，即 173s 工作。两处 Release-only E target + client/unit 构建预计可容纳，但新接缝尚未编译，不能承诺。风险是双路径构建、编译错误、sanitizer/解包数量和 INFO 日志峰值；完整封包必须再核静态实际文件尺寸与预留，若预计超过173s或320MiB先 BLOCKED，不用实际失败试额度。

Builder 原累计约727.078086s，本轮335s使总计1062.078086<1200；旧累计1,486,288,348B，新增512MiB+8MiB guard后仍余115,935,780B。无删除、压缩旧历史腾额度。
