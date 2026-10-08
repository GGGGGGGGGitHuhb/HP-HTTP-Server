# M2 R007 唯一剩余 smoke 候选（待 Reviewer 准入，未执行）

唯一 `run-smoke-002`；1 秒 warmup + 3 秒 measurement，128 连接、server 4 workers、client 2 threads、预热前冻结 selected 4 对，detailed on。全部 WSL bootstrap、私有 marker 映射、工作和清理共用 outer reservation 的 20 秒绝对期限，预留 7 秒清理。root 只启用自有 marker，无系统 event/perf/host 采样；负载经 `initgroups(power)`、setgid/setuid 保持 uid/gid 1000。不得另开 helper 微跑。

```sh
python3 benchmark/tail-localization/localize_v7.py \
  --role builder --run-id run-smoke-002 --kind smoke --seconds 20 \
  --static-inventory-sha256 8199f58a268884125ce50c454821fc6318533e50824817e2306726273c782b61 \
  -- /mnt/c/Windows/System32/wsl.exe --distribution Ubuntu --user root \
  --cd /home/power/projects/HP-HTTP-Server --exec /usr/bin/env \
  TMPDIR=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp \
  TMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp \
  TEMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp \
  XDG_CACHE_HOME=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache \
  PYTHONDONTWRITEBYTECODE=1 \
  LD_LIBRARY_PATH=/home/power/projects/HP-HTTP-Server/.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu \
  'LUA_PATH=/home/power/projects/HP-HTTP-Server/.cache/v0.5-s4/tools/root/usr/share/luajit-2.1/?.lua;;' \
  /usr/bin/python3 \
  /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/root_marker_launcher_v2.py \
  --output /home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/run-smoke-002 \
  --manifest /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/m2-inputs-002.json \
  --seconds 20 --connections 128 --warmup 1 --duration 3 --detailed
```

入口需 Leader 第一次就经窄受控提升执行；普通沙箱不作权限探测。input manifest 固定实际 Release binaries、原 done-only summary.lua、source diff 与 inventory。通过的纯 binary 合成自检不等于本 smoke 已通过。

容量：6×16MiB=96MiB；最短完整请求 server14/client8 records，各32B。沿既有最高53095.45QPS/128均匀估计，25秒每serverworker约4.43MiB，client最多3 selected连接约7.60MiB；4秒 smoke 约0.71/1.22MiB。偏斜和partial/EAGAIN可增加事件，估计不构成容量证明；overflow仍立即invalid/stop，完整原始记录保留。

R007 前置：Leader 固定恢复 receipt 已绑定、纯 run-r007-check-001 通过且 Reviewer 独立准入后才执行。001 失败保留，不重置样本或预算；002 失败即停止。recording_start 是 GO 发布前实际 monotonic，future warmup_start 单列，原负载 warmup/measurement 算法不变。
