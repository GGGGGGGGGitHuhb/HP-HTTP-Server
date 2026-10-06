# V0.5.1 / S3 Reviewer 018 — R017宿主metadata超时

2026-10-06，阶段结论：**FAIL**。Approved R017唯一宿主只读metadata查询invalid，未取得有效能力结论；不把查询期限不足解释为provider不存在或系统不支持。原正式P3失败保持，产品根因未知。

独立静态审查固定EncodedCommand、NoProfile/NonInteractive、UTF8、NULcache、声明可写Windowsnative小temp、只集合类别与固定provider注册metadata。没有实际counter值、其他VM实例、events、ETW会话或管理员操作。自有Windows supervisor按自身StartTime+5秒期限、双异步pipe、自有child超时回收；WSL6秒+1秒异常回收，文件含native temp计入1MiB。

只读实际summary确认：`builder/run-r017-host-metadata-001`账本耗时5.333848237991333秒；parent return0，但own child timed_out=true/forced=true、child_exited=true、exit_code=-1、cleanup_error=null。interop_reaped=true；native temp0B/保留文件0。parent正常退出不等于查询成功，summary.success=false正确。没有执行期待成功的结构checker，没有重试原查询或新增负载。

原查询/script/hash、stdout/stderr、生命周期及失败均保留。R018需独立Approved更窄基线后才能一次原生provider查询，不由此失败自动放行；同R01540秒/48MiB与R008300秒/500MiB额度不重置。能力注册即使后续存在也不证明目标VM调度事件已取得。正式S3仍FAIL、不得关闭。
