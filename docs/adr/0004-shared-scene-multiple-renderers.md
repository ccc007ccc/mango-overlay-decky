# 一套场景接口连接多个 SteamOS 渲染器

游戏模式使用 MangoApp/Gamescope 外部覆盖层，桌面游戏使用 MangoHud 的 Vulkan/OpenGL 注入渲染；两者共用提供者协议和场景语义，避免调用方为了运行模式维护两套集成。原计划分阶段发布，现由 [ADR 0015](./0015-first-release-supports-game-and-desktop-games.md) 决定在第一版同时交付两种渲染器；共享场景接口的决策不变。
