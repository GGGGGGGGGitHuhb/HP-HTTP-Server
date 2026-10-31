# 非特权部署示例与本地验收

本页与`examples/systemd/hp-http-server.service`、`examples/deployment/server.json`配套。示例用于运维者复核后手动部署；仓库验收不会创建系统账号、安装服务、修改/etc/防火墙、运行TLS代理或申请证书。

## 路径与权限

- 专用普通用户/组：hp-http，不授予capability，不使用root运行进程。
- 二进制：`/opt/hp-http-server/bin/hp_http_server`。
- 配置：`/etc/hp-http-server/server.json`；默认值 < 文件 < CLI，各来源独立校验，见[配置说明](CONFIGURATION.md)。
- 工作目录：`/srv/hp-http-server`；静态根：`/srv/hp-http-server/www`。
- 业务8080（实际0.0.0.0）、管理9090（实际IPv4 127.0.0.1），不需要CAP_NET_BIND_SERVICE。

部署者应先建立上述普通账号及目录，确保站点由root或部署账号维护，服务用户只有读取/逐层搜索权限、没有写权限。可采用root/deployer所有、hp-http组，目录0750、普通静态文件0640；二进制还须可执行，例如0750。所有父目录均须允许搜索。不要chmod777，也不要把站点写权限交给处理HTTP的进程。

配置正确但root无法打开时启动exit1；已打开root下合法不可读文件403。符号链接被拒绝，不提供目录列表。任务只在内存里，重启清空；本阶段不提供业务鉴权和持久化。

运维者在真实目标机核对路径、账号、权限、配置、外部隔离与日志策略后，可手动执行以下步骤；这些命令不是本地验收工具的行为：

```sh
# 先由部署者按上文建立账号/目录并安装二进制、站点和配置。
# 仅在真实systemd主机上，复核示例后安装unit：
sudo install -m 0644 examples/systemd/hp-http-server.service /etc/systemd/system/hp-http-server.service
sudo systemd-analyze verify /etc/systemd/system/hp-http-server.service
sudo systemctl daemon-reload
sudo systemctl start hp-http-server.service
sudo systemctl status hp-http-server.service
# 确认health/ready及停止策略后，是否enable开机启动由运维另行决定。
```

## 启动、停止与重启

Type=exec确认二进制成功执行，不代表HTTP ready；应用没有sd_notify能力，不使用Type=notify。真正就绪由管理`/readyz`观察，readiness也不保证剩余容量。[systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)

示例Restart=on-failure、RestartSec=2s，60秒窗口最多3次启动；配置错误exit2由RestartPreventExitStatus=2阻止自动重启。KillSignal=SIGTERM和KillMode=control-group配合15秒停止超时；不通过ExecStop重复发信号，不支持ExecReload/SIGHUP或socket activation。

**SendSIGKILL=no是有意选择**：超过15秒仍可能有进程停不下来，残留进程可能阻止重启。15秒不是应用退出保证，也不能用强杀通过来证明优雅退出。若实际运维选择强制回收，须另行决定并接受丢失在途请求/日志的代价。[systemd.kill](https://www.freedesktop.org/software/systemd/man/latest/systemd.kill.html)

应用首次SIGTERM撤ready后排空网络；网络结束先关闭管理线程，再等待业务worker与日志consumer。不合作处理器或阻塞日志sink仍可能卡join；管理口关闭不能当作进程退出。NoNewPrivileges/PrivateTmp/ProtectSystem=strict/ProtectHome/UMask与空capability集合是示例硬化参数，本地普通进程验收没有验证这些systemd机制实际生效。

## 日志与HTTPS边界

stdout/stderr均交journal，但应用控制协议仍须按原stdout来源消费；不要把合并诊断流当HP_METRICS协议。journald的轮转、速率限制、持久化由外部运维配置，不能保证日志无损。应用仍有界异步stderr，不内部轮转、不SIGHUP重开；直接重定向到文件后rename不重开、copytruncate可能丢行或重复，详见[运行说明](RUNNING.md)。

HTTPS示意为client → 独立TLS反向代理 → HTTP8080。本阶段不交付具体代理配置、不运行代理、不验证证书。运维需协调证书生命周期、Host、连接复用、超时及正文上限；管理9090不应被代理。不要将X-Forwarded-*当作本项目已有鉴权或可信真实客户端能力。

业务仍监听0.0.0.0。代理upstream使用127.0.0.1并不会阻止其他机器直接访问8080；真正阻止绕过代理需要外部防火墙或网络空间等另行部署。本阶段未做该隔离，随机端口也不是安全边界。管理回环无鉴权，任何能连接的本机进程可读指标。

## 本地验收及证据等级

从构建所用的同一干净源码运行标准库工具；使用可信开发环境和非root UID。路径必须绝对，artifacts父目录已存在，目标目录尚不存在：

```sh
python3 scripts/verify_deployment.py --server /absolute/build/hp_http_server --artifacts /absolute/task-dir/deployment-check
```

工具只在指定目录生成0700临时目录/0600合成文件与配置，业务/管理均随机端口。实际验证静态GET/HEAD、文件000→403、root000→启动exit1、坏配置exit2、健康/ready/指标、任务创建/列表、首次SIGTERM正常退出和完整退出指标，再重新启动并验证任务清空。文件权限finally恢复后删除临时root；进程PID、退出、双reader、pipe和独立stdout/stderr诊断保存到summary.json及对应文件。正常stop最多10秒，本地网络drain1000ms；任何最后安全强杀均记录forced并令验收失败。root UID直接BLOCKED/非零，不伪造root也会被mode拒绝。

内部三故障为Popen前、已Popen未读端口、读端口并实际HTTP后；原异常与cleanup异常分列。`--negative`在完整有效链结束后故意错误预期，必须非零且清理完整。单操作5秒、suite120秒、每流16MiB上限。工具不读取安装目录、不修改系统服务。

systemd仅做原unit字段和配对JSON核对，再生成只改ExecStart/WorkingDirectory/User/Group的重定位副本，保存mapping与diff，调用systemd-analyze verify。副本使用当前真实binary/config/非root账号与任务路径；通过不代表原绝对路径已安装、专用hp-http账号存在或硬化生效。规划时普通沙箱PID1为codex，本轮受控提升实测PID1为systemd；两者均仅普通本地进程实测，未安装或运行unit；真实systemd启停/重启、服务账号隔离、TLS及公网部署均未实测。

版本回归必须从HEAD+纯阶段patch重建完整干净源（不依赖.git/用户暂存/cache/docs），在干净源码内部`.stage-build/`新建四配置（兼容旧benchmark输出路径校验，源码清单排除该构建子树）：Debug/Release各37、规定ASan25、既定setarch TSan5；同时运行干净Release原任务demo及新工具negative。独立Reviewer通过前不标V1.4验收完成，合并/标签单独确认；旧性能/EOF/WSL2/长尾和慢sink风险保持，不新增性能结论。

### V1.4/S3 最终验收检查点

2026-10-31 V1.4/S3独立Reviewer002 PASS，P2初始化清理缺陷关闭，累计纠正2/2，无新增债务。最终干净r2独立Debug37/37（35.50秒）、Release37/37（34.93秒）、ASan+UBSan/LSan25/25（31.95秒）、限定setarch TSan5/5（3.51秒）、Release demo exit0、新负对照exit1；24个服务器及2个demo进程完整回收。S1–S3阶段验收全部完成，不表示S3已合并、打标签或生产发布。原Debug35/37与Reviewer001 FAIL保留，人工RST不冒称原生产RST根因复现。

仅静态unit字段/重定位副本与非root普通进程实测，未安装运行systemd服务或TLS代理。业务全接口/管理回环无鉴权、readiness非容量、外部轮转、网络drain非进程退出硬截止以及TD006/WSL2/长尾/EOF限制保持；逻辑配额不等于RSS，无性能新采样。
