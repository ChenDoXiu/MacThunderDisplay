#include "Receiver.hpp"
#include <iostream>
#include <string>
#include <cmath>

int main(int argc, char **argv) {
    try {
        std::string host;
        bool fullscreen = false, settingsStdin = false;
        DisplayRequest request;
        KeyboardOptions keyboard;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--codec") {
                auto name = value();
                if (name != "h264" && name != "pyrowave")
                    throw std::runtime_error("Codec must be pyrowave or h264");
                request.h264 = name == "h264";
            } else if (arg == "--host")
                host = value();
            else if (arg == "--width")
                request.width = std::stoi(value());
            else if (arg == "--height")
                request.height = std::stoi(value());
            else if (arg == "--scale")
                request.scale = std::stod(value());
            else if (arg == "--fps")
                request.fps = std::stoi(value());
            else if (arg == "--mbps")
                request.mbps = std::stoi(value());
            else if (arg == "--link-mbps")
                request.linkMbps = std::stoi(value());
            else if (arg == "--settings-stdin")
                settingsStdin = true;
            else if (arg == "--retina")
                request.retina = 1;
            else if (arg == "--no-retina")
                request.retina = 0;
            else if (arg == "--420")
                request.chroma = 0;
            else if (arg == "--444")
                request.chroma = 1;
            else if (arg == "--fullscreen")
                fullscreen = true;
            else if (arg == "--release-shortcut")
                keyboard.releaseShortcut = value();
            else if (arg == "--fullscreen-shortcut")
                keyboard.fullscreenShortcut = value();
            else if (arg == "--local-shortcuts")
                keyboard.exclusive = false;
            else if (arg == "--game-mouse")
                keyboard.gameMouse = true;
            else if (arg == "--start-released")
                keyboard.startReleased = true;
            else if (arg == "--input-diagnostics")
                keyboard.diagnostics = true;
            else if (arg == "--help") {
                std::cout
                    << "thunder-display-receiver --host MAC_IPV4 [--width 2240 --height 1400 --retina --scale 1.4 --fps 60 --mbps 400 --444] [--codec pyrowave|h264] [--link-mbps 1000] [--settings-stdin] [--fullscreen] [--release-shortcut F8] [--fullscreen-shortcut Ctrl+Shift+F11] [--local-shortcuts] [--game-mouse] [--start-released] [--input-diagnostics]\nDisplay configuration is sent to the Mac before streaming starts.\nExclusive keyboard capture is enabled by default. Release and fullscreen shortcuts always stay local while the receiver is focused.\n";
                return 0;
            } else
                throw std::runtime_error("Unknown argument: " + arg);
        }
        if (host.empty())
            throw std::runtime_error("Specify the Mac Thunderbolt IPv4 address using --host");
        if (request.width < 128 || request.height < 128 || request.width > 8192 ||
            request.height > 8192 || (request.width & 1) || (request.height & 1) ||
            int64_t(request.width) * request.height > 33554432 || !std::isfinite(request.scale) ||
            request.scale < 0.5 || request.scale > 4 || request.fps < 10 || request.fps > 144 ||
            request.mbps < 20 || request.mbps > 3000 || request.linkMbps < 50 ||
            request.linkMbps > 40000)
            throw std::runtime_error("Invalid resolution, scaling, FPS or bitrate");
        if (request.h264 &&
            (request.width > 4096 || request.height > 4096 ||
             int64_t(request.width) * request.height > 9437184 || request.mbps > 200))
            throw std::runtime_error("H264 supports up to 4096 per axis, 9 MP and 200 Mbps");
        return runReceiver(host, fullscreen, request, keyboard, settingsStdin);
    } catch (const std::exception &e) {
        std::cerr << "ThunderDisplay: " << e.what() << '\n';
        return 1;
    }
}
