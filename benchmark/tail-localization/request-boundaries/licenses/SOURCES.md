# baseline-v1 输入与许可

- E-S3.tar：项目固定commit acda3f92d42a36d0b0554e185bc6f4155b4e5889；准备来源 `.cache/v0.5.1-s4/builder/E-S3/source.tar`，完整archive哈希在lock。项目源码未声明独立许可证；不推定可再分发授权，仅本项目批准的本地构建使用。
- wrk.orig.tar.gz：官方wrk 4.1.0，Ubuntu官方源包所含原始archive；准备来源leader/wrk-source，Apache-2.0及第三方许可保留在archive。wrk.debian.tar.xz：Ubuntu 4.1.0-4build2，Debian patch/版权清单见wrk-debian-copyright及archive。构建依序应用series，源码统计算法身份由patch_wrk.py固定。
- luajit-dev.deb / luajit-tool.deb：Ubuntu官方2.1.0+git20231223.c525bcb+dfsg-1ubuntu0.1 amd64包，准备来源leader/client-build-inputs；LuaJIT-dev-copyright与LuaJIT-runtime-copyright保留完整版权。
- runtime/：现有同版本官方运行库常规文件及jit Lua模块副本；无指向历史cache的链接。LuaJIT MIT及相关组件声明见LuaJIT-runtime-copyright。
- 自有baseline代码、配置、fixture：本项目R015新实现，不复制未声明的旧Python工具链。逐文件SHA和字节在inputs-lock.json，最终封包后生成且不把lock自身作为递归输入。

系统边界明确为Linux amd64 GNU工具链及glibc：/usr/bin/cc、c++、cmake、make、python3、patch、dpkg-deb、ldd；OpenSSL headers与libssl/libcrypto、libm/libdl/libpthread、libstdc++、libgcc及sanitizer runtime由系统提供。无安装/下载。build槽内记录工具版本/可执行SHA、ldd实际库位置，缺工具或OpenSSL头文件先失败。编译器系统头文件与链接器仍是显式host工具依赖，不宣称包包含操作系统。

新包命令不调用Git、pkg-config、用户profile或隐式Luajit下载。构建采用受控PATH=/usr/bin:/bin，LD_LIBRARY_PATH/LUA_PATH由当前绝对包根生成；源码由包内archive展开，所有输出限定指定output根。执行治理adapter/watchdog在包外，是显式S4角色授权依赖，不参与统计复算。

GNU compiler driver所调用的assembler/linker/archive工具显式限定为`/usr/bin/as`、`/usr/bin/ld`、`/usr/bin/ar`、`/usr/bin/ranlib`及编译器自身`/usr/lib/gcc/`内cc1/cc1plus/collect2。CMake显式指定`/usr/bin/make`，不自动选择旧工作区程序。Lua构建环境`LUA_CPATH`为空，禁止系统C模块搜索；内置ffi/bit仅由已锁定LuaJIT执行文件提供。`linux-vdso.so.1`属于主机内核虚拟对象，在receipt单独列出，不编造文件SHA。

Debian解包仅取经过安全核验的实际必要常规文件：dev包的`usr/include/luajit-2.1/`及tool包的`usr/bin/luajit`。包内开发链接别名、manpage和重复doc不生成在角色输出内；这些原料仍完整保留于SHA锁定deb和版权副本。源码archive不支持实际symlink提取，发现则明确失败；运行包和构建目录均不生成跨包/未批准链接。

准备来源补充：runtime两个同SHA库文件来自`.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu/libluajit-5.1.so.2.1.1703358377`；`runtime/lua/jit/*`逐项来自`.cache/v0.5-s4/tools/root/usr/share/luajit-2.1/jit/*`。runtime版权来自该tools/root的`usr/share/doc/libluajit-5.1-2/copyright`；dev版权来自`.cache/v0.5.1-s4/leader/client-build-inputs/root/usr/share/doc/libluajit-5.1-dev/copyright`；wrk版权来自封存wrk-source树的`debian/copyright`。这些仅是准备时来源记录，封包执行不访问这些历史目录。

R018派生说明：baseline/client逐字节从已验收R015 measurement-baseline/client复制；旧包不改。archive/runtime/版权同源常规文件静态准备，真实执行仅读新包。archive_inputs.py/prepare_package.py为最小自有安全解包/闭包复用副本，已列新lock，无指向旧工具链的导入。Observer/map/runner由R018新增实现，GNU系统工具和clang-format-18单独声明。O的唯一包外准备输入是本角色R015已成功build receipt及它明确绑定的E二进制/DSO，具名build槽复核并复制至本轮输出；不会隐式搜索其他role/cache。
