# 提供者协议

状态：协议 `1.0` 的提供者、渲染器和控制器角色均已实现。

## 目标

协议让第三方程序描述自己的保留式画布，而不暴露 ImGui、Vulkan、OpenGL 或 MangoApp 内部对象。同一提供者实现应同时适用于游戏模式和后续桌面游戏渲染器。

## 角色

一个连接在握手时选择一种角色：

- `provider`：注册身份，提交场景事务和绘制资源。
- `renderer`：订阅已授权场景，接收完整快照与有序变化。
- `controller`：读取平台状态，并修改总开关、审批策略、应用授权、可见性和层级。

角色在单个连接生命周期中不改变。Unix peer credentials 必须属于当前登录用户；角色权限由场景代理再次限制。

角色限制用于接口最小化和防误用，不是同一 UID 内的强安全沙箱。`controller` 使用独立消息集合，场景代理不会让 `provider` 连接意外调用管理操作。

## 传输

- Endpoint：`$XDG_RUNTIME_DIR/mango-overlay-decky.sock`
- Socket：Unix `SOCK_SEQPACKET`
- 消息：固定二进制头 + FlatBuffers 载荷
- 大资源：`SCM_RIGHTS` + 只读文件描述符
- 字节序：实现 schema 时固定，不依赖宿主默认值
- 队列：每连接有界，不允许无限缓冲

固定消息头至少携带：

- magic
- protocol major/minor
- message type
- flags
- request ID
- payload length

## 握手与能力

客户端先发送 `Hello`：

```text
role
protocol range
required capabilities
optional capabilities
client version
```

场景代理返回双方共同支持的版本和能力。缺少必需能力时立即拒绝，不允许客户端猜测降级结果。

## 提供者身份

提供者注册：

```text
application_id   稳定、由应用声明
instance_id      每次运行唯一
display_name     仅用于用户界面
requested_visibility
virtual_canvas
```

授权、全局层级和用户覆盖设置按 `application_id` 保存；实际场景按 `(application_id, instance_id)` 隔离。

“要求新提供者审批”默认关闭：

- 关闭：未知应用通过身份和配额检查后立即发布画布。
- 开启：未知应用保持 `pending`，用户批准前渲染器看不到其画布。
- 无论开关状态如何，用户都能撤销、隐藏或重新排序应用。

`application_id` 是自声明身份。审批策略不能防御当前用户下故意冒充其他应用的恶意进程；它只管理正常合作程序的显示权限。

## 参考画布与输出视口

每个提供者实例声明一个参考画布：

```text
width       默认 1280
height      默认 800
visibility  默认 game_only
```

参考宽高只确定逻辑坐标密度，不代表用户屏幕分辨率。渲染器每帧读取当前输出，按 `min(output_width / reference_width, output_height / reference_height)` 等比缩放，并将较宽或较高方向扩展为当前输出视口。根元素的九个锚点依附输出视口边缘，因此切换内屏、外接屏或输出分辨率时不需要提供者重新注册，也不需要用户手工填写分辨率。

元素不会被非等比拉伸，最终统一裁剪到当前输出。协议当前不向提供者回报输出尺寸或安全区；用户视觉缩放若后续加入，应作为平台策略独立于参考画布和显示模式。

## 场景节点

所有节点共有：

```text
element_id
parent_id
z_index
rotation
scale
opacity
clip
hidden
```

位置和尺寸属于需要它们的具体节点。

第一版节点类型：

- `Group`
- `Text`
- `Line`
- `Polyline`
- `Rectangle`
- `Circle`
- `Image`
- `Gif`

`Rectangle` 的 `corner_radius` 大于零时即为圆角矩形。

`Group` 的变换、透明度和裁剪由子节点继承。循环父子关系、超过最大深度或引用不存在父节点的事务整体失败。

## 场景事务

提供者通过小接口修改场景：

```text
begin_transaction
upsert_element
delete_element
commit_transaction
abort_transaction
```

规则：

- 一个实例同一时间最多有一个未提交事务。
- `commit` 前，渲染器继续显示上一个完整场景。
- 所有元素和资源引用验证通过后，事务才获得单调递增序号并原子发布。
- 任一操作失败会拒绝整个事务，不留下部分修改。
- 连接中断等价于放弃未提交事务。
- 高频更新可以覆盖尚未发送的旧变化，但不能跨越已提交事务重新排序。

## 绘制资源

资源接口：

```text
upload_resource
release_resource
```

- 资源 ID 由提供者在自己的实例内分配；平台验证 ID、格式、引用关系和配额。
- 小资源可以内联；大资源通过 `SCM_RIGHTS` 传递只读普通文件或完全 sealed 的 memfd。
- 不接受宿主文件路径或 GPU 对象。
- 静态图片支持 PNG、JPEG、WebP。
- 动画第一版支持 GIF。
- GIF 在场景代理连接工作线程中验证和解码；渲染器负责后端私有纹理上传。
- 提供者控制播放、暂停、速度和暂停帧；GIF 默认循环，不逐帧发送图片。
- 资源由提供者实例引用；无引用资源可回收，磁盘缓存只是可再生成优化。

## 渲染器同步

渲染器连接后接收：

1. 完整场景快照及其序号。
2. 后续按序提交的场景变化。
3. 场景引用的资源以及提供者移除事件。

检测到序号缺口时，渲染器请求新快照，不自行拼接不完整状态。渲染线程只消费已经构造完成的本地快照。

## 显示与层级

- 提供者可以请求 `game_only`、`steam_only` 或 `always`。
- 项目总开关和用户对单个应用的设置拥有最终决定权。
- Steam 原生性能统计开关不控制第三方画布。
- 系统统计内容默认位于提供者画布上方。
- 用户可以调整全局层级；提供者只能设置自己画布内部的 `z_index`。

## 背压与错误

- 每个提供者有元素、事务、场景大小、资源数量和资源内存配额；资源另有全局累计预算。
- 每个连接串行处理有界 `SOCK_SEQPACKET` 消息，代理最多接受 64 个并发连接，不建立无界应用队列。
- 格式错误、未知 ID、越界引用和资源验证失败都只影响对应请求。
- request ID 用于关联请求与响应；场景事务 ID 额外提供重复提交幂等性。
- 协议错误达到阈值后断开该连接，不影响其他提供者。
- 其他 UID 的连接在握手前拒绝。

## 首版调用库

稳定 C ABI 暴露连接、身份注册、资源和场景事务。C++ 封装提供类型安全接口，Python 封装提供上下文管理事务，Rust 封装提供自动回滚的 RAII 事务。

四套接口共用同一个 C ABI 和 wire protocol；TypeScript 等其他语言可先通过 C ABI 或直接实现协议接入。

## 第一版明确不做

- 输入事件、按钮或交互控件。
- 提供者代码或 ImGui 命令注入。
- 视频、SVG、自定义着色器和自定义字体上传。
- 网络连接或跨用户访问。
- 把实时场景持久化为安装状态。
