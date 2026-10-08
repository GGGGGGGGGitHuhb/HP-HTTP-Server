-- Fixed closed-loop wrk 4.1.0 corrected statistics; no timed response callback.
local connections = tonumber(os.getenv('HP_MATRIX_CONNECTIONS'))
if os.getenv('HP_MATRIX_MODE') == 'short' then
  wrk.headers['Connection'] = 'close'
end

done = function(summary, latency, requests)
  local interval = summary.duration / (summary.requests / connections)
  io.write(string.format('MATRIX_SUMMARY {"schema":2,"duration_us":%.0f,"requests":%.0f,"bytes":%.0f,"errors":{"connect":%.0f,"read":%.0f,"write":%.0f,"status":%.0f,"timeout":%.0f},"latency_us":{"mean":%.6f,"p50":%.6f,"p95":%.6f,"p99":%.6f,"max":%.6f},"latency_distribution":"wrk_corrected","population_status":"not_collected","corrected_population":null,"nonzero_bins":null,"correction_interval_us":%.6f}\n',
    summary.duration, summary.requests, summary.bytes,
    summary.errors.connect, summary.errors.read, summary.errors.write,
    summary.errors.status, summary.errors.timeout,
    latency.mean, latency:percentile(50), latency:percentile(95), latency:percentile(99), latency.max,
    interval))
end
