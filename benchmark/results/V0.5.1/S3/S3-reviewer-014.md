# V0.5.1 / S3 Reviewer 014 — R013有效观测与有限诊断

2026-10-06，唯一阶段结论：**FAIL**。按Approved R013、R012剩余同账额度及Reviewer/route规则复核。R005有效30样本矩阵保留，P3 corrected P99 E/C=3.2446仍未达≤0.25。**本轮校正准入与选定线程完整时间线已验证有效**，不是仅CRC通过；尚未证明产品根因或完成正式返工。

Reviewer静态核对final root/power SHA、4096KiB请求buffer、300批×300调用、实际批起点间隔至少10.1ms且不catchup；批内仍有突发，平均≤30000/s不等于瞬时速率限制。原账本校正微型及独立checker通过：sendfile/read各90000完整对、全部ret1024、CRC/hash一致、loss0/entries0及排空成功、实际约29452/s。gate边界read保留未知。仅一对control/observed各3秒预热/8秒测量，无新增HTTP；QPS/P99任一>20%标扰动，实际corrected P99 A/B为248.374/68.457ms，变化约72.44%，不能区分观测扰动与既有波动或外推性能收益。

独立 `reviewer/r013/production-independent.json` 核验wall4.01905632019043秒，CRC/hash/count/逐TID顺序与窗内配对一致：server sendfile64完整77192对、client read完整178330对，loss0、entries0、排空成功。首个未见enter的退出、末pending及未闭合调度边界均保留未知；其他workers、锁地址、未捕获futex/epoll及单请求归属未知。完整调用最长server15.322ms/client8.987ms，没有对应off-CPU记录；不能称pureCPU或排除宿主暂停。

独立 `gaps-independent.json` 经同ledger1.8103206157684326秒复算间隙数量、top10端点/时长，server77191/client178330间隙与Builder一致。最长server137.954ms：完整off-CPU137.736ms，非R至wake137.730ms、runnable0.006ms；client27.676ms中非R至wake27.331ms。另server123.350ms仅off-CPU0.057ms。支持两类可证伪方向：未捕获调用中的睡眠等待，以及guest时间线未记录相应switch-out的长间隙；不能合并归因于同一把锁或直接解释磁盘/网络。

最近137.954ms gap probe跨度约99.675ms，stat/wchan/stack顺序读取并非原子；stat S但wchan0/stack空不能定位锁。其他probe出现futex_wait_queue只证明等待曾存在，不能建立该gap的锁地址因果。最后小文件包围窗摘要：137.954ms gap包围378.020ms，guest runtime增量156.314ms、窗外240.066ms；123.350ms gap包围289.080ms，增量238.355ms、窗外165.730ms。宽包围窗含其他工作，guest计数不是exclusive物理CPU，schedstats关闭时零queue也不能排除等待。本轮只提出方向；单机制区分验证和生产修复需后续批准。

独立 `independent.json`/`final-stage.json` 最后静态复算：双方R012/R013动态46.22831757704262秒、文件snapshot92660808B，≤60秒/120MiB；R008累计158.1995637373766秒/418306178B，≤300秒/500MiB。snapshot后少量报告元数据另记。Buildercharged2933.697759045265、Reviewer193.90446448079秒，旧R005余额仍足，unknown/running为空；172个非例外protected项无变化，descriptor/patchseal一致。相关进程回收、owned空、私有instance/mount移除、默认状态恢复成立；没有改生产代码或重宣称功能测试验收。

所有dynamic停止。观测覆盖和准入是具体完成成果；P3正式失败、72%扰动、未观察机制与请求归属限制阻止性能关闭或根因确定。S3保持返工中，状态同步交Leader。

收口核对：Builder015/public015摘要与独立证据一致；R014仅Draft、未执行，不认定锁已确证。阶段FAIL与本轮有效准入/时间线成果分别保留。
