# R008 四步恢复执行计划

批准权威：docs/leader/reworks/V0.5.1/S4-rework-008.md，review-amendment-001；固定产品acda3f92d42a36d0b0554e185bc6f4155b4e5889。旧smoke001/002仍invalid，无额外手工HTTP探针或重试。

所有角色每步一次，依次A/B/C/D；上一真实capture与独立offline关联校验均valid后才能下一步。任何失败、HTTP/body错误、首因槽非空、overflow/丢失、线程身份/clock漂移、回收失败停止；未执行步骤不能充重试。M2四步不产生QPS/P99或真实≥50ms定位结论。

## 固定命令共用前缀

WSL native /home/power/projects/HP-HTTP-Server；每角色TMPDIR/TMP/TEMP为`.cache/v0.5.1-s4/<role>/tmp`，XDG_CACHE_HOME为对应cache，PYTHONDONTWRITEBYTECODE=1。外层localize_v10使用自己的ledger、1200秒/2GiB及R008子额，完整30秒含7清理。静态inventory SHA须取本角色实际封存值，不复用Builder可变state。

`/usr/bin/python3 benchmark/tail-localization/localize_v10.py --role <role> --run-id <fixed-id> --kind r008_link --seconds 30 --static-inventory-sha256 <actual-own-role-seal> -- <endpoint-entry>`

A `run-r008-link-once`：process_local_launcher_v3.py --role <role> --output <own-role-run> --manifest <own-sealed-v3-inputs> --requests-per-connection 1。2连接、无预热、各1请求；不开root/tracefs，socket按权限规则窄提升。

B `run-r008-link-repeat`：同入口，requests-per-connection16。2连接、各16请求，无预热；无重连、连接生命期不变。

C `run-r008-link-mapped`：外层明确Windows WSL root桥接，运行root_marker_launcher_v3.py --role <role> --output <own-role-run> --manifest <own-sealed-v3-inputs> --seconds30 --connections8 --requests-per-connection16 --warmup0 --duration1 --detailed。保留私有startup marker/systemevents关闭、load降权power；duration1仅固定模式控制结构名义窗口，不是停止条件，真正停止由每连接16完整响应组成。

D `run-r008-link-128`：同root入口，connections128/requests-per-connection0/warmup1/duration3/detailed。selected4覆盖4server/2clientworkers，两端完整buffer与body审计，窗口尾未完成保留边界。

root bridge必须明确sealed LD_LIBRARY_PATH/LUA_PATH及role TMP环境；root时间起点绑定外层running ledger，不重新计时。normal filesystem/process writes属于各自role资源，所有root仅私有自有instance且仅startup marker。

## 每步offline准入

capture valid后以单独离线记录预算：`localize_v10 --role <role> --run-id run-r008-decode-<once|repeat|mapped|128>-001 --kind offline --seconds15 --static-inventory-sha256 <actual-own-role-seal> -- python3 decode_v3.py --input <sample-run> --output <sample-run>/association.json --seconds6`。

离线没有socket。输出association.json为新证据，旧capture文件/ledger不改；成功receipt schema3/statusvalid与当前二进制、identity_scope相符。预算后继在下一步骤前核前置sample ledger valid和receipt；Leader/Reviewer另外核计数/identity/回收实际材料。每次外层15秒包含7秒清理、8秒工作，inner最多6秒；四次离线60秒明确保留。106.351055已用+Debug20+真实120+离线60=306.351055<360，M3预留570秒保持。receipt必须核run/role/sample原字节SHA/manifest规范JSON SHA、固定scope/selected/quota、complete、overflow=false、cleanup及raw catalog SHA；失败停止而非换ID续跑。

## 新候选身份

最终输入manifest在run004实际完成后静态生成；Builder/Reviewer显式role各有独立source/binary/summary/命令，runtime官方只读封存DSO可共享。v3controller拒绝跨role二进制/summary及未知control schema，不复用Builder可变构建作为Reviewer证据。所有哈希等待实际产物，不在本计划写占位SHA当结果。
