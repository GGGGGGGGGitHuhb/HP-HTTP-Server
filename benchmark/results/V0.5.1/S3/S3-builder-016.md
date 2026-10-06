# V0.5.1 S3 Builder 016 — 小量发送长区间

2026-10-06。Approved R014只运行一次3+6秒观察，未改生产，正式S3仍返工中/FAIL。

QPS51026.207414、corrected P99 24.240ms/max244.685ms，errors全0；无同轮控制，不能证明正式性能改善。仅server首worker约4.866秒coverage，loss0/entries0/完整排空；完整sendto56841、sendfile64 56841、recvfrom56840、futex34966、epoll_wait10644，覆盖尾端未返回epoll及调度区间单列未知。

最长sendto240.784ms，发送105B（raw len69/ret0x69）、MSG_NOSIGNAL，无完整offCPU；源码中ConnectionIo先send响应header再sendfile body，与该小量发送方向一致。但没有用户栈/请求内容关联，不能认定某请求、纯CPU或特定内核原因。最长futex7.868ms，其中blocked7.234ms、runnable0.624ms；地址6177ba63a940未知对象，不能猜logger mutex。

sendfile最长5.640ms、recvfrom2.451ms、epoll10.386ms，新增方向后本窗相邻syscall gap最大8.681ms；不能与上轮不同观测作因果比较。下一候选是定位小量sendto内部长区间，需要新设计审查/授权，不能直接改产品。全部动态停止。Reviewer独立CRC/配对/端点确认，unknown/replaced/orphan0；R01415.994242秒/44725653B snapshot，R008174.193805秒/463031989B，预算内，172保护/seals/清理成立。本报告定版。

完整边界和证据：`docs/builder/reports/V0.5.1/S3-report-016.md`。
