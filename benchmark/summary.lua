-- No response/request callbacks: only collect final wrk statistics.
done = function(summary, latency, requests)
  io.write(string.format('BENCH_SUMMARY {"schema":1,"duration_us":%.0f,"requests":%.0f,"bytes":%.0f,"errors":{"connect":%.0f,"read":%.0f,"write":%.0f,"status":%.0f,"timeout":%.0f},"latency_us":{"mean":%.6f,"p50":%.6f,"p95":%.6f,"p99":%.6f,"max":%.6f}}\n',
    summary.duration, summary.requests, summary.bytes,
    summary.errors.connect, summary.errors.read, summary.errors.write,
    summary.errors.status, summary.errors.timeout,
    latency.mean, latency:percentile(50), latency:percentile(95),
    latency:percentile(99), latency.max))
end
