# 一套场景接口连接多个 SteamOS 渲染器

游戏模式使用 MangoApp/Gamescope 外部覆盖层，桌面游戏实验使用 MangoHud 的 Vulkan/OpenGL 注入渲染；两者共用提供者协议和场景语义，避免调用方为了运行模式维护两套集成。[ADR 0016](./0016-defer-desktop-renderer-from-first-release.md) 将桌面渲染器移出第一版，但共享场景接口的决策不变。
