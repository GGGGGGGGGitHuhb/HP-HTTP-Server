# V0.5.1 / S2 Builder 修复验证

2026-09-28。候选在Acceptor交付已接受连接之前设置TCP_NODELAY；设置失败只关闭该连接，继续接受下一连接。文件响应仍使用sendfile。本记录是Builder实测，尚待独立验收；S3及RO-002整体收口未完成。

## 正确性与身份

当前6项加新增专项/黑盒共8项CTest最终全部通过，专项ASan/UBSan通过。新专项验证真实选项读回、无效fd errno、callback前设置、单个已接受fd的setsockopt失败/关闭一次/后续健康及真实EAGAIN恢复。threads0/2黑盒覆盖10次keepalive、0/1KiB/1MiB/8MiB完整字节、FIN/RST/close/SIGTERM。最终新工具40项synthetic通过。

C固定提交 `942f72cd9cea58e097025c3b9dd660f4132a1ffb`；D是显式未提交快照，保留工作区实际学习注释，[候选规格](builder-001/candidate-spec.json)archive SHA256为`3492c5926d8e62b75832c3a808c0aba95265dbf5bfcf0b703c968605e7a3478f`。双方独立Release -O3 -DNDEBUG，无sanitizer/LTO/native；具体源码/工具/二进制hash见[原始正式记录](builder-001/run-CD-001.json)。

生产修改限Socket.h、Socket.cpp及Acceptor.cpp。旧测试、S1工具/结果保持。用户注释未纳入本阶段生产补丁，工作区原注释保留；不将整个含注释文件直接作为待发布变更。复现命令见[修复入口](../../../repair/README.md)。

## 正式C/D配对结果

每版本×1KiB/1MiB×三轮，共12有效样本；每样本5s warmup+20s measurement，2workers，wrk -t2 -c32，默认客户端ACK；五类错误均零，前后长度/hash/复用/尾字节审计与正常回收通过。

| 指标 | C | D | D/C | 批准门槛 |
| --- | ---: | ---: | ---: | --- |
| 1KiB QPS中位 | 712.793 | 37108.201 | 52.06027 | ≥10 |
| 1KiB每轮P99中位ms | 48.354 | 1.946 | 0.04024 | ≤0.25 |
| 1MiB QPS中位 | 434.236 | 425.266 | 0.97934 | ≥0.90 |
| 1MiB每轮P99中位ms | 183.356 | 173.704 | 0.94736 | ≤1.25 |

四组QPS跨度：C小0.399%、D小3.261%、C大6.571%、D大3.018%，均≤20%。这是同机固定负载结果，不代表公网或物理机容量。

| 场景 | server CPU中位，单核100% | wrk CPU中位 | server采样最大RSS KiB |
| --- | ---: | ---: | ---: |
| C 1KiB | 5.939% | 2.875% | 5280 |
| D 1KiB | 193.670% | 102.779% | 5440 |
| C 1MiB | 25.412% | 105.818% | 5440 |
| D 1MiB | 25.487% | 105.755% | 5440 |

小响应吞吐提高同时明显增加双方CPU使用；RSS是采样值，非瞬时峰值。计时body未逐个审计、WSL2同机loopback/热页缓存/closed-loop等限制保留。

## 默认客户端时间线

C/D各一组，每组三连接×32请求、逐body核验，客户端未设置QUICKACK。正文等待中位数ms：

- C：42.903614 / 42.923908 / 42.790648。
- D：0.309939 / 0.317885 / 0.276869，均<5ms。

[机制C](builder-001/run-timeline-C.json)、[机制D](builder-001/run-timeline-D.json)及原始syscalls.trace与无插桩吞吐分开；不声称直接观测内核Nagle状态。

## 纠错、保留证据与预算

功能准备首轮7/8：手动accept后valid断言失败；窄strace未重现该点，而暴露测试注入将无效fd=-1误当目标、把EBADF改成EIO。分别增加accept就绪等待、限定非负定向fd；不把初始未复现的时序原因写成已证实产品缺陷。随后将新C++检查改为始终执行的require，避免Release去掉assert内操作。最终完整8/8及sanitizer通过；历史失败日志保留。

F-S2-01静态检查发现预算绕过：嵌套输出不计直属账本，机制限额又依赖目录名称。正式结束后经Leader明确授权，修复为直属输出、递归旧账本、按真实kind记录计组，并补正反例。旧脚本、[最小纠错diff](builder-001/budget-fix.patch)与正式原始身份保留。本套实际就是合法直属run-CD-001且此前没有其他真实测量，源/工具尾部核验通过、预算未越界；修复不改产品、采样或数值，因此明确继承这12样本，不伪装为新脚本运行或自动重跑。两组机制在修复后首次执行。

正式+机制累计310.796秒，角色stdout/stderr/trace254256850字节，低于30min/2GiB；14次记录端口均关闭、16自有PID已回收，无强杀。完整[结果索引及日志hash](builder-001/summary.json)保留，独立Reviewer仍需核验并复现。
