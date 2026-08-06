# Mango Overlay Decky

面向 SteamOS 的共享覆盖层绘制平台。第三方程序可以通过稳定接口在指定坐标绘制文字、图形、图片和动画，不需要各自实现 Gamescope 或 MangoHud 集成。

当前已完成共享数据面、基础绘制、图片/GIF、C/C++/Python/Rust 提供者 SDK，以及 Decky 安装、原子更新、回滚和延迟确认卸载。第一版只发布使用 MangoApp/Gamescope 的游戏模式渲染器，项目仍处于首版设备验收阶段。

## 范围

- 第一版：SteamOS 游戏模式的 MangoApp/Gamescope 渲染器。
- 后续实验：KDE 游戏的 MangoHud Vulkan/OpenGL 注入代码保留在仓库中，但不进入正式包，也不作为当前用户功能。
- 不采集 FPS，不内置业务监控，不提供 KDE 桌面常驻窗口。

## 使用模式

Decky 安装包后，游戏模式由用户级 `gamescope-mangoapp.service` drop-in 自动选择当前活动运行时，不需要为游戏设置 Steam 启动项。Steam 原生性能统计和提供者画布分别控制。

当前正式包不启用 KDE 游戏注入。旧测试版本留下的
`launcher.py desktop -- %command%` 启动项会原样透传游戏命令，不再设置 MangoHud
变量；仍建议从游戏属性中删除该启动项，恢复默认启动方式。

## 文档

- [架构](docs/architecture.md)
- [提供者协议](docs/protocol.md)
- [安装、更新与卸载](docs/lifecycle.md)
- [调试方法](docs/debugging.md)
- [实现计划](docs/implementation-plan.md)
- [SDK 使用与测试](docs/sdk.md)
- [领域语言](CONTEXT.md)
- [架构决策](docs/adr/)

## 开发环境

SteamOS 宿主保持不可变。编译、依赖安装和开发测试使用：

```bash
distrobox enter dev
```

正式测试包只在 `dev` 中构建：

```bash
distrobox enter dev -- ./packaging/build-steamrt4.sh
```

产物位于 `build/package/`。构建器使用固定 Steam Runtime 4 sysroot 保证安装到 SteamOS 的游戏模式二进制 ABI，不会把 Steam Runtime 或 KDE 注入层安装到用户系统。它在断网沙箱内生成带逐文件哈希的确定性 Decky zip，也不会替换宿主 `/usr/bin/mangoapp`。

## 许可证

项目以 MIT 协议开源，并保留 MangoHud 上游 Git 历史、版权和第三方运行时许可证。
