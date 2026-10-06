# V0.5.1 / S3 Builder R002 完整矩阵结果 007

日期：2026-10-05。状态建议：**阻塞**。完整30样本执行结束，CLI exit1/status invalid，不能视为性能通过；等待Reviewer静态复核及Leader处置。

## 依据与路线

原S3 Approved revision1、R002 Approved revision1（用户“做吧”）、原审查计划，Builder006及Reviewer005。分支codex/v0.5.1-s3-performance-acceptance，起点69424e6。复用原生WSL bash及Builder独立.cache目录，TMPDIR/TMP/TEMP/XDG_CACHE_HOME均在角色tmp/cache，Python -B；仅真实HTTP采用窄提升。无新构建、生产或工具修改。Reviewer确认停止动态后执行唯一第三套，期间工具冻结。读取agent-execution-route、personal-cpp-standards入口、plan-project-docs及Builder模板；本轮没有C++设计或修改。

## 预期与实际

预期五场景三轮完整、所有有效性和数值门槛通过。实际30/30样本有效、60个预热/测量phase五类错误全零，前后审计与回收通过；但两项最终条件失败：

- P1/D三轮QPS为2467.804、2029.150、3359.488，跨度53.9077%超过20%。
- P3/D三轮P99为667.923、99.471、1184.513ms；中位667.923ms，对C中位49.136ms比值13.59335，超过0.25。P3吞吐比19.53513已过吞吐门槛，但不能抵消尾延迟失败。
- P2/P4/P5本套数值及跨度条件通过；不能将这些局部结果拼成整套通过。

|场景|QPS D/C|P99 D/C|server CPU C/D (%)|wrk CPU C/D (%)|RSS max C/D (KiB)|
|---|---:|---:|---:|---:|---:|
|P1|110.254892|0.016627|0.349/22.472|0.193/7.849|4800/4800|
|P2|52.423634|0.043045|5.793/178.753|2.975/96.825|5440/5280|
|P3|19.535133|13.593353|29.133/329.813|13.639/141.827|7360/7360|
|P4|1.012966|0.955379|96.062/98.305|106.357/108.705|5440/5440|
|P5|1.016638|0.865669|23.989/23.772|107.581/108.192|5280/5280|

CPU按单核100%，RSS为server采样/HWM，闭环负载、WSL同机热缓存；不外推其他环境。本轮P3尾延迟及P1波动的具体根因未知，不能仅由CPU数值归因。

## 历史异常与本次时间

测量wrk时长20.001261—20.100226s，套UTC763.865823s、monotonic763.871583s；本轮未触发原timeout32或计时不一致检查。历史两套异常仍为根因unknown，原始记录永久保留；本轮无异常不等于历史原因已修复。未追加clock采样或机制实验。

## 命令与证据

- python3 -B benchmark/regression/regression.py --role builder unit --output .cache/v0.5.1-s3/builder/run-unit-006：24/24通过，1.316706399s。
- python3 -B benchmark/regression/regression.py --role builder check --output .cache/v0.5.1-s3/builder/run-check-003：8/8通过，8.376014459s；冻结测试未改。
- python3 -B benchmark/regression/regression.py --role builder run --wrk .cache/v0.5-s4/tools/root/usr/bin/wrk --output .cache/v0.5.1-s3/builder/run-matrix-003：exit1，30样本，763.871583209s。
- python3 -B benchmark/regression/collect.py --role builder --output benchmark/results/V0.5.1/S3/builder-r002：保留16条历史/当前run副本及原wrk输出；大server日志保留本地并公开hash/bytes。
- 最终clock工具12反例沿用Builder006/Reviewer005已有独立证据，SHA未变，不重复唯一诊断；S2 sanitizer按D与ab9b360的app/src/include/tests/CMakeLists.txt同树继承。

原预算未重置：正式启动前651.375226874s，结束累计1415.274100188s（账本finish包含最终保存微小开销），剩余384.725899812s，不足900s整套预留。角色日志1131619975B低于2GiB。175保护文件逐字一致，包括用户10注释文件；正式套前后identity一致，r002-before至final所有脚本hash未变。

## 修改、限制与交接

只新增Builder运行/公开证据及本报告，不改根文档、代码、旧工具或原始历史记录。无接口、回调绑定或状态归属变化，相关映射不适用。公开r002-final-audit.json记录完整工具hash、保护结果、样本数值及预算；run-matrix-003/archive-run.json为原run.json精确副本。

遵照R002失败即停：没有第四套，没有Reviewer完整矩阵，没有改门槛或产品，不提交/推送。不具备关闭S3/V0.5.1/RO-002的证据。请Reviewer仅静态独立复算本套、核查P1 noisy和P3 P99原始summary以及身份/预算；后续诊断范围与预算由Leader决定。
