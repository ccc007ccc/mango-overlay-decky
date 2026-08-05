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
~/.local/share/mango-overlay-decky/runtime/       版本化运行时
~/.local/state/mango-overlay-decky/               事务、活动版本和有界日志
~/.config/mango-overlay-decky/                    用户设置
~/.cache/mango-overlay-decky/                     可重新生成的资源缓存
~/.config/systemd/user/
  gamescope-mangoapp.service.d/                   MangoApp 用户级服务 drop-in
  mango-overlayd.socket                           场景代理 socket activation
  mango-overlayd.service                          场景代理进程
```

运行时目录使用版本化子目录，并维护原子 `active` 与 `previous` 指针。最终路径和 Decky 插件标识在实现前固定，之后不得根据模糊名称或目录扫描删除文件。

## 首次安装

新插件 `_main()` 调用稳定生命周期助手：

1. 获取生命周期锁。
2. 清除属于本次安装的过期待确认卸载记录。
3. 把插件包内运行时复制到同文件系统的临时版本目录。
4. 校验清单、文件权限和架构，运行 `--self-test` 与协议兼容检查。
5. 原子重命名为正式版本目录。
6. 原子设置 `active` 指针。
7. 安装指向稳定启动助手的用户级 service drop-in。
8. 安装 `mango-overlayd.socket` 与对应服务单元，执行 `systemctl --user daemon-reload` 并启用 socket。
9. 仅当 `gamescope-mangoapp.service` 原本处于活动状态时热重启；服务未运行时不擅自启动游戏模式组件。
10. 连接场景代理并等待渲染器就绪。失败时恢复系统 MangoApp，并保留诊断信息。

## 插件更新

旧版本 `_uninstall()` 只能：

1. 写入带唯一事务 ID 和时间戳的待确认卸载记录。
2. 请求稳定生命周期助手稍后核对插件是否重新出现。
3. 在 Decky 的五秒停止期限内立即返回。

新版本 `_main()` 启动后：

1. 获取同一生命周期锁。
2. 通过插件标识确认自己是合法替代版本，并取消待确认卸载。
3. 把新运行时写入独立暂存目录并完整验证。
4. 保留当前 `active` 为 `previous`，再原子切换 `active`。
5. 仅在 MangoApp 服务原本活动时热重启并等待新版本就绪。
6. 启动失败则原子恢复 `previous`；再次失败才回退系统 MangoApp。
7. 成功后提交版本状态，并只保留一个已验证回滚版本。

更新过程中断时：

- 切换前中断：删除暂存目录，继续使用旧版本。
- 切换后、提交前中断：下次启动根据事务记录验证活动版本，不能猜测成功。
- Decky 未能安装新插件：插件目录持续缺失，延迟核对转入最终卸载。

项目不提供第二套“更新后台”按钮。插件包、运行时和 Decky 前端作为同一版本发布，由 Decky 更新一次完成。

## 最终卸载

延迟核对只有同时满足以下条件才开始清理：

- 存在有效的待确认卸载记录。
- Decky 插件根目录中不存在相同插件标识的有效 `plugin.json`。
- 记录没有被新版本 `_main()` 取消。
- 当前没有安装或更新事务持有生命周期锁。

清理顺序：

1. 获取生命周期锁并再次验证上述条件。
2. 停止并移除场景代理 socket/service 和本项目的 MangoApp service drop-in，执行 `daemon-reload`。
3. 如果 MangoApp 服务正在运行，热重启并验证已回到 `/usr/bin/mangoapp`；服务未运行则保持未运行。
4. 删除版本化运行时、资源缓存、IPC 残留、项目设置和常规日志。
5. 最后删除生命周期状态和稳定助手；只有清理失败时保留最小诊断记录，待恢复成功后再删除。

不得递归删除未经清单验证的路径，不得跟随符号链接，也不得因为文件所有权或 schema 无法确认而强行清理。

## 休眠、关机和普通重载

这些事件只允许执行：

- 停止接收新的 IPC 请求。
- 释放进程内资源。
- 关闭日志和套接字。
- 让 systemd 正常结束当前渲染器进程。

这些事件禁止执行：

- 写入待确认卸载。
- 删除运行时、配置、缓存或 drop-in。
- 改变 `active`/`previous` 指针。
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
| 桌面/游戏模式切换 | 不变 | 对应渲染器按会话启停 |
| 休眠/恢复 | 不变 | 恢复活动版本，不产生卸载记录 |
| 关机/开机 | 不变 | 开机后继续使用活动版本 |
| Decky 更新成功 | 切换到新版本 | 验证后热切换，失败回滚 |
| Decky 更新中断 | 旧版本或可验证的新版本 | 不留下半安装活动版本 |
| 用户从 Decky 卸载 | 最终变为 absent | 恢复系统 MangoApp 并清理所属文件 |
