# S4 请求时间线定位工具

> 2026-10-08归档：V0.5.1已搁置（未完成）。以下命令/批准说明仅为历史记录，当前候选非生产、非就绪入口；依赖本地冻结资料，未完成正式采集。不得按旧命令恢复，见benchmark/results/V0.5.1/SHELVED.md。

当前入口状态（2026-10-07）：**暂停执行，进行公共契约与执行链文档收敛**。统一规则见 [CONTRACT](../CONTRACT.md)，场景导航见 [benchmark入口](../README.md)。最新R026隔离验证因注入仅按`.cache`名称匹配、误命中祖先而FAIL，真实cold入口尚未启动；不得沿历史命令恢复原五槽或正式采集。后续需明确新的实施与验证范围。

以下正文保留为早期实现与接口历史记录；“M1实现中”“未运行”等描述只代表当时状态，旧阶段授权不能作为当前执行入口。历史源码、工具及证据不删除，现有各版本不同时视为活动候选。

批准基线为 `S3-rework-020`、`S4-design` / `S4-review` revision 1。
固定 E-S3 commit：`acda3f92d42a36d0b0554e185bc6f4155b4e5889`。
这些工具仅服务诊断副本，不修改生产路径；当前处于 M1 实现中，未运行自检或负载。

## M1 分解

- 授权/身份：读取 Leader authorization，独立导出固定 commit，保存文件与候选四文件 hash；保护工作树及旧 S3 ledger，不从脏工作树建立候选。
- 预算：独立 S4 ledger；每角色动态 1200 秒 / 2 GiB，Builder/Reviewer 全局串行。执行前持久化 reservation；失败、超时、未结算都收费；所有产物计数。构建耗时单列。
- 离线：`analyze.py` 已实现规范化事件配对与有符号跨端边界、嵌套区间并集、缺尾 unknown。`test_localize.py` 是拟交独立审查的 synthetic 反例，尚未执行。
- 二进制记录/完整性：待落实固定布局、计数、writer 停止与 overflow latch；当前 normalized JSON 不是生产采集完整性证明。

## M2 分解与缺口

- server：在本角色 export 上添加诊断 observer；四 worker 各 16 MiB + main 4 MiB，预分配/预触页，线程自有写游标，无热路径文件写入或 Logger 依赖。
- client：原 wrk 同进程保持连接，预热前冻结最多 16 连接及两端四元组/生命期/worker 映射；预热结束 reset 统计但请求序号连续，保持原 closed-loop 算法。完整 Content-Length/body 审计，禁止重连/重试/pipeline。
- 首次全仓含 ignored 搜索仅找到历史不完整 wrk.c/stats.c/h，不能证明来源一致。随后 Leader 按批准范围取得与封存 Ubuntu binary 同版本的官方 dsc/orig/debian archives，并按 R001 最小追加批准取得同版本开发头文件与 luajit 字节码工具，均只缓存解包，不系统安装。Builder 已独立复制/解包源码并应用 Debian patch；尚未构建或运行，不能把输入补齐称为算法/协议已核验。

## 分析契约

事件键为 run ID + TCP 四元组 + connection lifetime + request sequence；PID/starttime/TID/worker 必须与预热前冻结 metadata 匹配。fd 仅辅助。
使用同 boot/time namespace 的 CLOCK_MONOTONIC ns。`write_complete→server_first_read` 和 `output_drained→client_complete` 可负，保留原值；不把它们当可相加独立组件。
raw `client_complete-write_begin >= 50ms` 才是 slow request。残差为 unknown；校正直方桶不能替代原请求。

首次自检/离线执行须 Reviewer 静态准入、封存 hash、Leader 新 S4 ledger 预登记后执行。
