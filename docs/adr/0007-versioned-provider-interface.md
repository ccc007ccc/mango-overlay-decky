# 提供者接口独立于 ImGui 和渲染后端

绘制提供者通过版本化 wire protocol 提交画布、场景事务和资源 ID，不直接调用 ImGui，也不传递 Vulkan/OpenGL 纹理对象。项目首版以稳定 C ABI、C++ 封装和 Python 封装承载该协议，使游戏模式 MangoApp 与后续桌面 Vulkan/OpenGL 渲染器能够共享同一调用方接口。

