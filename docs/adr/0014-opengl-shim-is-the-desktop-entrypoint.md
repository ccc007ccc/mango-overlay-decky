# 桌面 OpenGL 统一从 MangoHud shim 进入

日期：2026-08-06  
状态：已接受

## 背景

桌面 OpenGL 游戏通常通过 GLX/EGL 交换缓冲，而 Wine/Proton 的 Unix 图形进程
可能使用 `dlopen`/`dlsym` 获取这些函数。直接把 `libMangoHud_opengl.so` 放进
`LD_PRELOAD` 在普通原生程序上可以工作，但在 Proton OpenGL 测试中会出现库已
加载、socket 可见而交换缓冲拦截未发生的情况。

## 决策

桌面 OpenGL 的唯一注入入口是 upstream `libMangoHud_shim.so`。启动器预加载 shim，
并通过 `MANGOHUD_OPENGL_LIBS` 指向同一架构的项目 OpenGL 渲染库。shim 负责拦截
GLX/EGL 入口和延迟加载渲染库；原生 KDE、SteamRT4 与 Proton 共用这条入口。

## 结果

- 不为 Proton 或 SteamRT4 复制一套 OpenGL 覆盖层实现。
- `x86_64` 和 `i686` 仍需分别构建 shim 与 OpenGL 库。
- Runtime 启动器必须同时传播 shim、渲染库、图像私有依赖和 broker socket。
- MangoHud upstream 改变 shim 的 ABI 或拦截策略时，需重新运行原生、SteamRT4
  和 Proton 矩阵；协议与场景接口不因此改变。
