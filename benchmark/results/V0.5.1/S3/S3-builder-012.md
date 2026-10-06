# V0.5.1 S3 Builder 012 — R008观测能力核验

日期：2026-10-05；依据 Approved S3-rework-008；阶段仍为返工中，正式性能结论 FAIL。

## 预期与实际

预期先验证系统调用与调度观测，再执行最多四样本ABBA。实际 native WSL 执行已恢复，但提升后的host与普通sandbox均缺 perf/bpftrace/trace-cmd，tracefs目录为空，sched_switch及sendfile tracepoint格式不可读取，sched_schedstats=0。strace微型核验成功，仅证明基本ptrace与一条动态加载read的记录；不证明业务sendfile路径、线程切换覆盖或HTTP性能。RW-02准入未满足，未启动ABBA。

没有修改生产源码、系统配置、安装工具或重跑正式矩阵。既有锁获取长等待、去除逐响应info后仍长尾及两端运输包围区间停顿的证据继续有效；区间内部原因仍未知。不能把CPU近wall解释为内核独占执行，也不能从sendfile直接判定磁盘或网络。

## 验证与预算

执行路线为native WSL及role-local tmp；ptrace首次使用窄提升，所有动态先由原batch_candidate.py --role builder check登记同一R005 ledger。微型命令为strace --seccomp-bpf -f -qq -ttt -T -e trace=sendfile,read,write,recvfrom,sendto,epoll_wait,futex -o micro.stdout /bin/true。

有效run run-r008-ptrace-preflight-001，退出0，耗时0.8010072708129883秒，无HTTP样本，127B trace。tracer PID8782/starttime3103841已回收；trace child8793短暂运行，未保存其starttime，正常退出及owned_before_cleanup为空支持已结束，不声称完整child身份采样。

独立复算：Builder累计2784.9611774438367秒，R005增量1114.2019286628565秒，剩余685.7980713371435秒；累计日志3302831304B，增量1515907764B，剩余631575884B。R008仅0.801007秒、127B trace，均在300秒/500MiB及旧余额内。无unknown/running entry，172个非例外protected文件一致，candidate descriptor/patch seals匹配。未进行额外功能或性能测试。

## 证据与下一步

能力摘要：.cache/v0.5.1-s3/leader/r008/capabilities.json；原run/cleanup/trace：.cache/v0.5.1-s3/builder/run-r008-ptrace-preflight-001/；独立复算：.cache/v0.5.1-s3/reviewer/r008/independent.json。

下一步先准备隔离的系统调用与调度观测环境，核验工具和目标事件后再跑相同ABBA；所需依赖/挂载/权限变更另行具体设计，不能在本轮禁止安装和改全局配置的范围内擅自实施。诊断执行受环境能力限制，正式S3仍FAIL，RO-002未关闭，无发布操作。
