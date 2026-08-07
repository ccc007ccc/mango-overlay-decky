# 安装、更新与卸载生命周期

## 不变量

以下规则优先于具体实现：

1. 安装状态和运行会话是两套独立状态。
2. 休眠、关机、重启、切换桌面/游戏模式、Steam 重启、Decky 重启、插件后端重载和渲染器退出都不是卸载或更新。
3. `_unload` 不删除文件、不移除 systemd drop-in、不切换活动版本，也不写入卸载意图。
4. Decky 的 `_uninstall` 回调不能直接视为用户最终卸载，因为 Loader 更新插件时也会调用它。
5. 安装和更新必须先验证新版本，再原子切换；失败时保留并恢复上一个可用版本。
6. 最终卸载必须恢复系统 `/usr/bin/mangoapp`，然后只删除本项目明确拥有的文件。
7. 所有生命周期操作都必须幂等，并由同一把跨进程锁串行化。
8. Decky 的“禁用插件”不等于关闭扩展覆盖层；显示状态只由项目自身总开关改变。

## Loader 的真实行为

该结论已对照 Decky Loader `origin/main` 提交 `3e4302e`：更新路径位于 `backend/decky_loader/browser.py`，插件停止与回调顺序位于 `backend/decky_loader/plugin/sandboxed_plugin.py` 和 `plugin.py`。

当前 Decky Loader 在更新已安装插件时执行：

```text
下载完整插件包
  -> uninstall_plugin(name)
     -> stop(uninstall=True)
        -> _unload()
        -> _uninstall()
     -> 删除旧插件目录
  -> 解压新插件目录
  -> 启动新版本 _main()
```

普通 Loader 退出、重启、后端热重载和禁用插件只调用 `stop()`，因此只会进入 `_unload()`。

结论：`_uninstall()` 表示“Loader 正在移除当前插件包”，不表示“用户确定永远删除项目”。

Decky 的非 root 插件后端不保证继承登录会话的 `XDG_RUNTIME_DIR` 或 `DBUS_SESSION_BUS_ADDRESS`。插件后端启动任何需要访问当前用户运行时的子进程时，都必须根据自身有效 UID 构造 `/run/user/<uid>` 和对应用户总线地址；这同时适用于生命周期助手的 `systemctl --user` 和管理页面调用的 `mango-overlayctl`，不能依赖 Loader 环境。

## 两个正交状态机

### 安装状态

```text
absent
  -> installing
  -> installed(version)
  -> updating(old, new)
  -> installed(new)

installed(version)
  -> pending_uninstall
  -> installed(new)       新版本出现，取消待确认卸载
  -> uninstalling         插件保持缺失，执行最终卸载
  -> absent
```

### 运行状态

```text
stopped -> starting -> ready
                    -> system_fallback
ready   -> stopped
ready   -> failed -> system_fallback
```

运行状态变化不得隐式修改安装状态。系统向进程发送 `SIGTERM`、图形会话退出或设备关机时，只结束当前运行会话。

## 文件归属

建议的稳定位置：

```text
~/homebrew/plugins/mango-overlay-decky/          Decky 当前插件包
~/.local/libexec/mango-overlay-decky/            稳定启动与生命周期助手
~/.local/share/mango-overlay-decky/runtime/       内容寻址的不可变运行时修订
~/.local/share/mango-overlay-decky/coordinator/   共享核心声明、活动指针和失败修订
~/.local/state/mango-overlay-decky/               事务、活动版本和有界日志
~/.config/mango-overlay-decky/                    用户设置
~/.cache/mango-overlay-decky/                     可重新生成的资源缓存
~/homebrew/settings/mango-overlay-decky/          Decky 设置目录
~/homebrew/data/mango-overlay-decky/              Decky 运行数据目录
~/homebrew/logs/mango-overlay-decky/              Decky 普通日志目录
~/.config/systemd/user/
  gamescope-mangoapp.service.d/                   MangoApp 用户级服务 drop-in
  mango-overlayd.socket                           场景代理 socket activation
  mango-overlayd.service                          场景代理进程
```

每份运行时清单经规范化后生成 SHA-256 修订 ID，目录以修订 ID 命名。`install.json` 分别记录对外的 `active_version` / `previous_version`、内部的 `active_runtime` / `previous_runtime` 和当前插件 generation。相同版本、相同内容的 Loader 重载复用修订且不重启；相同版本、不同内容的测试包按正常更新事务切换到新修订，旧修订仍可回滚。路径和 Decky 插件标识已经固定，清理不得根据模糊名称或目录扫描删除文件。

## 多产品共享核心

生命周期所有权由本项目的 Runtime Coordinator 统一维护；产品插件不得复制
`LifecycleManager` 后各自争抢 drop-in。每个产品先把自己的不可变运行时修订复制到共享
`runtime/versions/<sha256>`，再提交有界声明。

快速路径按以下规则实现：

1. 每个产品只校验并登记自己的 candidate manifest 和持久 claim。
2. claim 变化时在共享锁内读取一份有界索引，按 Mango 核心语义版本选择最大候选；
   一百个 claim 只产生版本比较，不启动一百个进程。
3. 选择结果没有变化时不重新哈希全部候选、不运行自检、不重启服务。
4. 只有新的最大候选需要激活时才验证该内容修订；已经验证的相同修订复用结果。
5. 验证或启动失败只隔离该精确修订，再降序尝试下一个；同版本不同内容必须显式报冲突。
6. 稳定启动器平时只读取原子提交的活动指针；索引扫描只用于 claim 事务或损坏恢复。

候选的 provider protocol 范围用于激活后的握手和错误报告，不用于并行挑选第二个旧核心。
旧产品与最新版核心不兼容时进入明确失败状态；活动核心仍保持唯一。`_unload` 不删除
claim，`_uninstall` 只标记当前产品 pending removal，最后一个 claim 被确认移除后才执行
本文件定义的最终卸载。

稳定助手提供以下最小命令面，输出为有界 JSON：

```text
launcher.py coordinator status
launcher.py coordinator register ...
launcher.py coordinator pending-remove ...
launcher.py coordinator finalize ...
launcher.py coordinator retry [content-revision]
```

`register` 只比较候选的 Mango 核心语义版本；产品插件版本不参与比较。结构校验会确认
候选修订已经落盘，但只有被选中的最高候选才运行二进制自检。活动指针由协调器状态文件
拥有，启动器读取它而不是根据目录猜测。最后一个声明确认移除后才恢复系统 MangoApp；
仍有其他声明时只切换到下一个可用核心并保留共享运行时。

第一版活动修订只拥有游戏模式所需文件：

```text
runtime/bin/                                  MangoApp、代理、控制器和测试提供者
runtime/lib/                                  提供者客户端库与游戏模式私有依赖
runtime/licenses/                             随包私有库许可证
```

桌面 `libMangoHud`/shim、`runtime/lib32` 和 Vulkan layer manifest 不进入正式运行时。
它们的源码与开发工具仍保留在仓库中，但不参与安装、更新或回滚。

## 首次安装

新插件 `_main()` 调用稳定生命周期助手：

1. 根据清单生成运行时修订 ID，并把插件包内运行时复制到共享候选目录。
2. 结构校验候选清单并提交自己的持久声明。
3. 协调器按核心语义版本选出唯一候选；只有被选中的候选运行游戏模式二进制自检。
4. 获取生命周期锁，原子切换活动修订并提交安装状态。
5. 清除属于本次安装的过期待确认卸载记录。
6. 安装指向稳定启动助手的用户级 service drop-in。
7. 安装 `mango-overlayd.socket` 与对应服务单元，执行 `systemctl --user daemon-reload` 并启用 socket。
8. 仅当 `gamescope-mangoapp.service` 原本处于活动状态时热重启；服务未运行时不擅自启动游戏模式组件。
9. 连接场景代理并等待渲染器就绪。失败时隔离该修订并回退已知可用核心或系统 MangoApp。

从没有协调器状态的旧安装迁移时，首次声明会先读取旧活动修订作为 known-good；新候选
失败不会先拆掉这份活动指针。只有确认没有任何声明时才恢复系统 MangoApp。

## 插件更新

旧版本 `_uninstall()` 只能：

1. 在插件事件循环线程中原子写入带唯一 token、generation、时间戳以及旧插件目录
   device/inode 的待确认卸载记录。
2. 用一秒上限的 `systemctl --user --no-block restart` 请求稳定生命周期助手稍后核对，
   不同步启动或等待 cleanup oneshot。
3. 在 Decky 的五秒停止期限内立即返回。

cleanup timer 的确认窗口为 3 秒；只要旧目录仍存在、同路径出现身份不明的半解压目录，
或扫描无法安全完成，就保持旧运行时并每 5 秒重试。timer 安装时只执行 `enable`，作为
用户管理器重启后恢复遗留 pending 的保险，不在正常安装时启动；pending 被替代版本取消
或根本不存在时会立即停止，不在正常运行中周期唤醒 cleanup service。

若首次安装尚未提交，`_uninstall()` 先按事务记录恢复安装前状态；恢复后没有活动版本即视为半安装已清理，不创建待确认卸载。

新版本 `_main()` 启动后：

1. 把新运行时写入共享候选目录并提交新的 generation 声明，取消自己的待确认卸载。
2. 同产品低核心版本替换被拒绝；新最高版本才进入激活事务。
3. 保留当前活动修订为已知可用版本，再原子切换活动修订。
4. 仅在 MangoApp 服务原本活动时热重启并等待新版本就绪。
5. 启动失败则隔离精确修订并按版本向下回退；再次失败才回退系统 MangoApp。
6. 成功后提交协调器活动指针和安装状态。

同一产品后端重载若仍提交相同内容修订，只刷新该产品 claim 的 generation，不重新运行自检
或重启渲染器。共享核心中的非活动产品也会写入自己的待确认卸载记录；最终核对先移除
对应 claim，只有最后一个 claim 消失才执行全局清理。

更新过程中断时：

- 切换前中断：删除暂存目录，继续使用旧版本。
- 切换后、提交前中断：下次启动根据事务记录验证活动版本，不能猜测成功。
- Decky 未能安装新插件：插件目录持续缺失，延迟核对转入最终卸载。

项目不提供第二套“更新后台”按钮。插件包、运行时和 Decky 前端作为同一版本发布，由 Decky 更新一次完成。

## 遗留桌面启动项

第一版不要求也不支持用项目启动项注入 KDE 游戏。为了让曾测试双模式包的用户安全
更新，稳定启动器仍识别：

```text
~/.local/libexec/mango-overlay-decky/launcher.py desktop -- %command%
```

当活动运行时没有完整桌面实验产物时，启动器只使用继承环境原样 `exec` 游戏，不设置
`MANGOHUD_CONFIG`、`LD_PRELOAD`、Vulkan layer 或场景 socket。该兼容行为避免遗留启动
项阻止游戏或覆盖 Steam 原生性能统计；用户仍应删除启动项并恢复默认启动方式。

## 最终卸载

延迟核对只有同时满足以下条件才开始清理：

- 存在有效的待确认卸载记录。
- 记录中的原旧插件目录 device/inode 已经消失。
- 旧路径没有处于半解压、身份不明的状态。
- 在旧路径父目录最多 256 个条目的有界扫描中，不存在相同插件标识的有效
  `plugin.json`；旧版没有目录身份字段的 pending 记录也按这条安全路径兼容处理。
- 记录没有被新版本 `_main()` 取消。
- 当前没有安装或更新事务持有生命周期锁。

清理顺序：

1. 获取生命周期锁并再次验证上述条件。
2. 停止并移除场景代理 socket/service 和本项目的 MangoApp service drop-in，执行 `daemon-reload`。
3. 如果 MangoApp 服务正在运行，热重启并验证已回到 `/usr/bin/mangoapp`；服务未运行则保持未运行。
4. 删除版本化运行时、资源缓存、IPC 残留、项目设置和常规日志，并精确删除 Decky
   为本插件保留的 `settings/mango-overlay-decky`、`data/mango-overlay-decky` 和
   `logs/mango-overlay-decky`；不得删除这些目录的父目录或其他插件的相邻数据。
5. 最后删除生命周期状态和稳定助手；只有清理失败时保留最小诊断记录，待恢复成功后再删除。

不得递归删除未经清单验证的路径，不得跟随符号链接，也不得因为文件所有权或 schema 无法确认而强行清理。

## 紧急恢复系统 MangoApp

真实会话出现异常时，可以保留运行时、设置和安装状态，只移除本项目的 MangoApp drop-in 并恢复系统服务：

```bash
python3 ~/.local/libexec/mango-overlay-decky/lifecycle.py \
  restore-system --home "$HOME"
```

命令只在 `gamescope-mangoapp.service` 原本运行时重启它；不会卸载插件、删除版本或写入待确认卸载。之后重新加载插件会恢复受管 drop-in，但当前会话保持系统 MangoApp，直到服务下次重启。

## 休眠、关机和普通重载

这些事件只允许执行：

- 停止接收新的 IPC 请求。
- 释放进程内资源。
- 关闭日志和套接字。
- 让 systemd 正常结束当前渲染器进程。

这些事件禁止执行：

- 写入待确认卸载。
- 删除运行时、配置、缓存或 drop-in。
- 改变活动版本或回滚版本。
- 将正常 `SIGTERM` 计为崩溃。
- 在关机过程中启动更新、回滚或最终卸载。

下一次图形会话启动时，稳定启动助手读取原有安装状态并正常启动活动版本。

## 验收矩阵

| 事件 | 安装状态 | 运行结果 |
| --- | --- | --- |
| Decky 后端重载 | 不变 | 管理后端重连，覆盖层继续运行 |
| Decky Loader 重启 | 不变 | 覆盖层继续运行或按会话正常重启 |
| 插件禁用 | 不变 | 只停止管理前后端；覆盖层保持项目总开关的既有状态 |
| Steam 重启 | 不变 | 不清理任何文件 |
| 桌面/游戏模式切换 | 不变 | 桌面不启动项目渲染器；进入游戏模式后按服务启动活动版本 |
| 休眠/恢复 | 不变 | 恢复活动版本，不产生卸载记录 |
| 关机/开机 | 不变 | 开机后继续使用活动版本 |
| Decky 更新成功 | 切换到新版本 | 验证后热切换，失败回滚 |
| 更新时新目录仍在解压 | 保持 pending | 不清理旧运行时，等待目录身份明确或新版本取消 |
| Decky 更新中断 | 旧版本或可验证的新版本 | 不留下半安装活动版本 |
| 用户从 Decky 卸载 | 数秒内最终变为 absent | 恢复系统 MangoApp 并清理所属文件 |
