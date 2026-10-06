# V0.5.1 S3 Builder 015 — 完整运输证据与下一方向

2026-10-06。Approved R013完成一次校正微型及原两个短HTTP样本，没有产品修改/发布；正式S3仍返工中/FAIL。

4096KiB/CPU、无追赶分批微型各90000对sendfile/read，独立CRC/配对/返回1024核验通过、loss0/entries0/完整排空。实际生产速率每类约29452/s，不能当P3等效速率。

HTTP各3秒预热/8秒测量：A QPS48119.155/P99 248.374ms，B QPS46648.273/P99 68.457ms，errors0。QPS差3.06%、P99差72.44%，扰动flag=true；单pair不能因果认定工具影响，也不是正式性能验收。B完整77192对server sendfile、178330对client read，独立CRC/数量一致、loss0。

最长完整sendfile15.322ms/read8.987ms未观察到offCPU；更长停顿在运输调用之间。server gap137.954ms几乎全为睡眠等待（blocked137.730ms），另123.350ms gap仅offCPU0.057ms、剩余123.293ms；client最长gap27.676ms主要睡眠等待。这些gap包含未追踪syscall/用户代码，不对应单个请求；运行残差不等于纯CPU，不能排除宿主暂停。

异步probe见futex只是采样时刻证据，不能认定logger mutex。包围CPU计数窗口显著超过gap，不能精确归因gap CPU。下一候选是只为所选serverworker补futex/epoll/read/write及调用地址，需要新设计审查授权；本轮全部动态停止。

完整边界与证据：`docs/builder/reports/V0.5.1/S3-report-015.md`。Reviewer014已独立复核：R012/R013合计46.228318秒/92660808B snapshot；R008累计158.199564秒/418306178B，预算内；172保护文件/seals及恢复一致，unknown/running为0。R014仅Draft，未执行；本报告定版。
