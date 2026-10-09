# V1.0 / S3 Reviewer R002 有限诊断

依据：Approved R002 revision1。本次只完成诊断契约，S3唯一结论仍为**FAIL**，原正式结果3valid/1invalid/5NotRun不变。

- 唯一M6：原Release SHA `9f95b5f3b31062737032e0ce130dfd725a4e9f923ec96524b3006ccbb2ac7530`；1MiB、workers2、idle30000ms、keepalive15000ms、shutdown5000ms，metrics-on-exit。一个server、一个socket、三个顺序GET；无wrk/构建/CTest/sanitizer/重跑。
- 三个200、Content-Length/正文SHA/keepalive/无尾字节均通过；未复现EOF。指标started/completed/200各3、connection1、aborted/errors各0、active/logger_pending0。不能因此关闭原EOF或宣称已修复。
- 唯一动态消费36.380733385s。正常TERM、exit0、forced=false、owned0；局部RAC-D01成立。旧Builder两批失败和Reviewer001/002原记录保留。
- UTC起点2026-10-09T04:50:50.929431+00:00，Reviewer原1800s连续截止2026-10-09T05:20:50.929431+00:00；结算后等待继续计时，不自动授权余量或重置时钟。
- 原冻结scan精确AST SHA `ddcaa27201b79c03de742fa32e6b0cb4b5d0714c17905ded5dc1c63b6de24b04`，每原guard一次，共55次。累计旧scan 35.859516946s，新根计费0.027467897s；最长scan0.785739429s，guard最长间隔0.787215708s。真实监控未超过1s；本次结果不保证后续完整batch间隔。
- 单请求客户端send至complete约11.612/10.958/10.637s；最长两recv之间暂停0.787s。server三请求累计latency988587us/max850645us为聚合口径，无法对应每个响应完成时间。本次没有测得单次暂停或请求完整读取跨15s。
- trace持久化最长0.000289431s；同步scan占诊断绝大部分时间，观察者成本已确认，但原EOF因果仍未知。
- 合并计费最大211767296B、真实capture最大753664B。两根不相交，全路径capture/输入分类与失败finally局部治理通过；oldroot1921文件、210219008B前后SHA/bytes完全相同。旧oversize输入保留total-only边界。

扫描输入为失败清理后快照，原payload已删、尾部日志已落；其计费比Builder最终记录多28672B。原失败瞬间没有完整文件清单，不能复原逐文件差异或称本次为原现场复现。新根计费及增量trace也增加观测成本。真实EOF分支未触发，不能声称已动态覆盖EOF现场快照。

[结构化结果与完整增量时序](reviewer-diagnostic-001.json) · [一次性诊断源文本](reviewer-diagnostic-001-source.py)。大日志/前后全文件清单留独立Reviewer cache；源文本仅为此轮证据，不是常规入口。

后续需另行批准编排修正：固定canonical根与生产者分类、避免每64KiB全树扫描，以统一实际≤1s巡检及单次读取2s语义协调HTTP等待；保留计费/停止/cleanup与增量证据。该优化解决实测扫描干扰，不等于证明EOF根因。之后才可批准两角色各一新1800s完整验收，历史账保留。RO-002/TD-001/TD-006仍Open，WSL2及冻结覆盖边界不变。
