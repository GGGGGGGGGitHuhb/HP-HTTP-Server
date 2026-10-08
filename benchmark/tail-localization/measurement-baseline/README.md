# Measurement baseline v1

本包用于 R015 的独立测量基线，不代表旧 S3 长尾原因已确认。包由固定 E-S3 commit、官方 wrk 与 Debian patches、已核验 LuaJIT 原料和本包诊断代码构成。`inputs-lock.json` 封闭全部文件、来源、许可与系统工具边界；运行不搜索旧项目缓存。

所有执行由角色独立目录中的外层治理入口授权和计费。顺序为 build180 → check60 → smoke20；Builder 再执行 baseline45，最后 offline30。任何失败停止，没有自动重试。build 在 native 与 relocated 两个路径独立编译；真实样本使用 relocated 包与二进制。

入口参数：

- `build_package.py --package-root PACKAGE --output-root BUILD_OUTPUT`
- `check_package.py --package-root PACKAGE --build-output BUILD_OUTPUT --output-root OUTPUT --governance-test TEST --governance-test-sha256 SHA --governance-module-sha256 SHA`
- `execute_sample.py --role ROLE --run-id RUN --output-root OUTPUT --build-output BUILD_OUTPUT`
- `verify_sample.py --sample-dir SAMPLE --output-root OUTPUT`

入口还要求外层绑定 role/run、真实 governor PID/starttime、package SHA、工作与清理绝对期限及允许输出路径。上述参数不是绕过治理的直接运行许可。

统计定义见 `SCHEMA.md`：全部 128 连接就绪后发布共同 MONOTONIC 窗口，完成时间位于半开测量窗的已验证响应进入 N/raw，QPS 分母固定为窗口长度。跨暖身请求、窗口结束时在途请求、结束后的完成及截尾单列。真实请求错误使样本 invalid。

校正保留原 `stats_correct` 与 `stats_percentile`，包括校正后仍从 raw min 扫描的行为。因此某些正延迟分布的 corrected percentile 会返回 0；不会替换为常规分位数。CPU/RSS 为真实 PID/starttime 对应的有限采样 envelope，不声称测量窗完整 CPU 或精确峰值。

每角色新增材料总额 512 MiB：包、双路径构建与重定位 320 MiB，正式样本 128 MiB，其余检查、冒烟和离线 64 MiB。全量直方图或慢请求证据超过容量即 invalid，不能删桶或截断后宣称完整。
