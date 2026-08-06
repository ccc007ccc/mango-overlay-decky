# 调试方法

## 原则

- 编译、安装依赖和运行测试工具都在 `dev` distrobox 容器中完成。
- 宿主 SteamOS 只运行构建产物和系统自带工具，不安装开发依赖。
- 日常修改不需要重启 SteamOS。
- 不覆盖 `/usr/bin/mangoapp`，任何测试都必须有明确的原版恢复路径。
- 无显示测试覆盖共享核心；图形测试分别覆盖游戏模式和桌面游戏的真实入口。

## 四层测试

### 1. 无显示测试

在 `dev` 容器中测试：

- IPC 编解码和版本协商。
- 输入大小、连接数和权限限制。
- 多提供者合并与连接断开清理。
- 场景事务提交成功、提交失败和连接中断时的原子性。
- 数据快照的并发读取。
- 布局计算和配置迁移。
- 启动保护器的版本判断与回退状态机。
- 场景代理 socket activation、空闲停止和渲染器重连。
- 应用身份、多实例以及审批开关的两种策略。
- 其他 UID 拒绝、角色越权消息和自声明身份冲突。

这层应进入每次提交的 CI，不依赖 Gamescope、Steam 或 GPU。

### 2. KDE 中的嵌套 Gamescope

Gamescope 的 `--mangoapp` 会通过 `PATH` 查找名为 `mangoapp` 的程序。项目脚本使用宿主 SteamOS Gamescope，并让开发 MangoApp 与测试程序运行在 `dev` 容器中，既不影响系统服务，也避免容器用户命名空间无法读取宿主测试进程的 `/proc` 信息：

```bash
tools/run-nested-baseline.sh
```

退出嵌套窗口后，测试进程和覆盖层一起结束。基线构建命令与参考结果见 [baseline.md](./baseline.md)。

这层可以验证：

- 外部覆盖层窗口是否被 Gamescope 正确识别。
- Alpha、缩放、分辨率和布局。
- Gamescope 帧消息兼容性。
- 多提供者数据显示、更新和消失。
- 显示开关、配置重载和异常输入。

这层不能替代真实游戏模式中的 Steam preset、QAM、HDR、VRR、挂起恢复和服务生命周期测试。

项目应提供 `tools/run-nested.sh`，自动选择开发二进制、启动示例提供者、收集日志并在退出时清理临时文件，避免开发者手工拼接环境变量。

### 3. 正式包从桌面到游戏模式测试

正式包的生命周期和代理不依赖游戏模式。先在桌面模式执行只读预检：

```bash
python3 tools/activate-package-desktop.py \
  build/package/mango-overlay-decky-0.1.0.zip
```

在明确同意修改当前用户安装状态后，增加 `--apply`。工具只允许在
`gamescope-mangoapp.service` 为 `inactive` 时激活正式 zip，随后通过真实
systemd 用户管理器验证 socket activation 和代理状态，并确认没有启动 MangoApp：

```bash
python3 tools/activate-package-desktop.py \
  build/package/mango-overlay-decky-0.1.0.zip --apply
```

这一步覆盖 Decky 后端最关键的文件验证、事务恢复、用户服务安装和代理启动，失败时可直接在桌面修复，不需要重启或切换会话。

正式 Decky 包激活后，在 KDE Steam 游戏中设置一次启动选项：

```text
~/.local/libexec/mango-overlay-decky/launcher.py desktop -- %command%
```

打开插件测试画布后，先验证一个 OpenGL 游戏和一个 Vulkan/DXVK/VKD3D 游戏。两者
应显示同一套文字、图形、PNG 和 GIF；改变窗口尺寸或全屏后画布应恢复。桌面启动器
默认设置 `no_display=1`，所以只显示提供者画布；用户显式设置的 `MANGOHUD_CONFIG`
会保留。桌面测试失败时，退出游戏并临时把启动选项恢复为 `%command%` 即可停止注入，
不会改变插件安装状态。

桌面正式入口通过后，才从桌面模式切换一次游戏模式做最终 Steam 集成验收；切换会话
不需要重启设备。

进入游戏模式后，通过另一台设备 SSH 连接 SteamOS，完成后续迭代。

Decky 安装测试包后，生命周期助手会安装指向版本化运行时的 drop-in。更新测试包后只需在必要时重启 MangoApp 用户服务：

```bash
systemctl --user daemon-reload
systemctl --user restart gamescope-mangoapp.service
journalctl --user -u gamescope-mangoapp.service -f
```

这会短暂重建性能覆盖层，不会重启 Steam、Gamescope 或当前游戏。插件在激活版本前已经运行所有二进制自检；扩展 MangoApp 意外退出时，启动器会在同一服务内执行系统 MangoApp。

紧急恢复命令为：

```bash
python3 ~/.local/libexec/mango-overlay-decky/lifecycle.py \
  restore-system --home "$HOME"
```

它删除本项目的用户级 service drop-in、重载 systemd，并仅在服务原本活动时重启系统 MangoApp。运行时、设置和安装版本保持不变，便于保留现场诊断。

真实游戏模式需要验证：

- Steam UI、游戏焦点和无游戏三种场景。
- Steam 性能层 0–4 级和关闭状态。
- 第三方显示策略。
- QAM 打开、关闭和切换游戏。
- 游戏运行中热重启覆盖层。
- 扩展进程主动崩溃后的自动回退。
- Decky 重载、更新和卸载。

### 4. 必须改变会话状态的测试

只有以下场景需要额外操作：

- 开机自启动：重启一次。
- 挂起与恢复：执行一次睡眠/唤醒。
- 桌面模式和游戏模式切换：切换会话，不重启系统。
- SteamOS 或 MangoHud 升级兼容性：升级后重新执行兼容矩阵。

## 桌面游戏渲染器开发矩阵

桌面模式支持指“从 KDE 中启动游戏后，在游戏画面内绘制”，不是创建一个覆盖整个 KDE 桌面的常驻窗口。该后端使用 MangoHud 的 Vulkan 隐式层和 OpenGL shim，矩阵覆盖 Vulkan、OpenGL、`x86_64`、`i686`、Proton、全屏、无边框和多游戏进程场景，并验证 Steam Runtime 内仍能访问同一场景代理 IPC。

当前开发构建可用同一个测试场景分别启动三个 `x86_64` 渲染器：

```bash
tools/run-overlay-test.sh game provider-only
tools/run-overlay-test.sh vulkan provider-only
tools/run-overlay-test.sh opengl provider-only
```

把 `provider-only` 改为 `combined` 可同时显示原生 MangoHud 统计。桌面 Vulkan
默认运行 vkcube，OpenGL 默认运行 glxgears；二者都只使用临时 broker、临时
socket 和开发构建，不安装 Decky 插件，也不修改系统 MangoApp。

颜色、动画、连续帧稳定性、松手后的窗口尺寸恢复以及 KDE 全屏进入/退出使用以下
无人值守检查：

```bash
tools/check-desktop-vulkan-visible.sh
tools/check-desktop-opengl-visible.sh
tools/check-desktop-egl-visible.sh
```

`i686` Vulkan 和 OpenGL GLX 使用同一套像素与 resize 断言：

```bash
tools/build-desktop-i686.sh
tools/check-desktop-i686-vulkan-visible.sh
tools/check-desktop-i686-opengl-visible.sh
tools/check-desktop-i686-egl-visible.sh
```

Steam 从 KDE 启动游戏时还会经过 pressure-vessel。正式兼容基线使用固定的
Steam Runtime 4 SDK 同时生成 `x86_64` 与 `i686` 产物，并在真实 Runtime 中
串行执行八格图形矩阵：每种架构分别覆盖 GLX、EGL、单 swapchain Vulkan 和同一
进程双 swapchain Vulkan：

```bash
distrobox enter dev -- tools/build-desktop-steamrt4.sh all
tools/check-desktop-steamrt4-visible.sh all
```

Runtime 检查会显式共享每次测试的临时 broker 目录；只传
`MANGO_OVERLAY_SOCKET` 环境变量并不足以让 pressure-vessel 看见该 Unix socket。
构建脚本也会随对应架构产物放置 Runtime 平台缺少的私有图像依赖，并通过
`--ld-preload` 逐项引入，不修改 Steam 安装或宿主系统库。

构建脚本只在 `dev` 容器中使用 `gcc -m32`/`g++ -m32` 和 multilib 依赖，生成
`MangoHud.x86.json`、`libMangoHud.so`、`libMangoHud_opengl.so` 与
`libMangoHud_shim.so`，并拒绝非 ELF32 产物、缺失依赖或误链的非 32 位动态库。
Vulkan 图形检查要求 `dev` 中已有 `lib32-vulkan-icd-loader` 和对应的 32 位 Vulkan
驱动；测试窗口本身是最小 Xlib 交换链程序，不依赖外部 32 位 vkcube 包。
EGL 检查使用最小 X11 window surface 与桌面 OpenGL context，直接经过
`eglSwapBuffers` 注入路径；GLX 检查则经过 `glXSwapBuffers`。

检查会验证静态 PNG 每一帧都存在、GIF 包含预期颜色且实际换帧，并依次切换
`800×500`、`1200×750`、`640×400` 和 `960×600`，随后进入当前 KDE 输出的 EWMH
全屏并退出。每一步都要求画布连续恢复；Proton fixture 还要求窗口中央底图保持非黑。
KDE 中原生 vkcube 在按住边框实时拖动时也可能间歇闪黑；覆盖层验收关注应用结束
交互缩放后完成 swapchain 重建并连续恢复画布，不能把原生测试程序自身的拖动现象
归因于注入层。

Proton 通过同一桌面渲染器检查，六格矩阵分别覆盖两种架构的 Windows OpenGL、
D3D11→DXVK→Vulkan 和 D3D12→VKD3D→Vulkan：

```bash
distrobox enter dev -- tools/build-proton-test-windows.sh all
tools/check-desktop-proton-visible.sh all
```

OpenGL 检查必须预加载 `libMangoHud_shim.so`，由 `MANGOHUD_OPENGL_LIBS` 指向
`libMangoHud_opengl.so`；只预加载后者在 Wine 中可能绕过交换缓冲拦截。Proton
检查除了覆盖层颜色、PNG、GIF 和 resize，还会检查窗口中央底图至少有非黑像素，
避免“覆盖层正常但游戏画面全黑”的假通过。D3D11 最小程序绘制与 OpenGL 对照窗口
相同的旋转彩色三角形；D3D12 最小程序通过真实 command queue、flip swapchain 和
`ClearRenderTargetView` 绘制移动的高饱和彩色区域。

多 renderer 进程共享同一场景代理的原生 Vulkan 检查：

```bash
tools/check-desktop-multiprocess-visible.sh
tools/check-desktop-multiprocess-visible.sh steamrt4-proton x86_64
tools/check-desktop-multiprocess-visible.sh steamrt4-proton i686
tools/check-desktop-multiprocess-visible.sh proton-proton x86_64
tools/check-desktop-multiprocess-visible.sh proton-proton i686
```

该命令同时启动两个独立 Vulkan 进程，验证两者都能收到同一已提交场景，再关闭第一个
进程并确认第二个仍持续显示。默认模式使用两个原生进程；`steamrt4-proton` 同时运行
一个 SteamRT4 原生 Vulkan 进程和一个 Proton D3D12 进程；`proton-proton` 使用独立
compatdata 同时运行 Proton D3D11 与 D3D12。Proton 总回归命令已包含后两种模式。
同一进程内多个 swapchain 使用另一条检查：

```bash
tools/check-desktop-multiswapchain-visible.sh x86_64
tools/check-desktop-multiswapchain-visible.sh i686
```

双 swapchain fixture 只启动一个进程、一个 Vulkan instance/device/queue，并创建两个
X11 window、surface 和 swapchain；检查先确认两边同时显示，再让程序只销毁 A，随后
连续确认 B 的提供者画布仍然可见。`check-desktop-steamrt4-visible.sh all` 已包含这两格。

兼容性是有边界的：游戏模式固定 MangoHud/MangoApp 与 Gamescope 基线，并在运行时
自检失败时回退系统 MangoApp；桌面注入按原生 GLX/EGL/Vulkan、SteamRT4 和 Proton
矩阵分别验证。升级 SteamOS、MangoHud、Steam Runtime、Proton 或 Vulkan loader 后，
应重新运行对应构建和矩阵，不能仅凭旧版本通过推断新版本兼容。

桌面渲染器的合成性能参考使用：

```bash
tools/benchmark-desktop-renderers.sh all all
```

它对每条路径比较无注入、空 broker 和典型提供者场景，记录被测渲染进程以及项目
进程的 CPU task-clock、cycles、instructions 和最大 RSS。结果写入
`build/desktop-performance/latest.csv`；采样定义和当前参考值见
[baseline.md](./baseline.md)。该命令不替代真实游戏模式固定功耗下的发布性能测试。

挂起恢复和真实输出切换需要人工操作。统一测试画布可用：

```bash
tools/run-desktop-recovery-test.sh
```

它让 SteamRT4 OpenGL 与 Proton D3D12/Vulkan 两个带标题窗口共享同一 broker 和
提供者场景，不安装插件、不修改服务。测试结束或画面异常时的恢复命令为：

```bash
tools/run-desktop-recovery-test.sh --stop
```

该停止命令会验证控制器 PID，再关闭两个窗口、Runtime、provider 和 broker；日志路径
由启动命令打印。

## 日志

所有组件使用同一组字段：

```text
timestamp
level
component
event
session_id
provider_id（适用时）
protocol_version（适用时）
message
```

启动时必须记录：

- 项目版本和提交。
- Gamescope 与 MangoHud 版本。
- 检测到的协议版本或基线。
- 选择扩展版还是系统版。
- 回退原因。

高频数据更新不能逐条写普通日志；只记录限流后的统计和异常。

## 自检工具

- `mangoapp --mango-overlay-self-test`：不创建窗口，验证扩展版本和协议。
- `mango-overlayd --mango-overlay-self-test`：验证场景代理协议。
- `mango-overlayctl --mango-overlay-self-test`：验证控制器协议。
- `mango-overlay-test-provider --mango-overlay-self-test`：验证示例提供者。
- `mango-overlayctl status`：读取代理、策略和提供者状态。
- 清空 `XDG_RUNTIME_DIR` 与 `DBUS_SESSION_BUS_ADDRESS` 后运行插件后端的控制器回归测试：验证 Decky 沙箱环境仍能连接当前用户代理。
- `tools/run-nested.sh`：运行 KDE 嵌套 Gamescope 端到端绘制测试。
- `tools/run-overlay-test.sh`：以同一场景运行 Game/MangoApp、KDE Vulkan 或 KDE OpenGL 对照窗口。
- `tools/build-desktop-i686.sh`：构建并验证 i686 Vulkan、OpenGL 与 shim 注入库。
- `tools/build-desktop-steamrt4.sh`：在固定 SteamRT4 SDK 中构建并验证两种架构的桌面注入库和测试程序。
- `tools/check-desktop-steamrt4-visible.sh`：串行验证 SteamRT4 中两种架构的 GLX、EGL、
  单 swapchain Vulkan 与双 swapchain Vulkan 图形矩阵。
- `tools/check-desktop-vulkan-visible.sh`：验证 Vulkan 图片、GIF、连续帧稳定性、窗口尺寸
  恢复和 KDE 全屏进入/退出。
- `tools/check-desktop-opengl-visible.sh`：验证 OpenGL 图片、GIF、连续帧稳定性、窗口尺寸
  恢复和 KDE 全屏进入/退出。
- `tools/check-desktop-egl-visible.sh`：用原生 x86_64 EGL/OpenGL 程序验证 `eglSwapBuffers` 注入。
- `tools/check-desktop-i686-vulkan-visible.sh`：用原生 32 位 Vulkan/Xlib 程序验证 i686 隐式层。
- `tools/check-desktop-i686-opengl-visible.sh`：用原生 32 位 GLX 程序验证 i686 OpenGL 注入。
- `tools/check-desktop-i686-egl-visible.sh`：用原生 32 位 EGL/OpenGL 程序验证 i686 EGL 注入。
- `tools/check-desktop-multiprocess-visible.sh`：验证两个原生 Vulkan renderer 进程共享
  broker 场景，也可验证 SteamRT4+Proton 和两个独立 Proton 前缀；其中一个退出后
  另一个 renderer 必须保持可见。
- `tools/check-desktop-multiswapchain-visible.sh`：在 SteamRT4 中验证同一 Vulkan 进程
  的两个 swapchain 同时显示，并在销毁其中一个后保持另一个可见。
- `tools/benchmark-desktop-renderers.sh`：比较两种 ABI、六条桌面渲染路径在无注入、
  空 broker 和典型场景下的 CPU 与 RSS，生成可重复的回归参考。
- `tools/run-desktop-recovery-test.sh`：为真实挂起恢复和输出切换同时打开 OpenGL 与
  Proton D3D12/Vulkan 窗口；`--stop` 是已验证的清理命令。
- `packaging/build-steamrt4.sh`：在固定 Steam Runtime 4 中构建游戏模式、桌面双 ABI 并打包。
- `packaging/verify-packaged-sdk.py`：临时安装最终 zip，验证双 ABI、桌面启动器、生命周期和四套 SDK。

## 最低验收矩阵

| 场景 | 预期结果 |
| --- | --- |
| 无提供者 | 原生 MangoApp 行为不变，无空白第三方区域 |
| 一个提供者更新 | 数值平滑更新，渲染线程不阻塞 |
| 多元素事务提交 | 所有变化在同一快照出现，不显示半更新画面 |
| 游戏模式渲染器重启 | 场景代理保持场景，重连后获得完整快照 |
| 桌面 OpenGL/Vulkan 游戏 | 使用同一启动选项显示同一提供者场景 |
| 32/64 位桌面游戏 | `$LIB` 选择正确 ABI，不加载另一架构的库 |
| 桌面游戏退出 | 只结束该渲染会话，安装状态和代理保持不变 |
| 多个提供者同名字段 | 按提供者隔离，不互相覆盖 |
| 提供者崩溃 | 连接关闭后自动移除数据 |
| 恶意超长或高频输入 | 被拒绝或限流，覆盖层继续运行 |
| 提供任意宿主文件路径 | 明确拒绝，不读取文件 |
| 损坏图片或 GIF | 只拒绝对应资源，其他画布继续运行 |
| Steam 性能层关闭 | 系统统计隐藏，获准的第三方画布仍可显示 |
| 新提供者审批关闭 | 新应用立即显示，仍可由用户隐藏或撤销 |
| 新提供者审批开启 | 未知应用保持待授权，不向渲染器发布画布 |
| 扩展版启动失败 | 自动运行 `/usr/bin/mangoapp` |
| 扩展版运行中崩溃 | 自动回退且不重启 Steam 或游戏 |
| Decky 重载 | 覆盖层继续运行 |
| Decky 卸载 | drop-in 和项目状态被清理，系统 MangoApp 恢复 |
| Decky 更新 | 新版本验证后原子切换；失败恢复旧版本 |
| 更新过程中终止 Loader | 下次启动恢复可验证版本，不进入最终卸载 |
| 休眠/恢复 | 安装状态不变，不出现待确认卸载 |
| 关机/开机 | 安装状态不变，正常启动活动版本 |
| 普通 `SIGTERM` | 只结束运行会话，不计为崩溃或卸载 |
