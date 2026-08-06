# ADR 0016：桌面游戏渲染器移出第一版

日期：2026-08-06
状态：已接受
取代：[ADR 0015](./0015-first-release-supports-game-and-desktop-games.md)

## 背景

桌面注入实验已经覆盖 Vulkan、OpenGL、Steam Runtime、Proton 和两种 ABI，但当前
用户入口仍要求为每个 Steam 游戏配置：

```text
~/.local/libexec/mango-overlay-decky/launcher.py desktop -- %command%
```

真实游戏测试显示，不设置启动项时 Steam 自带性能统计正常，但提供者画布不会进入
游戏；使用项目启动项时，启动器默认写入 `MANGOHUD_CONFIG=no_display=1`，会让 Steam
原生统计消失。即使底层渲染矩阵可运行，这种接入方式也不符合产品要求：安装 Decky
后应自动接入，系统统计和提供者画布必须独立可见。

## 决策

第一版恢复为仅支持 SteamOS 游戏模式的 MangoApp/Gamescope 渲染器。

- 正式 Decky 包不携带桌面 `x86_64`/`i686` 注入库或 Vulkan layer manifest。
- 正式构建、包验收和 CI 不以桌面 ABI/Proton 矩阵为发布条件。
- 用户不需要为任何游戏设置项目启动项。
- 第一版运行时缺少桌面注入产物时，遗留的 `launcher.py desktop -- %command%`
  只原样执行游戏命令，不添加 MangoHud、Vulkan、preload 或场景 socket 环境变量。
- 桌面渲染器源码、开发脚本、测试程序和历史测量保留在仓库中，作为未发布实验代码，
  但不对兼容性或持续维护作首版承诺。

桌面渲染器只有同时满足以下条件才可重新进入发布范围：

1. 安装 Decky 后自动接入，不要求逐游戏启动项。
2. Steam 原生性能统计和提供者画布可分别开关，任何一方都不挤掉另一方。
3. 不修改 SteamOS 只读系统分区。
4. 同一用户级接入覆盖需要支持的 Vulkan、OpenGL 和 Proton 路径，并有真实设备回归。

## 结果

- 首版包显著减小，不再携带未启用的 32 位运行时和桌面注入私有库。
- 首版设备验收只关注真实游戏模式、生命周期、分辨率/输出、挂起恢复和性能预算。
- 保留的桌面代码继续验证共享协议和多渲染器架构可行性，但它不是已发布功能。
- Steam Runtime 4 仍可作为正式游戏模式二进制的固定构建 sysroot；这不表示包中包含
  Steam Runtime，也不表示桌面渲染器已启用。
