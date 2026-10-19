# ThunderDisplay

[English](README.md) | 简体中文

将 Linux 电脑作为 Mac 的局域网扩展屏幕，通过 Linux 的键盘和鼠标控制 Mac。

Mac 端创建独立的虚拟显示器并采集、编码画面；Linux 客户端负责显示和输入回传。支持普通局域网和雷电网桥，分辨率、缩放、刷新率及编码参数均在客户端设置。

## 功能

- **虚拟副屏**：扩展 Mac 桌面，自定义输出分辨率、1× / 2× 及分数缩放。
- **两种编码**：PyroWave GPU 编解码，以及 Mac VideoToolbox 硬件 H.264 编码；Linux H.264 优先使用 VAAPI 硬件解码。
- **画质与性能**：Mac 端 Lanczos 缩放、PyroWave 变化区域编码、独立光标绘制，以及数据足够时的丢包部分帧恢复。
- **键鼠控制**：快捷键捕获、跨屏控制、游戏鼠标相对移动，以及可配置的释放控制和全屏快捷键。
- **系统音频**：通过 UDP 传输 48 kHz、16 位双声道无压缩 PCM，自动使用 Linux 默认设备播放；音频数据约占 1.54 Mbps，不含包头。
- **连接与状态**：局域网发现、手动输入地址、客户端性能叠加层、Mac 菜单栏控制和登录时自动启动。

目前发送端为 macOS，接收端为 Linux，项目仍处于早期开发阶段。

## 系统要求

| 端 | 要求 |
| --- | --- |
| Mac | Apple Silicon，macOS 14 或更新版本；允许屏幕录制和辅助功能权限 |
| Linux | 支持 Vulkan 1.3 及 PyroWave 所需 subgroup / 8-bit buffer 功能的 GPU 和驱动；支持 X11 / Wayland |
| H.264 硬件解码 | Linux 安装适配 GPU 的 VAAPI 驱动；不可用时尝试软件解码 |
| 网络 | 两端 IPv4 可达，并允许双向 UDP 通信；Mac 控制与发现端口为 UDP `42420` |

两种编码模式目前都使用 Vulkan 显示路径。雷电网桥也需要先由系统配置 IP 地址，程序不负责分配地址。

## 安装与连接

预编译包请查看 [Releases](https://github.com/ChenDoXiu/MacThunderDisplay/releases)。尚无适用安装包时，可按下文从源码构建。

1. 在 Mac 上打开 `ThunderDisplay.app`，通过菜单栏面板授予屏幕录制和辅助功能权限。应用启动后自动监听连接。
2. 在 Linux 上解压客户端包，运行包内的 `./ThunderDisplay`。运行 `./install.sh` 可为当前用户安装并添加应用菜单入口，无需 root。
3. 在客户端选择发现的 Mac，或填写 Mac 的 IPv4 地址 / 主机名。
4. 设置输出分辨率、界面缩放、刷新率、编码和码率后连接。需要调整屏幕排列时，使用 Mac 的系统显示器设置。

Mac 安装包使用本地开发签名，未经过 Apple 公证；首次打开可能需要在系统设置的“隐私与安全性”中允许打开。源码构建会保留本机的开发签名身份，避免每次重建都更换身份。

Linux 预编译包仍依赖宿主的 glibc、Vulkan 和显卡驱动。首批 x86_64 包构建于 glibc 2.44 环境，旧发行版建议从源码构建；具体要求见包内 `BUILD-INFO.txt`。

### 默认快捷键

在接收窗口获得焦点时使用：

| 快捷键 | 功能 |
| --- | --- |
| `F8` | 释放 / 恢复键鼠控制 |
| `Ctrl+Shift+F11` | 切换全屏 |
| `Ctrl+Alt+Shift+S` | 显示 / 隐藏性能叠加层 |

释放控制和全屏快捷键可在客户端修改，并在独占捕获模式下保留给本地处理。叠加层可查看帧率、带宽、延迟和丢帧等统计；软件计时不包含显示器实际发光的耗时。

### 编码选择

| 编码 | 适用场景 | 特点 |
| --- | --- | --- |
| PyroWave | 雷电网桥、高带宽有线网络 | GPU 编解码，支持 4:4:4 和变化区域优化；带宽需求较高 |
| H.264 | 普通局域网 | 带宽需求较低；高频网格和彩色文字边缘可能出现压缩损失 |

“链路速率”用于发送节流，应按实际网络能力设置。视频码率也应留出网络余量。

### 分辨率与缩放

客户端的输出分辨率是**实际传输的像素尺寸**，界面缩放决定 Mac 桌面的逻辑尺寸。例如同样输出 `2240 × 1400`：

| 界面缩放 | Mac 逻辑桌面 | Mac 渲染尺寸 | 传输尺寸 |
| --- | --- | --- | --- |
| 1× | 2240 × 1400 | 2240 × 1400 | 2240 × 1400 |
| 2× | 1120 × 700 | 2240 × 1400 | 2240 × 1400 |
| 1.4× | 1600 × 1000 | 3200 × 2000 | 2240 × 1400 |

1× 使用原生渲染，2× 使用 Retina 渲染；其他倍率先按 Retina 2× 渲染，再由 Mac 使用 Lanczos 缩放到输出尺寸。实际采集尺寸与输出一致时跳过缩放。

## 从源码构建

```sh
git clone https://github.com/ChenDoXiu/MacThunderDisplay.git
cd MacThunderDisplay
```

### macOS

安装 Xcode 或 Command Line Tools、CMake 和 Python 3，然后运行：

```sh
bash scripts/build-mac.sh
open build/ThunderDisplay.app
```

脚本会编译 Metal 编码库和发送端、生成应用包并进行本地签名。重复构建时请保留 `~/Library/Application Support/ThunderDisplay/Signing` 中的签名状态；不要将其中的密钥上传到仓库。

### Linux

构建依赖：C++17 编译器、Git、CMake 3.24+、pkg-config、Python 3、Qt 6.5+（Widgets / Network）、SDL3、Vulkan 开发文件、`glslc` 和 FFmpeg 开发文件（libavcodec / libavutil）。Wayland 开发文件和 libsystemd 为可选依赖。

```sh
bash scripts/build-linux.sh
./build/linux/thunder-display-client
```

构建脚本会下载固定版本的 PyroWave 及其依赖，首次构建需要网络。PyroWave 自身的其他构建依赖由其 CMake 配置检查。

生成可分发的 Linux 包：

```sh
python3 scripts/package-linux.py
```

产物位于 `build/dist/`，包含客户端、相关动态库和安装脚本。Vulkan 加载器、显卡驱动及 glibc 仍由目标系统提供。

也可以直接运行接收器，例如使用 H.264：

```sh
./build/linux/thunder-display-receiver \
  --host 192.168.1.10 \
  --codec h264 --mbps 50 --link-mbps 1000 \
  --width 2240 --height 1400 --retina --scale 1.4 \
  --fps 60 --fullscreen
```

将示例 IP 替换为 Mac 的实际地址；其余参数见 `--help`。

## 当前限制

- 当前协议使用 UDP，尚未实现身份认证和传输加密，仅适合可信局域网。
- 自动发现依赖 IPv4 广播；跨网段或广播受限时可手动填写地址。
- H.264 当前限制为单边不超过 4096 像素、总像素不超过约 9 MP、码率不超过 200 Mbps。
- 虚拟显示器依赖 Apple 未公开的 CoreGraphics 接口，系统升级后可能需要适配。
- 刷新率上限设置不代表实际帧率保证；性能取决于分辨率、画面内容、网络和两端硬件。

## 代码结构

```text
mac/        虚拟显示器、采集、编码、输入注入和菜单栏界面
linux/      Qt 设置界面、收包、解码、Vulkan 显示和输入捕获
common/     传输协议、光标数据和显示布局
scripts/    构建、签名和 Linux 打包安装脚本
vendor/     PyroWave Metal 和 stb 等第三方源码
```

## 许可证与致谢

项目使用 [MIT 许可证](LICENSE)。GPU 编解码基于 [Themaister / PyroWave](https://github.com/Themaister/pyrowave)，性能叠加层使用 [stb_easy_font](https://github.com/nothings/stb)。第三方来源和许可说明见 [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)。

仓库保留源码、构建工具和必要许可证，构建产物通过 Releases 单独发布。
