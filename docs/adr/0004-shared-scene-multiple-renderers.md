# 一套场景接口连接多个 SteamOS 渲染器

项目最终支持 SteamOS 游戏模式以及桌面模式中启动的游戏，但第一阶段只实现游戏模式渲染器。游戏模式使用 MangoApp/Gamescope 外部覆盖层，桌面游戏后续使用 MangoHud 的 Vulkan/OpenGL 注入渲染；两者共用提供者协议和场景语义，避免调用方为了运行模式维护两套集成。
