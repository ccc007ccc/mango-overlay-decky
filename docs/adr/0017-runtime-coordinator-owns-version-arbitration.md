---
status: accepted
---

# 共享核心仲裁由绘制平台拥有

当多个 Decky 产品各自内置 Mango Overlay 时，候选注册、版本选择、原子切换、回滚和最终卸载统一由 Mango Overlay Decky 的 Runtime Coordinator 实现，产品插件只维护自己的持久运行时声明。协调器先按 Mango 核心语义版本选择最大候选，只验证实际要激活的这一份；一百个候选的正常路径只能比较有界元数据，不能逐个运行二进制自检。若最大候选实际验证或启动失败，隔离该精确内容修订并降序回退；同版本不同内容视为冲突，任何旧插件后启动都不得把活动核心降级。

实现合同：

- 声明、活动指针、失败修订和有界验证缓存写入
  `~/.local/share/mango-overlay-decky/coordinator/state.json`，所有写入通过私有锁和临时文件原子替换。
- 产品运行时先复制到 `runtime/versions/<content_revision>`，`runtime_ref` 只能是受限的不透明标识；生命周期 adapter 负责结构校验、单次自检和 systemd/MangoApp 切换。
- `launcher.py coordinator status/register/pending-remove/finalize/retry` 是稳定的最小 JSON 命令面；未知 schema 只读失败，不继续写入。
- `_uninstall` 只把 claim 标记为 pending；最终确认时若仍有其他 claim，只切换活动核心并保留共享文件，最后一个 claim 才恢复系统 MangoApp 并执行清理。
- 同一产品以相同内容修订重新注册时只更新其 generation，不重新验证或重启运行时；非活动产品的卸载记录由协调器 token 关联，不能因为全局 lifecycle generation 不同而被拒绝。
