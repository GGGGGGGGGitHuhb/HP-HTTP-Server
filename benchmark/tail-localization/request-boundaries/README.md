# R018 request boundaries

> 2026-10-08归档：V0.5.1已搁置（未完成）。以下命令/批准说明仅为历史记录，当前候选非生产、非就绪入口；依赖本地冻结资料，未完成正式采集。不得按旧命令恢复，见benchmark/results/V0.5.1/SHELVED.md。

状态：静态候选，尚未构建或执行。唯一用途是在baseline-v1-map负载下关联客户端起止与服务端首次正读取/输出排空两点，不能据此直接归因网络、锁或调度。

包根与输出根显式提供；包文件以inputs-lock.json相对SHA闭合，准备来源仅追溯，不在执行期间读取历史cache。server_O是本角色已验证R015 E Release，构建槽从显式封存receipt核源码、Release参数、二进制和实际DSO后复制至新输出。server_B从固定E archive的新导出副本施加observer补丁，独立Debug测试构建和Release采样构建。不得修改生产树、旧common、旧二进制或历史原料。

构建命令：`/usr/bin/python3 -I PACKAGE/build_package.py --package-root PACKAGE --output-root BUILD_OUTPUT --server-o-receipt OWN_R015_RECEIPT --server-o-receipt-sha256 SHA`。必须经新治理具名build90授权，传精确role/run/packageSHA/output/governor身份及绝对work/cleanup期限；命令本身不是执行许可。真实样本使用BUILD_OUTPUT/relocated-package、server-O或build-B-release/hp_http_server，以及同一client/client-map。build-B只用于原9项CTest，observer_sanitized用于真实专项。

每role新建材料192MiB规划：共享+primary+relocated完整输入32MiB；固定E展开与wrk/dev/tool必要源码12MiB；B Debug全9项CTest构建80MiB；B Release24MiB；client/map与sanitizer12MiB；实际工具/格式/依赖日志及峰值32MiB。合计192MiB。64MiB observer是运行内存，不混入build磁盘估算；正式trace归Reviewer B256MiB限额。旧31.6秒双Release构建仅可行性参照，本次Debug全部tests+Release+client+sanitizer+format预计55–75秒，83秒work内仍有风险，Leader必须先核完整真实输入与target闭包，再唯一执行。

系统工具限定Linux amd64 GNU cc/c++/cmake/make/patch/dpkg-deb/ldd/binutils、clang-format-18和系统Python。OpenSSL/编译器头文件、glibc/libstdc++/sanitizer运行库为显式主机依赖；现有官方LuaJIT头/tool/DSO/modules在包内。禁止下载、安装、pkg-config隐式定位、profile和旧cache回退。LUA_PATH只有当前包，LUA_CPATH为空。

check入口为`/usr/bin/python3 -I PRIMARY/check_package.py --package-root PRIMARY --build-output BUILD_OUTPUT --output-root CHECK_OUTPUT --governance-test OWN_GOVERNANCE_TEST --governance-test-sha256 SHA --governance-module-sha256 SHA`。Reviewer另须提供`--verification-bundle OWN_BUNDLE --verification-bundle-sha256 SHA`；Builder禁止提供。所有工作同一次check60内完成：完整Python源码compile、真实治理测试、原9项CTest、真实ASan/UBSan观测器及实际导出的32字节wire核对、6组decoder接缝反例和真实IO专项。decoder合成wire测试不代替实际smoke/正式原料的完整统计验证。

build receipt明确区分E原文件`server_patch.before`、apply后`after`和格式化后`formatted`。`compiled_sources`封实际编译源码，`map_patch`封客户端接缝前后身份，系统工具/OpenSSL头/实际DSO和精确Debian跳过别名另有来源证据。check成功仅证明这些实际验证完成，阶段结论仍由独立Reviewer与Leader给出。

首次构建、纯验证、CTest、smoke、解析、计量或保护失败即停，不换role/slot重试。动态执行由Leader按Approved R018控制，当前README不宣称任何命令已通过。
