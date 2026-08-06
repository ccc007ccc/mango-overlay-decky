# ADR 0015：第一版同时支持游戏模式和桌面游戏

日期：2026-08-06
状态：已被 [ADR 0016](./0016-defer-desktop-renderer-from-first-release.md) 取代
取代：[ADR 0001](./0001-mvp-scope.md)

## 背景

游戏模式通过 MangoApp/Gamescope 合成，KDE 中启动的游戏则需要在进程内经过
MangoHud Vulkan 隐式层或 OpenGL shim。两边的系统接入不同，但已经验证可以共享
同一套提供者协议、原子场景、资源模型和 ImGui 场景渲染器。桌面双 ABI、Steam
Runtime 4 与 Proton 图形矩阵也已经达到可重复验收的程度，因此把桌面游戏继续留到
首版之后会让可用实现与发布范围脱节。

## 决策

第一版同时支持：

- SteamOS 游戏模式中的 MangoApp/Gamescope 渲染器。
- KDE 中启动的原生、Steam Runtime 和 Proton 游戏，覆盖 Vulkan、OpenGL、
  D3D11/DXVK 与 D3D12/VKD3D 最终进入的宿主渲染路径。
- `x86_64` 与 `i686` 桌面注入运行时。
- 一个稳定桌面启动接口：
  `~/.local/libexec/mango-overlay-decky/launcher.py desktop -- %command%`。

两个模式共享 `mango-overlayd`、版本化提供者协议、`SceneClient`、完整场景快照、
资源 ID 和 `ImGuiSceneRenderer`。桌面模式的差异只允许存在于 ABI 产物、Vulkan
manifest、OpenGL shim 和 Runtime/Proton 环境传播中，不复制场景或绘制业务实现。

KDE 桌面常驻覆盖窗口、输入处理、视频、SVG、自定义着色器和上传字体仍不属于第一版。

## 结果

- Decky 包必须携带游戏模式运行时、两种桌面 ABI、相对路径 Vulkan manifest 和所带
  私有库的许可证，并把它们作为同一个内容寻址修订原子安装、更新和回滚。
- 桌面游戏按游戏设置一次启动选项；不要求用户为 OpenGL、Vulkan 或 Proton 维护
  不同环境变量。
- 图形兼容性按模式、ABI 和 Runtime 矩阵验收。SteamOS、MangoHud、Steam Runtime、
  Proton 或 Vulkan loader 升级后需要重跑对应矩阵，不承诺未经测试的未来版本。
- 第一版发布验收顺序改为先在 KDE 用正式 Decky 包测试桌面游戏，再用同一包回归
  游戏模式；外接屏在没有硬件时明确延期，不伪造通过结论。
