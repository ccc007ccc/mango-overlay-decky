# 提供者 SDK

当前开发接口包含 C ABI、C++、Python 和 Rust。四套接口都连接同一个 `$XDG_RUNTIME_DIR/mango-overlay-decky.sock`，共享协议 `1.0`、资源 ID 和原子场景事务语义。

## 能力

- 注册应用与实例身份，声明参考画布和显示策略；当前输出及其切换由渲染器处理。
- 创建分组、文字、矩形、线、折线、圆、图片和 GIF。
- 使用锚点、父子变换、透明度和矩形裁剪。
- 上传 PNG、JPEG、WebP 或 GIF；小文件内联，大文件自动使用 sealed memfd。
- 一次提交多个元素；失败或未提交时继续显示上一份完整场景。
- Rust `Transaction` 未提交或提交失败时自动回滚，同一 `Provider` 可继续使用。

公开入口：

- C：[client.h](../client/include/mango_overlay/client.h)
- C++：[client.hpp](../client/include/mango_overlay/client.hpp)
- Python：[mango_overlay](../client/python/mango_overlay/__init__.py)
- Rust：[mango-overlay](../client/rust/src/lib.rs)

完整调用示例见 [provider_demo.cpp](../tools/provider_demo.cpp) 和各语言进程测试。SDK 由提供者随自己的程序构建或打包，不要求 Decky 把开发头文件或语言包安装到系统目录；Decky 运行时中的客户端库只服务于包内测试提供者。

## 验证

在 `dev` 容器中构建后，运行四套真实 broker 测试：

```bash
distrobox enter dev -- meson test -C build/overlay \
  'mango overlay provider client process' \
  'mango overlay C++ provider client' \
  'mango overlay Python provider client' \
  'mango overlay Rust provider client' \
  --print-errorlogs
```

嵌套 Gamescope 的端到端绘制测试：

```bash
tools/run-nested.sh
```

该脚本只运行工作区构建产物，不替换宿主 `/usr/bin/mangoapp`。

正式包验收会从 Decky zip 解出运行时，再让四套 SDK 连接包内 broker；Rust 在该步骤中由 Cargo 重新编译：

```bash
distrobox enter dev -- python3 packaging/verify-packaged-sdk.py \
  --archive build/package/mango-overlay-decky-0.1.0.zip \
  --source-root . \
  --development-build build/overlay
```
