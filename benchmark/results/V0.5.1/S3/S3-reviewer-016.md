# V0.5.1 / S3 Reviewer 016 — R015原始perf能力与符号边界

2026-10-06，阶段结论：**FAIL**。Approved R015唯一自有能力检查invalid，未执行HTTP，原记录与停止历史保留；原正式P3门槛失败未关闭，不将能力结果改成cap PASS。

实际perf_event_open返回fd3，CPU_CLOCK≤99Hz、内核IP/TID/TIME/CPU/CALLCHAIN及mono取得44条样本。Reviewer静态检查attr112、ring acquire-head/seqcst-tail、wrap、LOST/THROTTLE/unknown及清理；独立原ledger `run-r015-perf-verify-001` wall0.20795989036560059秒，逐条raw与summary一致，kernel context/IP、PIDns/身份/时间有效，非sample记录0、head/tail/raw完整、FD/mmap/pipe关闭且child正常回收。

能力检查1.5110130310058594秒的失败原因是工具要求全部kallsyms地址单调；readonly分组解释0.8600351810455322秒实际发现329全表回退、10groups。所需地址均落唯一当前core严格text区间，unknown0；不安装、不重新perf采样、未启动HTTP。当前lookup不是原snapshot，capture boot未封存，historical_symbol_identity_verified=false；不能无条件确认旧函数归属，也不能把工具失败称系统不支持perf。

已有原始记录证明本次perf内核采样实际可用，不证明生产sendto执行位置或宿主暂停不存在。R016仅经独立Approved补充基线恢复原未用观察，不能由本轮offline自动放行；预算继续原R01540秒/48MiB及R008300秒/500MiB，不重置。本轮dynamic全部结束，后续同期boot/身份/符号边界、零loss及清理须另审。

证据：`reviewer/r015/perf-independent.json`与Builder原cap/readonly run；独立raw SHA `1b0824ef932ae948f6b7a0a8e5b43ff6bec8b8cac362b9c2a0632bdd85b8594e`。正式阶段仍FAIL，S3不得关闭。
