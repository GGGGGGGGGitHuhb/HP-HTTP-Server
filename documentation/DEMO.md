# 无数据库任务应用演示

服务器支持静态文件、原始正文 echo 和内存任务 CRUD。生产 main 将任务 JSON/存储/编码投递到有界业务执行器，静态文件与 echo 仍走同步路径。数据仅驻留当前进程，重启清空；此演示不创建数据库或持久化文件。

## 一键演示

在仓库根目录构建后执行（Python 3 标准库，无第三方包）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
python3 scripts/demo_task_application.py --server build/hp_http_server --output demo-evidence
```

`--server` 必须指向已有可执行文件；`--output` 的父目录必须存在且该输出目录必须不存在。重复运行时选择新的目录，脚本拒绝覆盖旧证据。脚本自行创建专属临时静态根和合成任务，不读取用户静态文件或真实记录。先运行静态 GET、echo、创建/查询/完整替换/列表/删除，再保留一条合成任务，停止并重启进程，确认列表为空。后续请求使用实际返回的 ID。

**监听边界：服务器仍按既有行为监听 `0.0.0.0`，端口由 `--port 0` 自动分配；只有演示客户端的目的地址是 `127.0.0.1`。** 网络可达的其他主机可能读取临时静态文件及读写无鉴权任务 API，取决于路由、防火墙和 WSL 网络。随机端口不是访问控制，请仅在确认可信的开发环境运行。脚本在启动前同样显示此说明；不会修改防火墙、创建网络命名空间或公网转发。

成功返回0；断言、启动或清理失败返回非零。证据目录保留 `summary.json`、每次服务器日志及对应 JSON，记录监听/客户端地址、实际端口、PID、退出码、进程回收、reader join、管道关闭及临时根删除。正常完成不依赖强杀；失败后也尝试有界清理。临时静态根在结束时删除，证据保留。输出目录不是持久化业务存储。

## 手工 curl 路径

在可信开发环境启动服务器，将启动输出中的实际端口填入 `base`；静态根只放合成文件。以下示例假定服务器已经启动，停止它仍由启动者负责。

```bash
base=http://127.0.0.1:8080
curl --fail-with-body "$base/"
curl --fail-with-body -X POST "$base/api/echo" --data-binary 'hello'
created=$(curl --fail-with-body -X POST "$base/api/tasks" \
  -H 'Content-Type: application/json' \
  --data-binary '{"title":"合成任务","completed":false}')
id=$(printf '%s' "$created" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
curl --fail-with-body "$base/api/tasks/$id"
curl --fail-with-body -X PUT "$base/api/tasks/$id" \
  -H 'Content-Type: application/json' \
  --data-binary '{"title":"完整替换","completed":true}'
curl --fail-with-body "$base/api/tasks"
curl --fail-with-body -X DELETE "$base/api/tasks/$id"
curl -i "$base/api/tasks/$id"  # 已删除：404
```

PUT要求两个字段，完整替换而非部分更新；HEAD任务路由返回405，不支持PATCH。任务响应是JSON并带`Cache-Control: no-store`。JSON输入最多8KiB，title为1–256 UTF-8字节且无ASCII控制字符；最多128条记录、列表最多256KiB。ID是十进制字符串，进程内递增不复用。

## 异步与失败边界

- 两个业务线程、最多32个排队任务和34个未释放任务；已完成但尚未领取的结果仍占槽。超载为503 `task_overloaded` 并关闭连接。
- 默认1秒期限从准入开始，包含排队、运行和等待owner领取；到期503 `task_timeout` 并关闭连接。5ms检查间隔不是硬实时保证。
- 超时、断连或响应发送失败不会自动回滚已经提交的修改。503不保证请求未执行，盲目重试POST可能创建重复记录；本项目无幂等键或事务回滚。
- 首次停止先关业务准入，再排空网络，网络owner退出后等待业务线程。第二次信号只强关网络，不能强杀任意C++处理器；不合作处理器可能延长join。
- 一键演示只调用真实生产路由。慢任务、排队、迟到结果及停机故障由独立测试门闩夹具验证，不存在生产慢请求调试端点。

## 组合验收入口

```bash
ctest --test-dir build -R '^(task_application_concurrency|http_application_acceptance)_tests$' --output-on-failure --timeout 120
```

综合测试包括两个owner模式的混合pipeline、静态缓存/Range、JSON并发整对象一致性、慢任务与独立连接响应、取消/超时副作用、断连后新连接、停机及部分初始化失败清理。资源上界是已实现的逻辑预算，不等于全局RSS硬限；有限测试及TSan不证明所有竞态均不存在。完整验证和版本验收状态见[开发说明](DEVELOPMENT.md)与[路线图](../ROADMAP.md)。
