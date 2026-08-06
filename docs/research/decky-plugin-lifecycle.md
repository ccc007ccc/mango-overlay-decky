# Decky 插件安装、更新与卸载生命周期源码研究

研究日期：2026-08-06

本笔记只使用一手来源：Decky Loader 源码、SteamDeckHomebrew 官方插件数据库中的 gitlink，以及插件自身在该 gitlink 固定提交上的源码。研究快照为：

- Decky Loader：[`b4b8be3297e427dad6fbc6697ffdb765a796f7fd`](https://github.com/SteamDeckHomebrew/decky-loader/tree/b4b8be3297e427dad6fbc6697ffdb765a796f7fd)
- Decky Plugin Database：[`454d4c85f11ded263df1e84e016a861e744191b8`](https://github.com/SteamDeckHomebrew/decky-plugin-database/tree/454d4c85f11ded263df1e84e016a861e744191b8)
- 数据库固定的插件模板：[`cadd0bee9e3f2df1b788bb0e6353bab1af1455e2`](https://github.com/SteamDeckHomebrew/decky-plugin-template/tree/cadd0bee9e3f2df1b788bb0e6353bab1af1455e2)
- 插件模板当前 `main`：[`90d0780e882a17f5714fc6de044c645f22608290`](https://github.com/SteamDeckHomebrew/decky-plugin-template/tree/90d0780e882a17f5714fc6de044c645f22608290)

数据库样本不是随意挑选的 GitHub 项目。官方数据库在固定快照中明确登记了 CSS Loader、TunnelDeck、DeckMTP、Recorder、Syncthing、XR Gaming、Sunshine、SpoofDPI、Framegen、DoT DNS 和 Virtual Surround Sound；对应登记可见 [`.gitmodules` L11-L13](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L11-L13)、[L53-L55](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L53-L55)、[L75-L80](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L75-L80)、[L132-L134](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L132-L134)、[L177-L179](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L177-L179)、[L192-L194](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L192-L194)、[L228-L237](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L228-L237) 和 [L247-L262](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/.gitmodules#L247-L262)。

## 插件数据库本身不定义运行时生命周期

Decky Plugin Database 是商店目录和构建入口，不是安装器。插件以 git submodule 固定源码版本；更新商店版本的做法是推进对应 gitlink 并递增 `package.json` 版本，参见数据库 [`README.md` L3-L25](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/README.md#L3-L25)。数据库 builder 只安装前端依赖、执行插件构建，再把产物复制到输出目录；它不调用插件的 `_main()`、`_unload()` 或 `_uninstall()`，参见 [`builder/entrypoint.sh` L1-L17](https://github.com/SteamDeckHomebrew/decky-plugin-database/blob/454d4c85f11ded263df1e84e016a861e744191b8/builder/entrypoint.sh#L1-L17)。

因此需要分别看两层：数据库决定“商店发布哪个源码快照”，Decky Loader 决定“设备上如何替换插件包”，插件自身决定“如何处理它在插件目录之外创建的状态”。

## 结论

1. **Decky 的普通插件更新会调用旧版本的同一个 `_uninstall()`。** Loader 没有向插件传递“用户卸载”或“更新替换”的原因，插件仅收到一个 `uninstall: true` 布尔值。因此 `_uninstall()` 不能直接证明用户要永久删除插件。
2. **`_unload()` 与 `_uninstall()` 是叠加关系，不是二选一。** 所有后端停止先执行 `_unload()`；只有 `stop(uninstall=True)` 才随后执行 `_uninstall()`。重载、禁用和 Loader 关闭只执行 `_unload()`。
3. **Decky Loader 只保证删除当前插件目录。** 它不替插件清理 `settings`、`data/runtime`、`logs`、用户文件、systemd unit、Flatpak、系统配置或游戏目录中的文件。官方 README 也明确提醒卸载只移除插件文件，不移除插件创建的其他文件。
4. **商店插件没有统一的卸载策略。** 样本中同时存在：在 `_unload()` 清理运行态、在 `_uninstall()` 立即破坏性清理、完全保留外部软件和用户数据、把清理留给单独 UI 操作，以及明显不完整或存在竞态的钩子。不能把“已进入官方数据库”理解为某一种生命周期模式已经成为规范。
5. **Mango Overlay Decky 可以让真实卸载在数秒内完成，但不应在旧插件进程的 `_uninstall()` 内直接删除运行时。** 最合适的折中是保留插件目录之外的稳定 finalizer，将当前 60 秒确认窗口缩短为一个很短的替换确认窗口，并在确认同名插件没有重新出现后立即恢复系统 MangoApp 和删除项目所属文件。

## Decky Loader 的真实调用链

### 用户卸载

当前 Loader 的卸载路径是：

```text
前端确认卸载
  -> utilities/uninstall_plugin(name)
  -> PluginBrowser.uninstall_plugin(name)
     -> 通知前端卸载插件 UI
     -> PluginWrapper.stop(uninstall=True)
        -> 向插件进程写入 {"uninstall": true}
        -> 对插件进程发送 SIGTERM
        -> SandboxedPlugin.shutdown()
           -> _unload()
           -> _uninstall()
     -> 删除 Loader 自己保存的插件排序/隐藏/禁用状态
     -> rmtree(plugin_dir)
```

`uninstall_plugin()` 的实现明确先停止旧后端，然后只对 `plugin_dir` 执行 `rmtree()`；参见 [`browser.py` L139-L171](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/browser.py#L139-L171)。`stop(uninstall=True)` 只发送一个布尔标志，随后终止进程；参见 [`plugin.py` L147-L172](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/plugin/plugin.py#L147-L172)。插件进程收到 SIGTERM 后总是先 `_unload()`，`uninstalling` 为真时再 `_uninstall()`；参见 [`sandboxed_plugin.py` L146-L196](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/plugin/sandboxed_plugin.py#L146-L196)。

钩子不能做无限期工作。Loader 在进程停止约 5 秒后会发送 SIGKILL；参见 [`plugin.py` L176-L190](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/plugin/plugin.py#L176-L190)。此外，`_unload()` 和 `_uninstall()` 的异常只会被记录，Loader 仍会继续删除插件目录；参见 [`sandboxed_plugin.py` L146-L174](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/plugin/sandboxed_plugin.py#L146-L174)。所以耗时或必须可靠完成的清理应交给插件目录之外、可重试且幂等的助手。

### 普通更新

普通更新不是“覆盖解压”。当前链路是：

```text
把新 zip 下载到内存
  -> 发现同名插件已经安装
  -> 调用与用户卸载完全相同的 uninstall_plugin(name)
  -> 删除旧插件目录
  -> 此时才校验新 zip 的 SHA-256 并解压
  -> 下载 package.json 声明的远程二进制
  -> 导入新版本 _main()
```

更新对已安装插件直接调用 `uninstall_plugin(name)`，随后才 `_unzip_to_plugin_dir()`、下载远程二进制并导入新版本；参见 [`browser.py` L262-L305](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/browser.py#L262-L305)。zip 的散列校验位于解压函数内部；参见 [`browser.py` L58-L64](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/browser.py#L58-L64)。因此，一个已经下载完成但散列错误的更新会在发现错误前先删除旧插件；远程二进制下载失败也发生在旧插件删除之后。

Loader 内部虽然定义了 `UPDATE`、`REINSTALL` 等安装类型，但 `PluginInstallContext` 不保存该类型，确认安装时也只把 artifact、name、version 和 hash 传给 `_install()`；参见 [`browser.py` L29-L48](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/browser.py#L29-L48) 和 [L307-L324](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/browser.py#L307-L324)。旧插件后端因此无法从官方回调区分更新和最终卸载。

### 普通重载、禁用和 Loader 退出

后端重载最终调用 `import_plugin()`；若同名后端已存在，只调用不带卸载标志的 `stop()`，再启动新进程；参见 [`loader.py` L161-L183](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/loader.py#L161-L183) 和 [L201-L204](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/loader.py#L201-L204)。插件禁用同样只调用 `stop()`；参见 [`utilities.py` L480-L507](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/utilities.py#L480-L507)。这些路径只应释放进程和当前会话资源，不能删除安装状态。

### Loader 不负责插件外部数据

Loader 给每个插件暴露独立的 settings、runtime/data 和 logs 目录；参见 [`sandboxed_plugin.py` L74-L88](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/plugin/sandboxed_plugin.py#L74-L88)。卸载时 `cleanup_plugin_settings()` 清理的只是 Loader 自己的 frozen、hidden、order 和 disabled 列表；参见 [`browser.py` L329-L356](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/backend/decky_loader/browser.py#L329-L356)。官方 README 明确写道，卸载插件只删除插件文件，不删除插件创建的其他文件；参见 [`README.md` L94-L103](https://github.com/SteamDeckHomebrew/decky-loader/blob/b4b8be3297e427dad6fbc6697ffdb765a796f7fd/README.md#L94-L103)。

## 官方模板表达的语义

需要区分数据库固定模板和模板仓库当前 `main`：

- 官方数据库快照固定在 `cadd0bee` 的 `ci-testing` 模板，该版本只有 `_unload()`，没有 `_uninstall()`；参见 [`main.py` L14-L25](https://github.com/SteamDeckHomebrew/decky-plugin-template/blob/cadd0bee9e3f2df1b788bb0e6353bab1af1455e2/main.py#L14-L25)。
- 模板当前 `main` 已包含 `_uninstall()`。注释把 `_unload()` 描述为“插件停止但未完全移除”时的清理，把 `_uninstall()` 描述为 `_unload()` 之后清理系统遗留；参见 [`main.py` L24-L34](https://github.com/SteamDeckHomebrew/decky-plugin-template/blob/90d0780e882a17f5714fc6de044c645f22608290/main.py#L24-L34)。该钩子由提交 [`d69496d`](https://github.com/SteamDeckHomebrew/decky-plugin-template/commit/d69496d4adc30597bef8fdec377fd280ac9f1a00)（PR #34）加入。

模板说明了两个钩子的设计意图，但没有说明 Loader 更新也走 `_uninstall()`。实现源码仍是判断真实行为的最终依据。

## 商店插件源码样本

下表使用官方数据库 `454d4c85` 固定的 gitlink 提交，而不是各仓库后来可能变化的分支头。

| 插件与固定提交 | 外部状态 | `_unload()` | `_uninstall()` / Decky 卸载结果 | 更新风险与评价 |
| --- | --- | --- | --- | --- |
| [Decky SpoofDPI `4d3dd36`](https://github.com/ohaiibuzzle/decky-spoof-dpi/tree/4d3dd361d57132f95cfe5ef028b764658a3ed576) | 插件内二进制，加上 `~/.steam/steam/config/proxyconfig.vdf`；代理文件会指向本地 SpoofDPI 进程，见 [`constants.py` L1-L6](https://github.com/ohaiibuzzle/decky-spoof-dpi/blob/4d3dd361d57132f95cfe5ef028b764658a3ed576/py_modules/constants.py#L1-L6) 和 [`spoofdpi_control.py` L9-L24](https://github.com/ohaiibuzzle/decky-spoof-dpi/blob/4d3dd361d57132f95cfe5ef028b764658a3ed576/py_modules/spoofdpi_control.py#L9-L24)。 | 杀死子进程，但特意不重置 Steam 代理配置；见 [`main.py` L14-L20](https://github.com/ohaiibuzzle/decky-spoof-dpi/blob/4d3dd361d57132f95cfe5ef028b764658a3ed576/main.py#L14-L20) 和 [L61-L64](https://github.com/ohaiibuzzle/decky-spoof-dpi/blob/4d3dd361d57132f95cfe5ef028b764658a3ed576/main.py#L61-L64)。 | 重置 `auto_start`、删除代理配置；见 [`main.py` L66-L70](https://github.com/ohaiibuzzle/decky-spoof-dpi/blob/4d3dd361d57132f95cfe5ef028b764658a3ed576/main.py#L66-L70) 和 [`spoofdpi_control.py` L27-L41](https://github.com/ohaiibuzzle/decky-spoof-dpi/blob/4d3dd361d57132f95cfe5ef028b764658a3ed576/py_modules/spoofdpi_control.py#L27-L41)。 | 更新会走 `_uninstall()`，把自动启动设置改为 false；新版本 `_main()` 因而不会恢复代理。普通重载则有代理文件暂时指向已停止进程的窗口。 |
| [Decky DoT DNS `5ba1f43`](https://github.com/ohaiibuzzle/decky-dot-dns/tree/5ba1f43010ebbe2e5f582cc0cb5c7203a70efa22) | root 插件写 `/etc/systemd/resolved.conf.d/decky-dot-dns.conf` 并重启 `systemd-resolved`；见 [`main.py` L11-L12](https://github.com/ohaiibuzzle/decky-dot-dns/blob/5ba1f43010ebbe2e5f582cc0cb5c7203a70efa22/main.py#L11-L12) 和 [L55-L78](https://github.com/ohaiibuzzle/decky-dot-dns/blob/5ba1f43010ebbe2e5f582cc0cb5c7203a70efa22/main.py#L55-L78)。 | 空操作。 | 删除 drop-in；`drop_custom_dns_override()` 已重启一次服务，`_uninstall()` 又重启一次；见 [`main.py` L95-L123](https://github.com/ohaiibuzzle/decky-dot-dns/blob/5ba1f43010ebbe2e5f582cc0cb5c7203a70efa22/main.py#L95-L123)。 | 更新会立即移除生效中的 DNS 配置并重启服务两次。固定版本没有在新 `_main()` 中自动重应用设置，所以更新后覆盖可能保持关闭，直到用户再次操作。 |
| [XR Gaming `646d431`](https://github.com/wheaney/decky-XRGaming/tree/646d431c19cb361c91cd3adf10a91d9ca886feda) | 安装 Breezy Vulkan/XR driver 到 `~/.local/bin`、`~/.local/share/breezy_vulkan` 等插件目录之外的位置；见 [`main.py` L114-L143](https://github.com/wheaney/decky-XRGaming/blob/646d431c19cb361c91cd3adf10a91d9ca886feda/main.py#L114-L143) 和 [L153-L203](https://github.com/wheaney/decky-XRGaming/blob/646d431c19cb361c91cd3adf10a91d9ca886feda/main.py#L153-L203)。 | 空操作。 | 立即执行两个外部卸载脚本，并清空版本/manifest 状态；见 [`main.py` L223-L241](https://github.com/wheaney/decky-XRGaming/blob/646d431c19cb361c91cd3adf10a91d9ca886feda/main.py#L223-L241)。 | 更新同样卸掉驱动；新 `_main()` 只保存事件循环，没有自动重装逻辑，见 [`main.py` L211-L221](https://github.com/wheaney/decky-XRGaming/blob/646d431c19cb361c91cd3adf10a91d9ca886feda/main.py#L211-L221)。这是“直接清理最简单，但更新会退化”的明确样本。 |
| [TunnelDeck `d1b5058`](https://github.com/bkohler616/TunnelDeck/tree/d1b50581664b619aeb6372416a65cdf11467a4ab) | 构造并链接 `/var/lib/extensions/networkmanager-openvpn.raw`，启用 `systemd-sysext`，刷新扩展并重启 NetworkManager；见 [`install` L3-L48](https://github.com/bkohler616/TunnelDeck/blob/d1b50581664b619aeb6372416a65cdf11467a4ab/defaults/extensions/install#L3-L48)。 | 无条件运行卸载脚本；脚本删 sysext 链接、刷新扩展并重启 NetworkManager，见 [`main.py` L113-L124](https://github.com/bkohler616/TunnelDeck/blob/d1b50581664b619aeb6372416a65cdf11467a4ab/main.py#L113-L124) 和 [`uninstall` L1-L4](https://github.com/bkohler616/TunnelDeck/blob/d1b50581664b619aeb6372416a65cdf11467a4ab/defaults/extensions/uninstall#L1-L4)。 | 没有单独 `_uninstall()`。 | 重载、禁用、Loader 退出、更新和真正卸载都会先移除 sysext 并重启网络；若设置仍启用，新 `_main()` 再安装。它说明把安装状态清理放进 `_unload()` 会扩大中断面。 |
| [Virtual Surround Sound `b9996c3`](https://github.com/DeckSettings/decky-virtual-surround-sound/tree/b9996c3f3f76b780c9dbb783d180128a582ab0ce) | `~/.config/systemd/user/virtual-surround-sound.service`、PipeWire HRIR 文件和持续运行的用户服务；见 [`service.sh` L192-L216](https://github.com/DeckSettings/decky-virtual-surround-sound/blob/b9996c3f3f76b780c9dbb783d180128a582ab0ce/defaults/service.sh#L192-L216) 和 [L334-L369](https://github.com/DeckSettings/decky-virtual-surround-sound/blob/b9996c3f3f76b780c9dbb783d180128a582ab0ce/defaults/service.sh#L334-L369)。 | 只取消插件自己的后台检查任务，不停止 systemd 服务；见 [`main.py` L88-L100](https://github.com/DeckSettings/decky-virtual-surround-sound/blob/b9996c3f3f76b780c9dbb783d180128a582ab0ce/main.py#L88-L100)。 | 用 `P_NOWAIT` 后台启动 service 卸载脚本，立即删除 HRIR 并返回；见 [`main.py` L102-L115](https://github.com/DeckSettings/decky-virtual-surround-sound/blob/b9996c3f3f76b780c9dbb783d180128a582ab0ce/main.py#L102-L115)。 | 新 `_main()` 会重新安装服务，见 [`main.py` L77-L85](https://github.com/DeckSettings/decky-virtual-surround-sound/blob/b9996c3f3f76b780c9dbb783d180128a582ab0ce/main.py#L77-L85) 和 [L156-L162](https://github.com/DeckSettings/decky-virtual-surround-sound/blob/b9996c3f3f76b780c9dbb783d180128a582ab0ce/main.py#L156-L162)。但旧版本的后台卸载与新版本安装可并发，存在旧卸载脚本最后把新 unit 删除的竞态。 |
| [Decky Sunshine `70ce9d4`](https://github.com/s0t7x/decky-sunshine/tree/70ce9d4436a50a8109fd77326f5a05aba592be52) | 安装系统级 Flatpak `dev.lizardbyte.app.Sunshine`，并把 `bwrap` 复制到 Decky runtime/data 目录；运行时把副本改为 root 所有且 setuid，见 [`sunshine.py` L57-L80](https://github.com/s0t7x/decky-sunshine/blob/70ce9d4436a50a8109fd77326f5a05aba592be52/py_modules/sunshine.py#L57-L80)、[L190-L204](https://github.com/s0t7x/decky-sunshine/blob/70ce9d4436a50a8109fd77326f5a05aba592be52/py_modules/sunshine.py#L190-L204) 和 [L500-L518](https://github.com/s0t7x/decky-sunshine/blob/70ce9d4436a50a8109fd77326f5a05aba592be52/py_modules/sunshine.py#L500-L518)。 | 只记录日志，不停止 Sunshine；见 [`main.py` L150-L154](https://github.com/s0t7x/decky-sunshine/blob/70ce9d4436a50a8109fd77326f5a05aba592be52/main.py#L150-L154)。 | 没有 `_uninstall()`。 | Decky 卸载只删插件目录；系统 Flatpak、运行时 bwrap、副本权限、设置和可能仍运行的 Sunshine 都不由插件卸载清理。更新则利用这些外部状态连续运行。 |
| [Decky Syncthing `464fd7c`](https://github.com/theCapypara/steamdeck-decky-syncthing/tree/464fd7ca05ee0bea8daa8d5d125dfc672cf08942) | settings 中保存服务/Flatpak模式和 API 凭据；插件启动一个脱离会话的 watchdog，它管理 Syncthing 及其 systemd 服务，见 [`main.py` L17-L60](https://github.com/theCapypara/steamdeck-decky-syncthing/blob/464fd7ca05ee0bea8daa8d5d125dfc672cf08942/main.py#L17-L60) 和 [L101-L133](https://github.com/theCapypara/steamdeck-decky-syncthing/blob/464fd7ca05ee0bea8daa8d5d125dfc672cf08942/main.py#L101-L133)。 | 没有 `_unload()`。源码明确说 watchdog **不会**被插件停止，并解释这是为了跨 Decky 的不可靠重载继续工作；见 [`main.py` L101-L115](https://github.com/theCapypara/steamdeck-decky-syncthing/blob/464fd7ca05ee0bea8daa8d5d125dfc672cf08942/main.py#L101-L115)。 | 没有 `_uninstall()`。 | 更新连续性强，但真正卸载后 watchdog、Syncthing 服务、设置和凭据仍可能存在。它代表“插件只是控制面，外部服务生命周期独立”的取舍。 |
| [Decky Recorder `aa91a45`](https://github.com/safijari/decky-recorder-fork/tree/aa91a45dfa388fa892f8d08513dd769e372aec06) | 用户录制文件默认位于 `~/Videos`，滚动缓存位于 `/dev/shm`；见 [`main.py` L73-L75](https://github.com/safijari/decky-recorder-fork/blob/aa91a45dfa388fa892f8d08513dd769e372aec06/main.py#L73-L75)。 | 若正在录制则停止捕获并保存设置；见 [`main.py` L300-L327](https://github.com/safijari/decky-recorder-fork/blob/aa91a45dfa388fa892f8d08513dd769e372aec06/main.py#L300-L327)。 | 没有 `_uninstall()`。 | 更新会短暂停止录制，但不会删除用户视频；真正卸载也保留视频。这是用户产物不应由插件卸载删除的合理样本。 |
| [CSS Loader `b58bdbb`](https://github.com/DeckThemes/SDH-CssLoader/tree/b58bdbbe2e2782403e00257885f544a080fcd16a) | 主题保存在 `~/homebrew/themes`，并在 Steam `steamui/themes_custom` 创建 symlink；见 [`css_utils.py` L68-L80](https://github.com/DeckThemes/SDH-CssLoader/blob/b58bdbbe2e2782403e00257885f544a080fcd16a/css_utils.py#L68-L80) 和 [L116-L126](https://github.com/DeckThemes/SDH-CssLoader/blob/b58bdbbe2e2782403e00257885f544a080fcd16a/css_utils.py#L116-L126)。 | 没有 `_unload()`；`_main()` 创建 symlink、加载主题并启动观察任务，见 [`main.py` L214-L243](https://github.com/DeckThemes/SDH-CssLoader/blob/b58bdbbe2e2782403e00257885f544a080fcd16a/main.py#L214-L243)。 | 没有 `_uninstall()`。 | Decky 卸载保留主题、STORE 状态和 symlink。主题只有用户在插件 UI 中明确删除时才递归删除，见 [`css_theme.py` L138-L162](https://github.com/DeckThemes/SDH-CssLoader/blob/b58bdbbe2e2782403e00257885f544a080fcd16a/css_theme.py#L138-L162)。 |
| [Decky Framegen `291bc51`](https://github.com/xXJSONDeruloXx/Decky-Framegen/tree/291bc511329492715248c613089a4d78fc7fd255) | 把 OptiScaler 和卸载 wrapper 解压到 `~/fgmod`；启动 wrapper 再把 DLL 和配置复制进目标游戏目录，见 [`main.py` L45-L62](https://github.com/xXJSONDeruloXx/Decky-Framegen/blob/291bc511329492715248c613089a4d78fc7fd255/main.py#L45-L62)、[L94-L125](https://github.com/xXJSONDeruloXx/Decky-Framegen/blob/291bc511329492715248c613089a4d78fc7fd255/main.py#L94-L125) 和 [`fgmod.sh` L63-L110](https://github.com/xXJSONDeruloXx/Decky-Framegen/blob/291bc511329492715248c613089a4d78fc7fd255/defaults/assets/fgmod.sh#L63-L110)。 | 只记录日志；见 [`main.py` L9-L15](https://github.com/xXJSONDeruloXx/Decky-Framegen/blob/291bc511329492715248c613089a4d78fc7fd255/main.py#L9-L15)。 | 没有 `_uninstall()`；只提供一个用户主动调用的方法删除 `~/fgmod`，见 [`main.py` L208-L250](https://github.com/xXJSONDeruloXx/Decky-Framegen/blob/291bc511329492715248c613089a4d78fc7fd255/main.py#L208-L250)。 | Decky 卸载不会从游戏目录移除注入的 DLL，也不会恢复备份 DLL；删除 `~/fgmod` 反而会一并移除用户之后可调用的卸载 wrapper。逐游戏清理由 [`fgmod-uninstaller.sh` L68-L104](https://github.com/xXJSONDeruloXx/Decky-Framegen/blob/291bc511329492715248c613089a4d78fc7fd255/defaults/assets/fgmod-uninstaller.sh#L68-L104) 单独完成。 |
| [DeckMTP `a27a121`](https://github.com/dafta/DeckMTP/tree/a27a12111e6a389fc4a5e764a946075e03028130) | 运行时修改 USB configfs、内核模块、`/dev/ffs-mtp` 和 `/etc/umtprd`；见 [`start.sh` L7-L61](https://github.com/dafta/DeckMTP/blob/a27a12111e6a389fc4a5e764a946075e03028130/backend/src/start.sh#L7-L61)。 | 如果 MTP 正在运行就执行完整 stop 脚本，恢复 USB driver、卸载 configfs/FunctionFS、删除 `/etc/umtprd`；见 [`main.py` L43-L85](https://github.com/dafta/DeckMTP/blob/a27a12111e6a389fc4a5e764a946075e03028130/main.py#L43-L85) 和 [`stop.sh` L3-L47](https://github.com/dafta/DeckMTP/blob/a27a12111e6a389fc4a5e764a946075e03028130/backend/src/stop.sh#L3-L47)。 | 没有 `_uninstall()`。 | 这是把纯运行态资源正确放在 `_unload()` 的样本：重载和更新会中断当前 MTP 会话，但不会混淆额外的安装状态。 |

## 从样本归纳出的模式

### 1. `_unload()` 最适合当前进程和当前会话资源

DeckMTP 和 Recorder 的行为最清晰：插件停止时结束当前硬件/录制会话并保存必要状态，但不把“后端停止”解释成“删除安装”。这与 Decky 的实际重载和禁用调用链一致。

TunnelDeck 则展示了反例：把 sysext 删除和 NetworkManager 重启放在 `_unload()`，意味着任何后端重载、禁用或 Loader 退出都会触发系统级中断。

### 2. 在 `_uninstall()` 直接清理很常见，但更新代价真实存在

SpoofDPI、DoT DNS、XR Gaming 和 Virtual Surround Sound 都立即清理外部状态。然而 Loader 更新同样调用这些钩子：

- SpoofDPI 更新会清空 auto-start；
- DoT DNS 更新会删除正在使用的系统 DNS drop-in；
- XR Gaming 更新会卸载驱动而新版本不自动重装；
- Virtual Surround Sound 的异步旧卸载可能与新安装竞态。

这说明“其他插件也直接卸载”只能证明做法存在，不能证明它对基础库或要求无缝更新的组件合适。

### 3. 用户数据和独立软件通常被保留

Recorder 保留视频，CSS Loader 保留主题和 symlink，Framegen 保留游戏目录修改，Sunshine 保留系统 Flatpak，Syncthing 甚至有意让 watchdog 脱离插件继续运行。官方 Loader 本身也声明不会替插件删除这些外部文件。

这种策略对用户产物通常正确，但对安全敏感、特权或系统集成资源可能造成孤儿：例如 setuid bwrap、副作用仍生效的 systemd 服务、Steam 代理配置或系统 drop-in。因此每个插件必须有自己的明确所有权清单，不能依赖 Loader。

## 本项目发现与修复状态

以下实机发现基于公开提交 [`3ade79a3b47385b544bd1bb8473cfd69df8624c0`](https://github.com/ccc007ccc/mango-overlay-decky/tree/3ade79a3b47385b544bd1bb8473cfd69df8624c0)。对应修复已经完成离线回归，新包的真实 Decky 卸载已经通过；正常更新与故障更新仍需继续复测。

### 1. `_uninstall()` 的五秒超时：已修复并通过实机确认

旧实现通过 `asyncio.to_thread()` 等待 `mark_pending_uninstall()`，而后者在持有生命周期锁期间同步启动 cleanup oneshot。2026-08-06 21:33 的 Decky `v3.2.8-pre1` 实机日志显示：21:33:00.604 已进入 `_uninstall()`，cleanup service 曾启动并因旧插件目录仍存在而返回 `false`，但回调始终没有记录 `verification armed`；21:33:05 Loader 对仍未退出的后端发送 SIGKILL。该反馈环明确复现了“pending 已交接但回调没有在 Loader 期限内返回”的用户症状。

当前实现让 `_uninstall()` 在事件循环线程完成小型原子 pending 落盘，随后只执行一秒上限的 `systemctl --user --no-block restart mango-overlay-cleanup.timer`。它不再同步启动或等待 cleanup service。针对性测试会让同步 cleanup service 模拟阻塞 0.5 秒，并要求 pending handoff 在 0.25 秒内返回；完整插件测试为 53 项通过。

2026-08-06 23:10 的真实 Decky 卸载复测显示：23:10:15 进入 `_uninstall()`，同一秒记录 `Mango Overlay uninstall verification armed`，Loader 报告插件在 0.1 秒内停止，没有 SIGKILL；23:10:18 finalizer 进入最终清理，约 23:10:20 已移除项目运行时、状态、用户 units、MangoApp drop-in 和三个 Decky 保留目录。系统 `gamescope-mangoapp.service` 最终只加载 `/usr/lib/systemd/user/gamescope-mangoapp.service`，没有 drop-in、失败 unit 或残留项目进程。

### 2. cleanup timer 的空闲唤醒：已修复

旧实现把 timer 与 broker socket 一起 `enable --now`，没有 pending 时也每分钟运行。当前 unit 使用 `OnActiveSec=3s`、`OnUnitActiveSec=5s` 和 `AccuracySec=500ms`；安装时只 `enable` 作为用户管理器重启后的遗留 pending 恢复保险，不立即启动。只有 `_uninstall()` 写入 pending 后才非阻塞重启 timer；替代版本激活、pending 不存在或 generation 已失效时立即停止。因此正常运行不再周期执行 cleanup service。

### 3. Loader 保留目录：已纳入精确清理

最终清理现在除项目自己的 `~/.local/share`、`~/.local/libexec`、`~/.config`、`~/.cache` 和 `~/.local/state` 外，还精确验证并删除：

```text
~/homebrew/settings/mango-overlay-decky/
~/homebrew/data/mango-overlay-decky/
~/homebrew/logs/mango-overlay-decky/
```

回归测试同时创建相邻插件数据，确认不会递归删除 `~/homebrew/settings`、`data`、`logs` 父目录或其他插件目录。

### 4. 更新目录竞态与旧记录兼容：已覆盖离线回归

pending 记录保存旧插件目录的 device/inode。finalizer 会区分原目录仍存在、同路径半解压而身份不明、同标识替代插件已经出现和插件确实缺失；扫描限制为旧路径父目录的 256 个条目，并拒绝不安全的插件根目录。没有 device/inode 字段的旧 pending 记录仍可读取：旧目录存在时保守等待，目录消失后再进行同标识扫描和最终清理。

## 对 Mango Overlay Decky 的建议

### 用户的判断是对的：最终卸载不需要保留业务数据

Mango Overlay Decky 是基础绘制库，不拥有 FPS 数据，也没有需要卸载后保留的用户创作内容。用户从 Decky 真正卸载后，应尽快：

1. 停止并移除项目自己的 broker socket/service 和测试 provider；
2. 移除项目自己的 `gamescope-mangoapp.service` drop-in；
3. 若真实 MangoApp 服务正在运行，恢复并验证系统 `/usr/bin/mangoapp`；
4. 删除项目明确拥有的 runtime、state、config、cache 和普通日志。

不必像 Recorder、CSS Loader 或 Syncthing 那样保留外部内容。

### 但“立即”应是数秒内确认后清理，不是回调内直接删

直接在 `_uninstall()` 中完成上述操作有三个问题：

1. 每次正常 Decky 更新都会先恢复系统 MangoApp、停止 broker 并删除运行时，然后新版本再全部安装，造成不必要的覆盖层中断；
2. Loader 直到删除旧插件后才验证 zip，更新包损坏时没有可靠的旧插件进程可以补救；
3. 插件进程约 5 秒后可能被 SIGKILL，复杂清理在回调中没有可靠完成保证。

建议保留当前“稳定 helper + generation/token + 原子锁”的架构，只把确认速度提高：

```text
旧插件 _uninstall()
  -> 原子写 pending-uninstall（generation、token、插件身份）
  -> 启动插件目录之外的稳定 finalizer
  -> 立即返回

finalizer
  -> 等待 Loader 删除旧插件目录
  -> 经过很短的 replacement settle window
  -> 在同一把锁内重新检查：
       - 同标识 plugin.json 是否已经出现
       - 新 _main() 是否已取消 pending token
       - generation/token 是否仍属于旧版本
  -> 有替代版本：取消清理
  -> 仍然缺失：立即执行最终卸载
```

当前 Loader 在 `uninstall_plugin()` 返回后同步解压内存中的 zip，因此成功更新时新插件目录通常会很快重新出现。实现已选用 3 秒 grace 和 5 秒重试，并通过原目录 identity、半解压目录和同标识扫描降低误清理风险；这仍不是 Decky API 保证，必须用连续更新、慢启动和损坏包进行实机压力测试后才能视为发布验收完成。

当前 finalizer 不只相信旧目录名；它会在 Decky plugins 根目录中解析有界、受验证的 `plugin.json`，按插件标识确认替代版本，以兼容未来包顶层目录名变化。清理仍需保持幂等、拒绝符号链接、只删除所有权清单中的路径，并在重启后继续完成中断的最终卸载。

### 验收条件与状态

1. **真实卸载延迟（已通过）：** 点击 Decky 卸载后，回调 0.1 秒返回，约 5 秒内恢复系统 MangoApp、停止项目服务并删除项目所属文件。
2. **正常更新：** 旧版本 `_uninstall()` 被调用，但新插件目录出现后 finalizer 不清理；新版本验证并原子激活，Steam 统计和 provider 画布保持独立。
3. **慢启动更新：** 新目录已解压但新 `_main()` 故意延迟，目录身份检查仍保护旧运行时，随后由新版本取消 pending。
4. **损坏更新：** Loader 删除旧插件后散列失败、插件保持缺失，finalizer 最终恢复系统状态且不留下孤儿服务。
5. **重载与禁用：** 只执行 `_unload()`，不产生 pending uninstall，不改变 drop-in、活动 runtime 或用户总开关。
6. **finalizer 中断：** 清理中断或设备重启后能根据 token/事务记录安全重试。

其中真实卸载延迟、重载/禁用、目录身份、旧 pending、扫描上限、精确清理和非阻塞 handoff 已通过；正常更新、慢启动更新和损坏更新仍需要真实 Decky Loader 与 SteamOS 会话证据。

## 最终判断

商店生态中，“卸载时立即清理”“保留一切外部内容”“只清理运行态”三种做法都存在，没有可直接照搬的统一惯例。Decky Loader 的确定事实只有两个：**更新会复用 `_uninstall()`，Loader 只删除插件目录。**

因此，Mango Overlay Decky 不需要保留当前一分钟级的用户可见卸载等待，但也不应退化为在 `_uninstall()` 内直接删除。将外部确认窗口缩短到数秒，既符合“基础库应立即卸载”的产品预期，也保留正常更新不会被旧版本误清理的架构保证。
