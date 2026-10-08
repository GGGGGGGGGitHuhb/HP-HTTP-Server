# baseline-v1-map / request-boundaries-v1

R018共同包契约。旧baseline-v1统计模块字节保持。所有时间为CLOCK_MONOTONIC ns，四元组方向client-local→server-local，地址字段client_address_u32/server_address_u32为host-order IPv4整数，port为host整数。fd仅辅助，禁止按fd或最近时间关联。

每worker独占524288条记录，每条32字节小端：connectionId:u32、flags:u32、sequence:u64、R_ns:u64、D_ns:u64。flags=1为R-only，flags=3为R+D；连接ID在worker内从1递增，seq在连接内从1递增，预热不reset。R-only仅连接末槽，D=0。

boundary-worker-N.bin头尾各96字节小端：magic[8]（HPBOUND1/HPBTAIL1）、version:u32=1、worker:u32、pid:u64、tid:u64、process_starttime:u64、record_count:u64、completed:u64、incomplete:u64、overflow:u64（0/1）、writer_stopped:u64（0/1）、clock_id:u64=1、reserved:u64=0。头尾除magic完全相同；文件大小192+count*32。join后导出，缺尾/写失败/overflow/stopped!=1 invalid。

boundary-worker-N.json顶层schema=request-boundaries-worker-v1、worker、pid、tid、process_starttime、record_count、completed、incomplete、overflow(bool)、writer_stopped(bool)、invalid_reason（valid时空）、clock_start、clock_stop、connections。每connection包含connection_id、fd、client_address_u32/client_port/server_address_u32/server_port、opened_ns/closed_ns、last_sequence、tail_request_parsed(bool)、partial_eof(bool)。关闭后元数据保留。worker文件没有伪造run_id；其归属由sample catalog和实际PID/starttime绑定。

boundary-export-status.json含schema=request-boundaries-export-v1和workers[{worker,exported(bool)}]。B恰四worker全部true；写入、文件关闭或目录关闭失败由实际服务端失败状态及runner独立拒绝。B始终采集，输出环境变量只决定导出目的地。

client-map.json顶层schema=baseline-v1-map、run_id、pid、process_starttime、clock_start、clock_stop、connections。每connection含owner/life/fd、同向四元组整数、connected_ns、final_sequence，与client.json终态完全相同。两侧clock_start/clock_stop均含clock、boot_id、time_namespace、resolution_ns、observed_ns、process_starttime；启动与停止实际自己读取，namespace/boot/resolution/身份必须一致。sample.processes中owned启动双读另含clock_identity，不能替代停止证据。

正常T1部分发送且NeedMore+EOF末槽允许R-only：必须client真实active、full_sent=false、0<sent_bytes<request_bytes、sequence匹配末槽，并server partial_eof=true/tail_request_parsed=false。unsent不得有R。BadRequest/pipeline仍invalid；完整slow必须flags3。原EOF 400响应业务不改，不能把其排空当完整请求D。

sample.json schema=baseline-v1-map-sample，mode为O/B；smoke为B，正式boundary-01/02/03分别为O/B/O，绑定role/run_id/真实package lock/buildreceipt/server/client SHA、实际进程clock、资源、client statistics与mandatory catalog。smoke固定warm1+measure3，正式warm5+measure20；全部128连接ready后共同窗。O无server记录，B必须128对128唯一tuple映射。统计bins及body/socket错误沿baseline-v1严格复核。

独立decode输出statistics包含N、duration_ns、qps、slow_count、main_raw_p99_us、corrected_p99_us、main_raw_max_us、cross_warmup_count、after_window_count、interval_us与all_corrected_bins_verified。完成慢请求rows按C1,C0,owner,life,seq排序，C0/R/D/C1及signed_intervals_ns=[R-C0,D-R,C1-D]保持整数；D>C1保留crossed，不归零。intersection_ns为两请求时间区间交集，longest_interval仅ordered时按最早tie。completion_bucket_5ms从T0按C1分桶；coverage保留全部连接序号。top10按raw降序/owner/life/seq。性能警戒先复核O1/O2波动再B/两O中位数；任何warning或无慢请求BLOCKED，不确认锁/网络/CPU因果。
