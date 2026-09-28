# V0.5.1 / S1 独立诊断结果

2026-09-28，Reviewer独立复审PASS；首次Reviewer001因中间工具缺少尾部身份核验而FAIL，修复后38项测试和原三漂移反例通过。没有生产修复。

固定A/B/C的18正式样本独立有效，1KiB QPS中位分别33533.402 / 713.222 / 715.070，P99中位2.235 / 48.378 / 48.392ms。独立v0.5-s1三轮中位711.703QPS，异常已在该发布快照存在。

默认客户端B/C三连接正文等待中位约42–43ms；只改变收到头部后的客户端QUICKACK，分别降至0.25–0.33ms / 0.31–0.47ms。自有strace显示header send与sendfile迅速返回，长等待发生在成功写入以后。A单次发送头+正文，S1/B/C拆分发送；证据支持拆分发送与ACK等待交互。具体Nagle/delayed ACK内核状态没有直接观测，不将推断写成抓包事实。

完整原始JSON、六份短trace和日志hash见 [reviewer-002/summary.json](reviewer-002/summary.json)，前轮正式独立结果保留在 [reviewer-001/summary.json](reviewer-001/summary.json)。大型日志仍在本地Reviewer独立目录。9套测量/trace累计552.568s，六组均低于10s，日志242760293bytes；21个样本前后审计、错误零及全部33PID/27端口回收核验通过。

环境为WSL2同机loopback热缓存，A/B与C顺序测量，计时body并非逐个审计；不推广为物理机容量。根因诊断通过不代表性能修复通过，RO-002继续Open，S2/S3须另行批准。
