# 调试方法

## 原则

- 编译、安装依赖和运行测试工具都在 `dev` distrobox 容器中完成。
- 宿主 SteamOS 只运行构建产物和系统自带工具，不安装开发依赖。
- 日常修改不需要重启 SteamOS。
- 不覆盖 `/usr/bin/mangoapp`，任何测试都必须有明确的原版恢复路径。
- 当前测试链先覆盖游戏模式渲染器；桌面游戏渲染器实现时增加独立的游戏内测试链。

## 四层测试

### 1. 无显示测试

在 `dev` 容器中测试：

- IPC 编解码和版本协商。
- 输入大小、更新频率和权限限制。
- 多提供者合并、断开和心跳超时。
- 场景事务提交成功、提交失败和连接中断时的原子性。
- 数据快照的并发读取。
- 布局计算和配置迁移。
- 启动保护器的版本判断与回退状态机。
- 场景代理 socket activation、空闲停止和渲染器重连。
- 应用身份、多实例以及审批开关的两种策略。
- 其他 UID 拒绝、角色越权消息和自声明身份冲突。

这层应进入每次提交的 CI，不依赖 Gamescope、Steam 或 GPU。

### 2. KDE 中的嵌套 Gamescope

Gamescope 的 `--mangoapp` 会通过 `PATH` 查找名为 `mangoapp` 的程序。因此开发构建可以只在当前命令中覆盖 `PATH`，不会影响系统服务：

```bash
PATH="/绝对路径/构建目录:$PATH" \
MANGOHUD_CONFIG="fps,frametime,cpu_stats,gpu_stats" \
gamescope -W 1280 -H 800 -w 1280 -h 800 --mangoapp -- vkcube
```

构建目录中必须存在名为 `mangoapp` 的开发二进制。退出嵌套窗口后，测试进程和覆盖层一起结束。

这层可以验证：

- 外部覆盖层窗口是否被 Gamescope 正确识别。
- Alpha、缩放、分辨率和布局。
- Gamescope 帧消息兼容性。
- 多提供者数据显示、更新和消失。
- 显示开关、配置重载和异常输入。

这层不能替代真实游戏模式中的 Steam preset、QAM、HDR、VRR、挂起恢复和服务生命周期测试。

项目应提供 `tools/run-nested.sh`，自动选择开发二进制、启动示例提供者、收集日志并在退出时清理临时文件，避免开发者手工拼接环境变量。

### 3. 真实游戏模式热测试

从桌面模式切换到游戏模式即可，不需要重启设备。进入游戏模式后，通过另一台设备 SSH 连接 SteamOS，完成后续迭代。

开发 drop-in 指向工作区或测试安装目录中的启动保护器。更新二进制后只重启 MangoApp 用户服务：

```bash
systemctl --user daemon-reload
systemctl --user restart gamescope-mangoapp.service
journalctl --user -u gamescope-mangoapp.service -f
```

这会短暂重建性能覆盖层，不会重启 Steam、Gamescope 或当前游戏。测试脚本应在重启前验证二进制，并在服务未就绪时自动恢复系统 MangoApp。

开发期必须提供一条恢复命令，完成以下原子步骤：

1. 删除本项目的用户级 service drop-in。
2. 执行 `systemctl --user daemon-reload`。
3. 重启 `gamescope-mangoapp.service`。
4. 确认实际进程为 `/usr/bin/mangoapp`。

在恢复脚本实现前，不把开发 drop-in 作为默认安装方式分发给用户。

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

## 后续桌面游戏渲染器

桌面模式支持指“从 KDE 中启动游戏后，在游戏画面内绘制”，不是创建一个覆盖整个 KDE 桌面的常驻窗口。该后端使用 MangoHud 的 Vulkan 隐式层和 OpenGL shim。实现后应覆盖 Vulkan、OpenGL、`x86_64`、`i686`、Proton、全屏、无边框和多游戏进程场景，并验证 Steam Runtime 内仍能访问同一 `$XDG_RUNTIME_DIR` IPC。

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

首版实现前应先定义以下工具：

- `mango-overlay --self-test`：不创建窗口，检查运行环境、IPC 和配置。
- `mango-overlay --protocol-probe`：连接当前 Gamescope，报告协议兼容性。
- `mango-overlay-provider-demo`：周期更新几项确定性测试数据。
- `tools/run-nested.sh`：KDE 嵌套 Gamescope 测试。
- `tools/install-dev.sh`：安装开发 drop-in，执行预检后热切换。
- `tools/restore-system.sh`：无条件恢复系统 MangoApp。
- `tools/collect-diagnostics.sh`：收集版本、服务状态和有界日志，不收集隐私数据。

## 最低验收矩阵

| 场景 | 预期结果 |
| --- | --- |
| 无提供者 | 原生 MangoApp 行为不变，无空白第三方区域 |
| 一个提供者更新 | 数值平滑更新，渲染线程不阻塞 |
| 多元素事务提交 | 所有变化在同一快照出现，不显示半更新画面 |
| 游戏模式渲染器重启 | 场景代理保持场景，重连后获得完整快照 |
| 多个提供者同名字段 | 按提供者隔离，不互相覆盖 |
| 提供者崩溃 | 超时后自动移除数据 |
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
