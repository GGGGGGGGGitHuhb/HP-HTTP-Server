# V0.5.1 / S3 Reviewer 017 — R016同期perf与sendto观测

2026-10-06，唯一阶段结论：**FAIL**。Approved R016补充基线下原未用一次3+6秒观察有效；R015原cap invalid及失败历史保留。原R005有效30样本矩阵中P3 corrected P99 E/C=3.2446仍未达≤0.25；未修改产品，没有新控制或能力重试，正式S3不得关闭。

独立静态审查实际final SHA、单server worker marker kernelTID与perf namespace TGID/TID关系、99Hz CPU_CLOCK/mono、ring布局与屏障、LOST/THROTTLE/unknown、同期boot/starttime/core text上下界及清理。身份finish核验在disable/drain之后、symbol lookup之前，分别记录时间，避免自然退出误判。模块/歧义symbol未知与记录丢失分开；采样不足不证明宿主暂停。

`run-r016-production-verify-001`原Reviewer ledger精确wall1.1170287132263184秒，独立合并核验trace完整gzip/CRC/hash/配对及perf原始布局、身份、时间、symbol区间。**67021完整sendto、221原始内核样本、无未知解析/丢失、trace entries0/排空成功、perf head/tail完整、boot及身份稳定**。两时间线交集43568.232322703→43573.066335693，首尾未知保留。证据：`reviewer/r016/production-independent.json`，perf raw SHA `5f6b641d26e25a6b30fbea6371820073637028b3b1caa0d195c04b0c93d198c2`。

本轮最长sendto **7.058ms**，完整entry43570291419000→exit43570298477000，105B成功；没有复现R014的240.784ms。窗内唯一perf点43570298417788，在entry后6.998788ms/exit前59.212微秒，同期core符号为rb_insert_color，链经tcp_event_new_data_sent、tcp_write_xmit、__tcp_push_pending_frames、tcp_push、tcp_sendmsg_locked、tcp_sendmsg、inet_sendmsg及__sys_sendto。独立raw与映射核实该时点在TCP发送路径；不能说7ms全耗在红黑树，不能将guest采样推为exclusive物理CPU或排除宿主暂停。未复现限制阻止解释旧240ms根因。

最长相邻sendto gap **70.166ms**（43568.917734→43568.987900），完整off-CPU70.039ms，其中非R至wake69.939ms、runnable0.100ms。该同TID probe的syscall字段43568.976988280→43568.977106806完整处于gap内，读到232；实际x86_64 header确认232=epoll_wait，epfd9/maxevents0x40/timeout0x3e8。stack字段43568.977323671→43568.977424981亦在gap内，含do_epoll_wait/__x64_sys_epoll_wait。支持**该gap内这一采样位置执行epoll_wait**；各字段非原子，stat R/wchan0不构成反证，也不能将整个gap归为epoll、锁或正常空闲。原R014/R013不同观测不是同一次事件复现。

最后独立 `independent.json`/`final-stage.json`：Buildercharged2960.801495706974、Reviewer198.1895751928414秒，unknown/running为空；共享R015/R016动态15.394605659283116秒/文件snapshot42975277B，≤40秒/48MiB；R008累计189.58841111113725秒/506007415B，≤300秒/500MiB，旧R005余额仍足。snapshot后少量报告元数据另记，不重置预算。172非例外protected项无变化，descriptor/patchseal一致；owned为空、driver回收、perf FD/mmap关闭、私有instance/mount移除及默认状态恢复成立。

全部dynamic停止。完整同期观测、TCP路径采样点和gap内epoll位置是具体已验证成果；旧长sendto未复现、稀疏采样及未观察机制阻止根因定论或生产修复。后续只应围绕可区分机制的新批准设计推进，阶段同步交Leader。

收口核对：Builder018/public018摘要与独立证据和预算一致，正式FAIL与本轮有效观测成果分别保留，报告定版。
