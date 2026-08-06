# SteamOS MangoApp 基线

记录日期：2026-08-06

## 版本

- SteamOS Gamescope：`3.16.23.5`
- SteamOS MangoHud：`0.8.3.rc1.r24.g33c2c7dd-3`
- MangoHud 提交：`33c2c7ddbb72c15e19a42163d75424d5804f8ec8`
- 正式构建 ABI 基线：Steam Runtime 4 `4.0.20260608.242786`，glibc `2.41`

## 可复现测试

在 `dev` 容器中构建原版 MangoApp：

```bash
distrobox enter dev -- meson setup --wipe build/baseline \
  -Dbuildtype=release \
  -Dmangoapp=true \
  -Dmangohudctl=false \
  -Dmangoplot=disabled \
  -Dtests=disabled \
  -Dwith_dbus=disabled \
  -Dwith_nvml=disabled \
  -Dwith_xnvctrl=disabled
distrobox enter dev -- meson compile -C build/baseline
```

在 KDE 中运行宿主 Gamescope，并让开发 MangoApp 与测试程序在 `dev` 容器内运行：

```bash
tools/run-nested-baseline.sh
```

宿主 Gamescope 成功创建 Xwayland、外部 MangoApp 和 Vulkan 测试表面。测试运行 25 秒无 MangoApp 重启或崩溃。

## 参考开销

同一台设备、`1280×800`、vkcube、相同 MangoHud 配置下的一次稳定运行采样：

| 组件 | CPU | RSS |
| --- | ---: | ---: |
| SteamOS `/usr/bin/mangoapp` | 约 10.2% | 约 162 MiB |
| `dev` 容器内基线构建 | 约 10.8% | 约 86 MiB |

这是嵌套合成器中的开发参考值，不是发布性能预算。后续扩展版必须使用相同命令和场景重新比较。

## KDE 桌面渲染器实验参考（不属于首版）

以下数据只保留为历史研发记录。桌面注入不进入正式包，也不属于当前 CI、发布预算或
兼容承诺。

桌面渲染器使用以下命令建立可重复参考：

```bash
tools/benchmark-desktop-renderers.sh all all
```

工具在固定 SteamRT4 和 Proton 11.0 中，对两种 ABI 的 Vulkan、GLX、EGL、Proton
OpenGL、D3D11/DXVK 和 D3D12/VKD3D 分别测量三种状态：无注入基线、已注入但没有
提供者、典型示例场景。每格在 `960×600`、垂直同步限制下预热 1.5 秒并采样 2 秒；
`perf task-clock` 只统计测试窗口进程，后两种状态另计 broker，典型场景再计 provider。
RSS 是这些进程采样期间的最大合计值。工具会先像素确认典型场景确实可见。

本次单样本中，典型场景相对无注入基线的增量为：

| ABI | 路径 | CPU 增量（百分点） | RSS 增量 |
| --- | --- | ---: | ---: |
| x86_64 | Vulkan | +3.57 | +22.50 MiB |
| x86_64 | GLX | +2.10 | +35.11 MiB |
| x86_64 | EGL | +2.81 | +34.83 MiB |
| x86_64 | Proton OpenGL | +1.29 | +29.25 MiB |
| x86_64 | Proton D3D11/DXVK | +5.40 | +25.81 MiB |
| x86_64 | Proton D3D12/VKD3D | +4.40 | +29.52 MiB |
| i686 | Vulkan | +3.63 | +22.90 MiB |
| i686 | GLX | +2.12 | +33.28 MiB |
| i686 | EGL | +3.95 | +32.66 MiB |
| i686 | Proton OpenGL | +1.80 | +31.13 MiB |
| i686 | Proton D3D11/DXVK | +3.98 | +22.73 MiB |
| i686 | Proton D3D12/VKD3D | +4.55 | +26.38 MiB |

完整原始值写入 `build/desktop-performance/latest.csv`。这组数字用于发现后续回归，
不能跨路径比较绝对 CPU，也不是发布预算：各 fixture 的帧率节奏不同，2 秒单样本中
小于约 0.5 个百分点的差异可能只是调度噪声。发布预算仍需在真实游戏和固定功耗、
刷新率条件下重复采样。

## 构建边界

滚动版 Arch `dev` 容器当前使用 glibc `2.44`，其二进制不能安装到 glibc `2.41` 的 SteamOS。容器产物只用于开发测试；可安装运行时必须在上述 Steam Runtime 4 基线中构建。
