# 实现计划

## 原则

- 先建立可运行的 SteamOS MangoApp 基线，再加入功能。
- 每一阶段都有可执行验收，不以“代码写完”作为完成标准。
- 宿主 SteamOS 不安装编译依赖；所有构建与开发工具位于 `dev` distrobox。
- 第一版达到游戏模式验收后停止扩展功能；桌面渲染器实验不进入发布范围。

## 阶段 0：建立上游基线

1. 获取 SteamOS 当前 MangoHud 提交 `33c2c7ddbb72c15e19a42163d75424d5804f8ec8` 的完整上游历史。
2. 以该提交作为仓库历史基线，再加入当前文档和项目目录。
3. 在 `dev` 容器中复现原版 MangoApp 构建。
4. 使用 KDE 嵌套 Gamescope 运行原版构建，并记录 CPU、内存、帧时间和功能基线。
5. 固定许可证、上游来源和更新说明。

完成条件：本机构建产物在嵌套 Gamescope 中与 `/usr/bin/mangoapp` 行为一致。

当前结果：已完成。构建、启动桥、版本和参考开销记录在 [baseline.md](./baseline.md)。

## 阶段 1：协议与场景代理

1. 定义 FlatBuffers schema、错误码和能力位。
2. 先写协议兼容、场景事务、同 UID 身份检查、配额和损坏输入测试。
3. 实现 `mango-overlayd` 与 systemd socket activation。
4. 实现 C ABI、C++、Python、Rust 封装和确定性示例提供者。
5. 实现渲染器订阅接口、完整快照和序号缺口恢复。

完成条件：没有图形环境时，多提供者和多渲染器测试全部通过；代理重启、连接中断和恶意输入不会产生半场景。

当前结果：已完成。协议 `1.0`、原子场景、UID 检查、累计配额、提供者/渲染器重连以及四套 SDK 的真实 broker 测试均已通过。

## 阶段 2：游戏模式基础绘制

1. 在 MangoApp 基线中接入只读场景快照。
2. 实现参考画布、输出视口锚点、层级、分组和裁剪。
3. 实现文字与基础几何图形。
4. 保持 Steam preset、原生统计显示和 `no_display` 初始化行为兼容。
5. 完成 `tools/run-nested.sh`，自动运行场景代理、开发 MangoApp 和示例提供者。

完成条件：嵌套 Gamescope 中可稳定显示多个动态提供者；无提供者时行为和性能接近原版基线。

当前结果：绘制、布局和嵌套 Gamescope 路径已完成；无显示测试覆盖多提供者，嵌套测试覆盖动态示例提供者。真实游戏模式性能比较留到阶段 5。

## 阶段 3：图片与 GIF

1. 实现内联资源和只读文件描述符传输。
2. 在场景代理中异步验证、解码和缓存图片/GIF。
3. 在 MangoApp 渲染器中实现纹理生命周期和设备重建恢复。
4. 增加损坏资源、资源炸弹、超额资源和快速切换测试。

完成条件：图片和循环 GIF 不阻塞渲染线程；损坏或超额资源只影响对应提供者。

当前结果：已完成 PNG/JPEG/WebP/GIF 解码、内联与 sealed memfd 传输、原子资源镜像、OpenGL 纹理缓存和累计资源预算。嵌套 Gamescope 已目视确认静态图片与 GIF 换帧；IPC 和解码不进入渲染线程，纹理上传限制为每绘制帧一帧。

## 阶段 4：Decky 与生命周期

1. 实现稳定启动/生命周期助手和内容寻址的不可变运行时修订。
2. 实现首次安装、原子更新、上一版本回滚和系统 MangoApp 回退。
3. 实现待确认卸载与最终卸载。
4. 实现 Decky 状态、总开关、提供者授权、层级、诊断和测试画布。
5. 自动化验证 Loader 重启、后端重载、禁用、更新、卸载、休眠和关机矩阵。

完成条件：任何普通停止事件都不改变安装状态；Decky 更新失败保留旧版本；真正卸载恢复 `/usr/bin/mangoapp` 并清理所属文件。

当前结果：离线实现和自动化验收已完成。内容寻址运行时、逐文件清单、同版本重打包、事务回滚、更新中断恢复、待确认卸载、最终清理、系统 MangoApp 回退、控制器和 Decky 页面均有回归测试。卸载回调现在只做原子 pending 落盘和一秒上限的非阻塞 timer handoff；3 秒确认窗口会识别原目录、半解压目录和同标识替代版本，并清理精确的 Decky settings/data/logs 目录。真实 Loader 更新与卸载路径留到阶段 5。

## 阶段 5：真实游戏模式设备验收

1. 用 game-mode-only 正式 zip 验收安装事务、socket activation 与代理。
2. 切换到真实游戏模式，通过 SSH 观察并在用户操作后热重启 MangoApp 服务。
3. 完成 Steam 首页、游戏、QAM、Steam 性能层级和第三方画布独立显示矩阵。
4. 验证渲染器退出、代理崩溃和自动恢复都不改变安装状态。
5. 验证不同分辨率、安全区、挂起恢复和开机启动；外接屏在无硬件时明确延期。
6. 与基线比较空载和典型场景开销，确定发布配额。

完成条件：调试文档中的最低验收矩阵全部通过，并形成用户可执行的测试清单。

当前状态：进行中。新 game-mode-only 包已经通过真实 Decky 最终卸载：`_uninstall()` 在 0.1 秒内完成 pending handoff，3 秒后 finalizer 开始清理，约 5 秒内恢复系统 MangoApp 接入并删除运行时、状态、用户 units 以及精确的 Decky settings/data/logs 目录；没有 SIGKILL、失败 unit 或残留进程。旧正式包的激活、更新和最终卸载此前也已验证，但新实现的正常商店更新不误清理仍需单独复测，因为本次本地 ZIP 替换只走了 Loader reload/`_unload()` 路径。控制器、`XDG_RUNTIME_DIR`、同版本重打包、字体和完整输出窗口问题均已修复。桌面注入因要求逐游戏启动项且会覆盖 Steam 原生统计，已按 ADR 0016 移出首发。下一人工步骤是重新安装新包，在真实游戏模式完成显示与固定功耗性能回归，再执行真实更新测试。真实外接输出因当前没有硬件延期。

## 阶段 6：打包与首版发布

1. 构建可复现 Decky 插件包和运行时清单。
2. CI 执行格式、静态检查、单元测试、协议兼容测试和包结构检查。
3. 发布前在干净安装、旧版更新、卸载重装三条路径验收。
4. 文档只保留真实存在的命令、功能和限制。

完成条件：安装包可供普通 SteamOS 用户测试，不需要手工复制二进制或单独更新后台。

当前准备：确定性 Decky zip、内容寻址运行时、固定 Steam Runtime 4 sysroot 的游戏模式构建、包内 C/C++/Python/Rust SDK 验收和项目专用 CI 已纳入发布链。真实游戏模式验收完成前不创建首版 release。

## 阶段 7：多产品共享 Runtime Coordinator（首版后）

该阶段不阻塞第一版发布，但必须在任何第三方 Decky 产品内置 Mango Overlay Runtime 前完成：

1. 把候选清单、运行时声明、活动指针和失败修订定义为版本化的共享磁盘合同。
2. 将当前单插件生命周期深化为本项目拥有的 Runtime Coordinator，并提供最小的
   claim/register/remove/status interface。
3. 按 Mango 核心语义版本选择最大候选；claim 事务只比较有界元数据，只有实际要激活
   的候选运行一次验证，正常启动直接读取活动指针。
4. 实现同内容去重、同版本异内容冲突、验证缓存、失败修订隔离、known-good 回退和
   最后声明移除后的系统 MangoApp 恢复。
5. 建立新旧协调器磁盘 schema 与锁协议的合同测试，确保低版本插件后启动不会降级
   新核心，也不会在 schema 不可理解时继续写入。

完成条件：一百个并发/顺序测试声明只产生一个活动核心；稳定选择不调用 verifier，新增
最大版本只验证该候选一次；更新、失败回退、任意非最后声明卸载和最后声明卸载均通过
公共 interface 测试及真实 Decky 更新路径验证。

## 保留的桌面渲染器实验结果

以下内容记录已经完成的研发验证，便于未来评估自动接入方案；它不属于第一版正式包、
CI 发布门槛或当前兼容承诺。

当前开发进展：`x86_64` 与 `i686` Vulkan 隐式层和 OpenGL EGL/GLX shim 已接入共享
`SceneClient → SceneSnapshot → ImGuiSceneRenderer` 路径。两种架构的真实 KDE
图形检查均已覆盖第三方画布独立可见、文字/几何、PNG、GIF 换帧、连续帧稳定性，
以及 `800×500`、`1200×750`、`640×400`、`960×600` 间的窗口尺寸和 swapchain
重建恢复。`tools/build-desktop-i686.sh` 会构建三个注入库，并用 `file`、`readelf`
和 `ldd` 验证 ELF32 产物及依赖；最小 EGL、GLX 与 Vulkan 测试程序均已通过两种
架构的真实图形检查。固定 Steam Runtime 4 SDK 现在也能重复构建两种架构，且
真实 pressure-vessel 中的 GLX、EGL、单 swapchain Vulkan 与双 swapchain Vulkan
八格矩阵均已通过；前三类使用相同的 PNG、GIF、连续帧和 resize 断言。OpenGL
入口统一先加载 upstream `libMangoHud_shim.so`，
再由 `MANGOHUD_OPENGL_LIBS` 指向同一架构的 `libMangoHud_opengl.so`，因此 Wine
通过 `dlopen/dlsym` 取得函数时也能进入渲染器。Proton 的 x86_64/i686 OpenGL、
D3D11→DXVK→Vulkan 与 D3D12→VKD3D→Vulkan 六格矩阵均已通过真实底图、第三方
画布、PNG、GIF 换帧、连续帧和 resize 断言；D3D11 测试程序绘制旋转彩色三角形，
D3D12 测试程序绘制移动的高饱和彩色区域，避免覆盖层正常而测试底图全黑的假阳性。
2026-08-06 已在真实 SteamOS KDE 会话中让 SteamRT4 OpenGL 与 Proton
D3D12→VKD3D/Vulkan 共享同一提供者场景，并完成一次系统挂起与唤醒。两个窗口和
渲染进程均存活；恢复后连续 16 帧满足文字/几何、PNG、GIF 双色像素断言，GIF 至少
出现两个不同帧，OpenGL 与 D3D12 底图都保持非黑，日志中没有 device/surface lost
或异常退出。桌面游戏渲染器仍未完成的设备验收项是外接屏；当前没有外屏硬件，
因此明确保留为后续人工验收，不记为通过或失败。SteamOS、MangoHud、
Steam Runtime、Proton 或 Vulkan loader 升级后还需要重跑兼容回归。KDE 合成性能矩阵已经
为两种 ABI、六条路径记录无注入/空 broker/典型场景参考；真实游戏固定功耗下的发布
预算仍属于阶段 5 的设备验收。`tools/run-desktop-recovery-test.sh` 已把 SteamRT4
OpenGL 与 Proton D3D12/Vulkan 组合成共享场景的人工测试画布，并提供已验证的
`--stop` 清理入口；挂起恢复已经通过，该恢复画布只剩真实外接输出硬件项。
原生 Vulkan 双进程检查已经通过：两个进程共用一个 broker 和提供者，关闭其中
一个后另一个仍能连续显示。两种架构下的 SteamRT4 原生+Proton D3D12 混合进程，
以及使用独立 compatdata 的 Proton D3D11+Proton D3D12 双进程也已通过同一断言。
同一 Vulkan 进程双 swapchain 检查也已在 SteamRT4 的 `x86_64` 与 `i686` 中通过：
两个窗口同时显示，销毁 A 后 B 保持连续可见。原生、SteamRT4 和 Proton 单窗口
fixture 还会进入当前 KDE 输出的 EWMH 全屏并退出，两个状态都要求底图和提供者画布
连续恢复，因此全屏/无边框窗口路径已纳入所有渲染 API 的回归。

桌面渲染器只有在无需逐游戏启动项、不会影响 Steam 原生统计、且可用用户级集成覆盖
目标 Vulkan/OpenGL/Proton 路径时才重新评估发布。KDE 桌面常驻覆盖窗口、输入交互、
视频、SVG、自定义字体和自定义着色器仍不属于第一版。

## 停止条件

满足以下条件后第一版停止增加功能：

- 协议、生命周期和最低验收矩阵全部通过。
- 空载行为没有破坏原生 MangoApp。
- 典型文字、图形、图片和 GIF 场景达到基线测试确定的性能预算。
- 安装、更新、回滚和卸载均可由普通用户完成。
- 剩余问题均有明确记录，但不阻塞第一版核心用途。
