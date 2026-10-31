# 启动配置与本机管理

配置只在启动时读取；不支持热加载、include或环境变量插值。优先级为默认值 < `--config <file>` < 显式CLI，CLI位置不影响优先级。文件和CLI分别校验，CLI不能掩盖文件错误；重复键或同一CLI逻辑选项重复均报错，包括一正一反布尔开关。

文件仅普通文件，最大65536字节（读取期间增长也受限），严格UTF-8、无BOM的JSON单层对象。拒绝未知键、重复键、嵌套、数组、注释、尾随文本、null、浮点与负整数。root路径非空、最多4096 UTF-8字节；文件与CLI的相对root都相对进程启动cwd。最终必须提供port与root。配置错误在创建logger/线程/listener前exit2；实际root打开、绑定或线程启动失败exit1。仅单独`--help`退出0且不读取配置。

| JSON键 | CLI | 范围/默认 |
| --- | --- | --- |
| port / root | --port / --root | 必需；port 0..65535，0为随机端口 |
| threads | --threads | 0..64；默认2，0为单Reactor |
| idle_timeout_ms | --idle-timeout-ms | 0..86400000；默认30000 |
| keep_alive_timeout_ms | --keep-alive-timeout-ms | 同上；默认15000 |
| request_read_timeout_ms | --request-read-timeout-ms | 同上；默认10000 |
| shutdown_timeout_ms | --shutdown-timeout-ms | 0..60000；默认5000 |
| access_log | --access-log / --no-access-log | 布尔，默认false |
| metrics_on_exit | --metrics-on-exit / --no-metrics-on-exit | 布尔，默认false |
| admin_port | --admin-port / --no-admin | 缺省禁用；0..65535启用，0为随机回环端口 |
| max_connections | --max-connections | 1..4096；默认128 |
| input_budget_bytes | --input-budget-bytes | 1081344..1073741824；默认67108864 |
| output_budget_bytes | --output-budget-bytes | 1024..1073741824；默认67108864 |

连接/输入/输出是S1逻辑预算，不是RSS上限。输入最小值可容纳一个连接窗口和一个完整请求预留，不保证全部连接同时活跃；低输出预算可能拒绝合法大文件，沿S1静默EOF/reset。任务存储128条、单title256字节、执行器2 running/32 queued/34 outstanding及每消息/文件上限仍固定。

在仓库根目录运行：

```sh
./build/hp_http_server --config examples/server-config.json --admin-port 0
```

示例root为仓库的`www`；按实际站点目录调整。stdout保持原业务就绪行；启用管理时追加`HP_ADMIN_LISTENING 127.0.0.1 <port>`，端口0读取该实际值。管理监听实际绑定IPv4 127.0.0.1，业务仍监听0.0.0.0。管理无鉴权，本机任何可连接进程都能访问，不是多租户安全边界。

管理在独立单Reactor线程运行，独立8连接/16MiB输入/256KiB输出预算、2秒读取及idle期限；不能通过配置覆盖这些值。每响应关闭连接，正文最多32KiB，唯一`Cache-Control: no-store`及`Content-Type: text/plain`。不读取静态root、任务标题或请求正文内容用于指标；请求不计入业务HTTP/连接/访问日志和resource统计。

- `GET /healthz`：200 `ok\n`，仅表示管理loop能响应。
- `GET /readyz`：启动/停止503 `not ready\n`；业务listener已启用且未停止时200 `ready\n`。不是容量、依赖健康或业务时延保证。
- `GET /metrics`：200现有业务/日志/资源整数键快照，没有退出BEGIN/END标记，不宣称Prometheus格式合规。各模块分别线程安全，跨模块不是同一时刻事务快照。

仅原始目标精确匹配，query及编码别名404；先路径后方法，已知路径非GET405且`Allow: GET`，HEAD405不发正文。GET完整非空body400；Expect头阶段417close，不发100；更早parser错误仍沿原协议错误格式。

首次SIGINT/TERM先撤ready再业务drain。管理在业务网络退出前仍可返回not ready；业务网络退出后先停止/join管理，再join业务执行器与日志消费者。配置/启动失败不留下半服务；握手1秒是等待启动结果的期限，超时后仍合作stop/join，不是任意线程1秒强杀。管理端口已关闭也不意味着进程已经退出。
