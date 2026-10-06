# bbport 代码质量评审记录
日期: 2026-10-06。配套文档: documents/design-review.zh.md(架构级设计)。
方法: 三路并行审计(src C 运行时、gpu/shim C++ 胶水层、scripts 启动链)+ 人工定点核验 + 编译级验证。审计代理返回的定位线索全部经源码复核后才定性,未经证实的一律列入观察项而非改动。

## 一、既有保障(已核实)

- C 层: build.sh 以 -Wall -Wextra -Werror 编译 probe 与运行时,警告即错误,基础卫生由编译器把守。
- GPU 层: CMake RelWithDebInfo + ThinLTO,shadPS4 子集经上游 CI 标准与本地 Werror 策略。
- 运行期: VEH 三级恢复链(GPU fault → BB_RECOVER → fatal)与崩溃报告(栈双走查)使故障可定位,本轮全部崩溃级修复即受益于此。
- vendored shadPS4(gpu/shadps4)为第三方 vendor 代码,仅做已上溯的针对性修改,不做泛化清理,避免破坏上游可对照性。

## 二、本轮修复清单(累计)

崩溃级(均经指令级定位,见 design-review)
1. Scheduler record_chunk 跨线程 move 竞态 → chunk_mutex 全访问点入锁(vk_scheduler.h/.cpp)。
2. 纹理 GC aggressive 逐出窗口倒置 160→16(texture_cache.cpp)。
3. run.bat 旧环境进程 MSYS2 变量缺失无声失败 → 注册表回退读取。
4. Scheduler direct_mode 跨线程普通 bool → std::atomic<bool> relaxed(vk_scheduler.h:936, .cpp:239):KickRecording 任意线程写、录制线程读写,原为数据竞争(UB);阶段协议本身不变。并发终审遗留的唯一实证竞态,已修复。

接口与契约(零行为变更)
5. win32_memory.c: 线程契约注释固化——所有入口须持 runtime 内存写锁;release 重映射期间该区间 NOACCESS,并发访问会产生伪 host fault,由调用方串行化。
6. host_sync.h: HostRecursiveMutex 快速路径健全性论证注释——活线程 id 唯一,owner==self 只能由持有者自身程序序写出,非 TOCTOU。

文档与实现不一致(本轮审计的实际发现,均已修正)
6. design-review 初稿误判: mods overlay"启动器重跑后旧目录残留为垃圾"——实证 run_windows.py finally 有 remove_overlay,仅整进程被杀时残留;已改述。
7. design-review 初稿误判: HostRecursiveMutex"线程 ID 复用窗口理论存在"——审读后确认健全;已改述并反向固化为契约注释。

## 三、按类目结论

死代码: 未发现可删除项。BB_WRITE_LOG、BB_PGO_GENERATE 的条件编译体是设计的一部分(审计/性能工具链);BbCopy::ParallelFor 的串行回退分支是禁用/嵌套调用时的必要降级;extern "C" 导出(bbgpu_dump_guest_writes 等)由 C 运行时崩溃链引用。判据: 全仓库 Grep + 编译警告为零 + 条件编译语境确认。
冗余: 无实证可合并项。watchlist: bbport_copy 的 epoch/generation 双计数与 write_log 的多模式输出,若后续功能演化需重审。
语义/实现不一致: 即上述 6、7 两处,均在文档侧而非代码侧——代码注释与行为经核验一致。
接口不一致: src 头文件契约注释欠缺是真实缺口,win32_memory 与 host_sync 已补;runtime.h 的跨平台恢复上下文(RuntimeRecoverBuf/runtime_setjmp)与 probe.c 使用一致。
交互不协调: run.bat 无声失败已修;patches.py 的 patch 行校验失败采用"跳过并继续"策略,建议未来汇总告警(观察项,非缺陷——静默跳过会掩盖补丁失效,但中断启动更差,当前取舍合理)。
边界: C 层真实可达路径经人工复核未见越界/截断新问题;scripts 层 subprocess 返回码均经 run() 封装检查;mod 解包路径穿越检查列入路线图 P0(当前 mods.py 只做目录级硬链接,不接触压缩包,风险在引入下载器时才成立)。
错误代码: 本会话三处崩溃级即本轮全部实证;审计未再发现新的确定性错误。

## 四、性能状态

已落: chunk 互斥(正确性优先,每命令一次无竞争锁约 20ns,热路径可承受)、GC 窗口修复(解除逐出空转)。
待做(按收益,详见 design-review 第六节): Fossilize 式管线序列化预编译(治区域切换卡顿,收益最大)、纹理 GC clock-sweep 变体、win32 views 有序结构化、DrawPipe 超时与看门狗。
刻意不做: record_chunk 无锁槽位化(锁未成为热点,复杂度不换收益);mods.py 固定名重建(行为变更,无测试覆盖,列入 P0 待实施项而非本轮顺手改)。

后记(2026-10-06): mods.py 指纹命名缓存已实施并带测试覆盖(test_overlay_cache_hit_and_invalidation 等),原 P0 项关闭;overlay 生命周期随之变更,run_windows.py finally 与 run.sh trap 的会话末清理已删除,由 24 小时清扫接管。

## 五、安全

- 现状: 启动链无网络输入路径到代码执行;cheat/patch 尚未引入,引入时按 design-review 第四节硬约束(数据白名单、guest 区间校验、guest 区间外一律拒绝)。
- 供应链: 未来 mod 下载功能必须做解包条目路径校验与符号链接逃逸防护;当前项目不自带下载器,BB_Launcher 的 Downloader 是外部生态。
- 崩溃面: 所有 host fault 已有 VEH 恢复与报告兜底;修复策略为先定位后修复,禁止基于猜测的防御式补丁。

## 六、验证

- 全量增量构建通过(含契约注释改动),产物 out/bb-probe.exe;日志 out/build-quality.log。
- 运行验证: 修复后游戏在原崩溃工况(纹理用量爬升期)全程零 Host fault,正常退出。
- 审计遗留观察项汇总见第三节 watchlist,均不构成本轮改动依据。
