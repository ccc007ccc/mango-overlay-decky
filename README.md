# Mango Overlay Decky

面向 SteamOS 的共享覆盖层绘制平台。第三方程序可以通过稳定接口在指定坐标绘制文字、图形、图片和动画，不需要各自实现 Gamescope 或 MangoHud 集成。

当前处于架构阶段，尚无可安装版本。

## 范围

- 第一阶段：SteamOS 游戏模式的 MangoApp/Gamescope 渲染器。
- 后续阶段：SteamOS 桌面模式游戏的 MangoHud Vulkan/OpenGL 渲染器。
- 不采集 FPS，不内置业务监控，不提供 KDE 桌面常驻窗口。

## 文档

- [架构](docs/architecture.md)
- [提供者协议](docs/protocol.md)
- [安装、更新与卸载](docs/lifecycle.md)
- [调试方法](docs/debugging.md)
- [实现计划](docs/implementation-plan.md)
- [领域语言](CONTEXT.md)
- [架构决策](docs/adr/)

## 开发环境

SteamOS 宿主保持不可变。编译、依赖安装和开发测试使用：

```bash
distrobox enter dev
```

