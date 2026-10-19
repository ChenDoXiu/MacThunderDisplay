# ThunderDisplay

English | [简体中文](README.zh-CN.md)

Use a Linux computer as an extended display for your Mac over a local network, with keyboard and mouse control from Linux.

The Mac sender creates a virtual display, captures its contents, and encodes the video. The Linux client displays the stream and sends input back to the Mac. Both ordinary LAN connections and Thunderbolt networking are supported. Resolution, scaling, refresh rate, and encoding settings are configured on the client.

## Features

- **Virtual extended display:** Extend your Mac desktop with a custom output resolution, 1× / 2× scaling, and fractional scaling.
- **Two codecs:** PyroWave GPU encoding and decoding, or hardware H.264 encoding through macOS VideoToolbox. The Linux H.264 decoder prefers VAAPI hardware acceleration.
- **Image quality and performance:** Lanczos scaling on the Mac, dirty-region encoding for PyroWave, separate cursor rendering, and partial-frame recovery under packet loss when enough data is available.
- **Keyboard and mouse control:** Keyboard shortcut capture, movement across display boundaries, relative mouse input for games, and configurable release-control and fullscreen shortcuts.
- **System audio:** Uncompressed 48 kHz, 16-bit stereo PCM over UDP, played through the Linux default output device. Uses Core Audio Process Tap on supported macOS 14.2+ configurations, with ScreenCaptureKit fallback. Audio uses about 1.54 Mbps before packet overhead.
- **Connection and status:** LAN discovery, manual address entry, a client performance overlay, a Mac menu bar interface, and launch at login.

The sender currently runs on macOS and the receiver runs on Linux. The project is in early development.

## Requirements

| Component | Requirements |
| --- | --- |
| Mac | Apple Silicon, macOS 14 or later, with Screen Recording and Accessibility permissions |
| Linux | A GPU and driver supporting Vulkan 1.3 and the subgroup / 8-bit buffer features required by PyroWave; X11 or Wayland |
| Hardware H.264 decoding | A VAAPI driver for the Linux GPU; software decoding is attempted if hardware decoding is unavailable |
| Network | IPv4 connectivity and bidirectional UDP traffic; the Mac uses UDP port `42420` for control and discovery |

Both codecs currently use the Vulkan presentation path. Thunderbolt networking also requires IP addresses to be configured by the operating systems; the application does not assign addresses.

## Installation and connection

Check [Releases](https://github.com/ChenDoXiu/MacThunderDisplay/releases) for prebuilt packages. If no suitable package is available, build from source using the instructions below.

1. Open `ThunderDisplay.app` on the Mac. Use its menu bar panel to grant Screen Recording and Accessibility permissions. The application starts listening automatically.
2. Extract the Linux client package and run `./ThunderDisplay` inside it. Run `./install.sh` to install it for the current user and add an application menu entry; root access is not required.
3. Select a discovered Mac in the client, or enter its IPv4 address / hostname manually.
4. Choose the output resolution, interface scale, refresh rate, codec, and bitrate, then connect. Use the Mac's system display settings to arrange the displays.

The Mac package uses a local development signature and is not notarized by Apple. On first launch, you may need to allow it in System Settings → Privacy & Security. Source builds retain the local signing identity across rebuilds.

The Linux package still depends on the host's glibc, Vulkan loader, and GPU drivers. The initial x86_64 package was built with glibc 2.44; build from source on older distributions. See `BUILD-INFO.txt` inside the package for its runtime requirements.

### Default shortcuts

These shortcuts work while the receiver window has focus:

| Shortcut | Action |
| --- | --- |
| `F8` | Release / resume keyboard and mouse control |
| `Ctrl+Shift+F11` | Toggle fullscreen |
| `Ctrl+Alt+Shift+S` | Show / hide the performance overlay |

The release-control and fullscreen shortcuts can be changed in the client. They remain local even during exclusive input capture. The overlay shows frame rate, bandwidth, latency, dropped frames, and other statistics. Software timing does not include the time until the physical display emits light.

The Audio page lets you disable audio, choose a playback latency target, and mute local Mac playback during capture. Apply changes with **Apply and reconnect**. Automatic buffering targets 16 ms with Process Tap or 55 ms with ScreenCaptureKit; sound-device latency is additional. Local Mac muting requires Process Tap and is released when capture stops. Linux prefers native PipeWire playback when built with PipeWire 1.4+, with SDL playback as a fallback. Manual buffer targets of 8 ms or less request 64-frame Process Tap capture, UDP packets and native PipeWire playback. Other targets use 128 frames; unsupported devices and older senders fall back to compatible periods. The overlay adds a driver-reported capture-to-output estimate to the overlay; this is not an acoustic measurement.

### Choosing a codec

| Codec | Use case | Characteristics |
| --- | --- | --- |
| PyroWave | Thunderbolt networking and high-bandwidth wired networks | GPU encoding and decoding, 4:4:4 support, and dirty-region optimizations; higher bandwidth requirements |
| H.264 | Ordinary LAN connections | Lower bandwidth requirements; fine patterns and colored text edges may show compression artifacts |

The link-speed setting controls send pacing and should match the actual network capacity. Leave some network headroom when choosing the video bitrate.

### Resolution and scaling

The client's output resolution is the **pixel size transmitted over the network**. Interface scaling determines the logical size of the Mac desktop. For the same `2240 × 1400` output:

| Interface scale | Mac logical desktop | Mac render size | Transmitted size |
| --- | --- | --- | --- |
| 1× | 2240 × 1400 | 2240 × 1400 | 2240 × 1400 |
| 2× | 1120 × 700 | 2240 × 1400 | 2240 × 1400 |
| 1.4× | 1600 × 1000 | 3200 × 2000 | 2240 × 1400 |

At 1×, the Mac renders at native resolution. At 2×, it uses Retina rendering. Other scale factors use Retina 2× rendering followed by Lanczos scaling on the Mac to reach the output size. Scaling is skipped when the captured size already matches the output.

## Building from source

```sh
git clone https://github.com/ChenDoXiu/MacThunderDisplay.git
cd MacThunderDisplay
```

### macOS

Install Xcode or Command Line Tools, CMake, and Python 3, then run:

```sh
bash scripts/build-mac.sh
open build/ThunderDisplay.app
```

The script builds the Metal codec library and sender, creates the app bundle, and signs it locally. Keep the signing state in `~/Library/Application Support/ThunderDisplay/Signing` across rebuilds. Do not commit its private keys to the repository.

### Linux

Build dependencies: a C++17 compiler, Git, CMake 3.24+, pkg-config, Python 3, Qt 6.5+ (Widgets / Network), SDL3, Vulkan development files, `glslc`, and FFmpeg development files (libavcodec / libavutil). Wayland development files, libsystemd, and PipeWire 1.4+ development files are optional. A build with native PipeWire enabled also requires the corresponding PipeWire runtime library.

```sh
bash scripts/build-linux.sh
./build/linux/thunder-display-client
```

The build script downloads a pinned PyroWave revision and its dependencies, so the first build requires network access. PyroWave's CMake configuration checks its additional build dependencies.

To create a distributable Linux package:

```sh
python3 scripts/package-linux.py
```

The archive is written to `build/dist/` and includes the client, dependency libraries, and installation scripts. The target system must still provide the Vulkan loader, GPU drivers, and glibc.

You can also run the receiver directly, for example with H.264:

```sh
./build/linux/thunder-display-receiver \
  --host 192.168.1.10 \
  --codec h264 --mbps 50 --link-mbps 1000 \
  --width 2240 --height 1400 --retina --scale 1.4 \
  --fps 60 --fullscreen
```

Replace the example IP address with your Mac's address. Run with `--help` for additional options.

## Current limitations

- The protocol uses UDP without authentication or transport encryption. Use it only on trusted local networks.
- Automatic discovery uses IPv4 broadcasts. Enter an address manually when connecting across subnets or when broadcasts are restricted.
- H.264 is currently limited to 4096 pixels per axis, approximately 9 MP in total, and 200 Mbps.
- Virtual displays rely on undocumented Apple CoreGraphics interfaces and may require changes after macOS updates.
- The configured refresh rate is a target, not a guaranteed frame rate. Performance depends on resolution, screen content, network conditions, and hardware on both ends.

## Source layout

```text
mac/        Virtual display, capture, encoding, input injection, and menu bar UI
linux/      Qt settings UI, packet reception, decoding, Vulkan display, and input capture
common/     Transport protocol, cursor data, and display layout
scripts/    Build, signing, and Linux packaging / installation scripts
vendor/     Third-party sources, including PyroWave Metal and stb
```

## License and acknowledgments

ThunderDisplay is licensed under the [MIT License](LICENSE). GPU encoding and decoding are based on [Themaister / PyroWave](https://github.com/Themaister/pyrowave). The performance overlay uses [stb_easy_font](https://github.com/nothings/stb). See [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) for third-party sources and license information.

This repository contains source code, build tools, and required license files. Build artifacts are distributed separately through Releases.
