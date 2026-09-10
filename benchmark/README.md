# 固定版本 wrk 基线

该入口比较 V0.4/S4 与 V0.5/S3 的整体变化。正式负载是两个固定提交、1KiB/1MiB 两个文件、每组合三次，每次 5s warmup + 20s measurement；约 5 分钟，整套最多 600s。只连接 runner 自启服务的 `127.0.0.1` 端口，不接受外部目标，也不会安装工具、fetch、切分支或修改系统配置。

需要 Linux `/proc`、Python 3.12 标准库、Git 中已有下述两个完整提交、GCC、CMake、curl、dpkg-deb 和 Ubuntu 24.04 amd64 的运行库。本机固定工具为 GCC 13.3.0、CMake 3.28.3。两份程序分别从 `git archive` 导出，使用 Release/C++20、`-O3 -DNDEBUG`、`BUILD_TESTING=OFF`，无 LTO/native/sanitizer。不要复制已有 build 来代替构建。

| 标识 | 固定 commit | 固定 tree |
| --- | --- | --- |
| A | `e6aa82b5e2a95dc24bb76ff822599518a1e0d9fd` | `a12650f261912c55b26690df0e8e4239ede4fcf1` |
| B | `89514bd99a4410a6eb35d02444fe9e73f56232a2` | `f6912c28d25355873fbca48ede3b4d63c281a97c` |

## 仓库内准备工具

从仓库根目录执行。工具包固定为 Ubuntu 官方归档中的 wrk `4.1.0-4build2`，LuaJIT 两包均为 `2.1.0+git20231223.c525bcb+dfsg-1ubuntu0.1`。下载失败或 checksum 不符应停止，不能静默换版本。以下只解包到仓库，不执行系统安装。

```bash
mkdir -p .cache/v0.5-s4/tools/{packages,root} .cache/v0.5-s4/local/{tmp,cache}
export TMPDIR="$PWD/.cache/v0.5-s4/local/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR" XDG_CACHE_HOME="$PWD/.cache/v0.5-s4/local/cache"
export PYTHONDONTWRITEBYTECODE=1
curl --fail --location --max-time 60 -o .cache/v0.5-s4/tools/packages/wrk.deb https://archive.ubuntu.com/ubuntu/pool/universe/w/wrk/wrk_4.1.0-4build2_amd64.deb
curl --fail --location --max-time 60 -o .cache/v0.5-s4/tools/packages/luajit.deb https://archive.ubuntu.com/ubuntu/pool/universe/l/luajit/libluajit-5.1-2_2.1.0+git20231223.c525bcb+dfsg-1ubuntu0.1_amd64.deb
curl --fail --location --max-time 60 -o .cache/v0.5-s4/tools/packages/luajit-common.deb https://archive.ubuntu.com/ubuntu/pool/universe/l/luajit/libluajit-5.1-common_2.1.0+git20231223.c525bcb+dfsg-1ubuntu0.1_all.deb
sha256sum --check <<'SUMS'
909484b417378f0ef15fde4104f7d7adb3e5279b9578ad44984f4d29945034c0  .cache/v0.5-s4/tools/packages/wrk.deb
279787e9d7c3bbe41b01ff026702575352166eb3525f69029a83e1d55f164fb7  .cache/v0.5-s4/tools/packages/luajit.deb
e232f64a3b0a7ca0defbb18d30f7b181f9edaa45e949bb14dbc2405ae8748512  .cache/v0.5-s4/tools/packages/luajit-common.deb
SUMS
for package in .cache/v0.5-s4/tools/packages/*.deb; do
  dpkg-deb -x "$package" .cache/v0.5-s4/tools/root
done
export LD_LIBRARY_PATH="$PWD/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu"
ldd .cache/v0.5-s4/tools/root/usr/bin/wrk
```

`ldd` 不得有 `not found`。固定 wrk ELF SHA256 为 `b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d`，runner 会校验。该包 `--version` 在无 URL 时输出版本和 usage 并返回 1；实际负载必须正常退出 0。本次系统依赖：glibc `2.39-0ubuntu8.6`（libc/libm/loader）、libssl3t64 `3.0.13-0ubuntu3.9`（libssl/libcrypto）、libgcc-s1 `14.2.0-4ubuntu2~24.04`。实际动态库 hash 写入每次 run 的工具记录；环境不同须如实记录，不能当成完全相同的机器。

wrk 包的 `usr/share/doc/wrk/copyright` 列出 Apache-2.0、BSD-3-clause、Expat 及打包许可；LuaJIT 的 `usr/share/doc/libluajit-5.1-2/copyright` 列出 MIT/X 和 public-domain allocator。保留解包后的许可证，工具包/ELF 不进入版本控制。

## 构建与运行

继续使用上述任务内 tmp/cache 和 `LD_LIBRARY_PATH`。每个输出目录必须是新的空目录；以下名称仅供首次运行，复现时换成新名称。正式运行期间不要并行构建、测试或跑另一组压测。

```bash
python3 benchmark/build.py --revision A --output .cache/v0.5-s4/local/A-001
python3 benchmark/build.py --revision B --output .cache/v0.5-s4/local/B-001
NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost \
python3 benchmark/run.py \
  --baseline-manifest .cache/v0.5-s4/local/A-001/manifest.json \
  --candidate-manifest .cache/v0.5-s4/local/B-001/manifest.json \
  --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk \
  --output .cache/v0.5-s4/local/run-001
```

manifest 记录 archive/tree/源码文件 hash、配置命令、CMakeCache、compile_commands 和二进制 hash。runner 在前后核验固定身份和导出源码，不能手填一个 label 来标记不同程序。输出目录须位于仓库任务根内。预检要求 MemAvailable≥1GiB、磁盘可用≥4GiB、nofile≥256；不满足则非零退出，不自动调参。

服务器参数固定为 `--threads 2 --idle-timeout-ms 30000 --keep-alive-timeout-ms 15000 --shutdown-timeout-ms 5000 --port 0 --root <生成目录>`；文件为重复 `00..ff` 的 1024/1048576 字节 `.bin`。wrk 参数固定为 `-t2 -c32 --timeout 2s --latency -d <5s或20s> -s benchmark/summary.lua <本次URL>`。round1、3 为小文件 A/B 再大文件 A/B；round2 为大文件 B/A 再小文件 B/A。每样本新服务器，保留热页面缓存。

可在相同命令后加 `--smoke` 做独立 B/1KiB、1s+1s 实验，必须另用新输出目录。smoke 不构成正式基线，也不加入 12 样本统计。

## 有效性与结果解释

`run.json` 的 `log_limit_bytes=2147483648`、`min_free_disk_bytes=4294967296` 与 `observed_log_bytes_at_stop` 记录实际资源预算和结束日志量。上限由轮询检查后停止执行，允许检查和终止间隔内少量越界，不是文件系统硬配额；正式 CLI 没有提高或关闭预算的开关。再次越界应保留失败并停止，不能自动提高预算或筛选重跑。

`run.json` 保存整体状态、环境、构建/工具身份、执行顺序、原始计数、派生指标及分组摘要。每样本目录保存 server/warmup/measurement stdout/stderr、PID+starttime、回收记录及 `sample.json`。失败或 Ctrl-C 返回非零，整体 `invalid`，失败样本仍留在其目录，没有成功汇总；只移除本次生成的 root。不会丢弃失败样本或自动重试。整套所有样本的 stdout/stderr 累计日志超过 2GiB、每阶段超过 duration+10s、服务器早退均中止。正常 SIGTERM 最多等待 7s，随后仅对本次所属 PID 强杀并 wait；强杀使样本 invalid。

审计客户端自有原始 socket 接收缓冲，所有已读字节均保留并核对 header+Content-Length 的精确边界，同批 body 后多余字节也会被拒绝，避免带缓冲响应对象隐藏预读尾部。每样本前后各在一条 keepalive 连接上顺序请求五次，核验 HTTP/1.1 200、Content-Length、完整 body 长度/hash、无尾字节/断连。wrk warmup 和 measurement 必须请求数正、五类错误全部零、summary 完整有限、时长合理且 bytes≥requests×body_size。wrk 的 status 错误只涵盖 >399，独立审计额外拒绝 3xx。计时阶段无 response 回调，**没有逐个审计每一条计时响应 body**；总接收字节包含响应头，最低字节检查不等同逐响应正确性证明。

Lua duration/latency 原单位为微秒；QPS=requests/(duration_us/1e6)，MiB/s=bytes/实际秒/1048576，延迟换算 ms。每组保留三原始 QPS、median/min/max 和 `(max-min)/median`，跨度超过 20% 标 noisy 并保留数据，不能宣称稳定优劣。组延迟叫“每次 p99 的中位数”，不是全体请求的合并 p99。比较为 B/A 的 QPS median 比值，无性能通过门槛。

CPU 秒与单核 100% 口径百分比来自测量包围区间，server 与 wrk 分列。RSS 是每秒采样最大值及采样次数；VmHWM 是进程高水位，可能包含 warmup；均不是精确瞬时峰值或每请求归因。WSL、同机 loopback、closed-loop wrk、系统噪声和热页面缓存限制结果推广。A/B 包含 sendfile、异步日志与 Buffer 三阶段累计变化，不能单独归因某一项优化。

结果见 [V0.5-S4-baseline.md](results/V0.5-S4-baseline.md)。raw 中允许复现所需本机绝对路径；公开摘要不包含用户 home、完整 env 或私有协作路径。

## 快速验证

```bash
HP_S3_TEST_TMP_ROOT="$TMPDIR" python3 tests/benchmark_runner_tests.py
cmake -S . -B build-v0.5-s4-local -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v0.5-s4-local -j4
ctest --test-dir build-v0.5-s4-local --output-on-failure --timeout 60
```

快速测试中的 fake wrk/HTTP/故障是明确标记的 synthetic 测试，包含真实 socket 和子进程生命周期验证，不能替代实际 wrk。默认 CTest 只运行快测，不自动触发五分钟压测。运行以上 socket/进程检查需要环境允许 loopback 与 `/proc`；不改变全局权限或设置。
