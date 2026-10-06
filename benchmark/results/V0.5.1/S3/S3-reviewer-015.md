# V0.5.1 / S3 Reviewer 015 — R014服务器调用定位

2026-10-06，唯一阶段结论：**FAIL**。Approved R014观察与完整时间线验证成功，但原R005有效30样本矩阵中P3 corrected P99 E/C=3.2446仍未达≤0.25，产品根因未闭环。本轮只观察一次3+6秒样本，无单独容量微型或新增对照；不能用旧控制样本宣称改善。

按Reviewer/route规范静态审查单server首worker实际marker映射及私有tracefs、4096KiB buffer、真实sendfile64/read/write/recvfrom/sendto/epoll_wait/futex格式与scheduler过滤、完整排空及期限。recvfrom/sendto对应ConnectionIo::recv/send真实方向，保留eventfd read/write。Decoder未知行failclosed、首尾边界未知、窗内配对严格；root22秒加有界清理、两份离线各4秒在共享40秒内预留。未修改生产代码或正式门槛。

原Reviewer ledger `run-r014-production-verify-001`精确wall2.9601221084594727秒，独立完整gzip/CRC/hash/解析/配对核验：raw49889042B、468401行、unknown0、replaced0、orphan0，loss0/entries0/drained=true，身份稳定。完整pairs与Builder一致：futex34966、sendto56841、sendfile64 56841、recvfrom56840、epoll_wait10644；read/write未出现完整调用，不等于遗漏或可排除对应机制。coverage41376.005156921→41380.871455206，尾端epoll_wait及调度边界保留未知，不延长窗强求配对。

关键独立定位：完整sendto 41376.512510→41376.753294，**240.784ms**，fd0x8e、len0x69、flags0x4000、ret0x69；进入/返回同TID且完整配对，没有对应完整switch-out。支持“选定线程该sendto跨越长时间”的具体事实，不能认定pureCPU、内核忙或排除宿主暂停。R013的138/123ms sendfile间隙不是本轮同事件复现。本轮最长调用间隙约8.681ms，仅限当前覆盖窗。

最长完整futex **7.868ms**，uaddr0x6177ba63a940、op0x80、val2、ret0；完整off-CPU7.858ms，其中非R至wake7.234ms、runnable0.624ms。证明该地址发生等待，不证明它是Logger mutex，也不能据此将240.784ms sendto归因为锁。等待地址到对象映射、单请求归属、其他workers/client与宿主暂停仍未知。可证伪方向是检查长sendto期间guest/host推进及该等待地址的实际所有者；进一步验证需另行批准，不能直接修生产代码。

独立 `reviewer/r014/{production-independent,independent,final-stage}.json`：Buildercharged2946.731878651283、Reviewer196.86458658924948秒，无unknown/running；R014新增15.994241714477539秒/文件snapshot44725653B，≤40秒/64MiB；R008累计174.19380545185413秒/463031989B，≤300秒/500MiB，旧R005余额仍足。snapshot后少量报告元数据另记。172个非例外protected项无变化，descriptor/patchseal一致；相关进程回收、owned空、instance/mount移除、默认状态恢复及cleanup无错误成立。

所有dynamic停止。观测与调用定位是已验证成果，正式性能验收仍FAIL，S3不得关闭。下一方案必须区分上述候选，保留无对照、覆盖窗和地址未知限制；状态同步由Leader负责。

收口核对：Builder016/public016摘要一致。其sealed decode的syscall_top10复用了gap字段名：exit_monotonic实际为调用entry，next_enter_monotonic实际为调用exit；这是输出字段语义缺陷，原证据保留。独立enter_ns/exit_ns确认上述sendto真实端点和240.784ms配对正确，不能把字段命名缺陷解释为根因或配对错位。
