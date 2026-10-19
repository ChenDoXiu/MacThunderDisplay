#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "Receiver.hpp"
#include "VulkanView.hpp"
#include "Transport.hpp"
#include "ClockSync.hpp"
#include "UdpBatch.hpp"
#include "UdpPlacement.hpp"
#include <iomanip>
#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <condition_variable>
#include <exception>
#include <csignal>
#include <deque>

namespace {
volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int) {
    stopRequested = 1;
}

struct Frame {
    td::Header header;
    uint64_t firstNs = 0;
    size_t bytes = 0, completed = 0;
    uint64_t packets = 0, lastNs = 0;

    struct Unit {
        std::vector<uint8_t> data;
        std::vector<bool> received;
        size_t count = 0;
        bool done = false;
    };

    std::vector<Unit> units;
};

struct ReadyFrame {
    int width, height, chroma;
    uint64_t id, firstNs, readyNs, generation, session, captureNs;
    bool timed = false, callbackTime = false, h264 = false, key = false, partial = false;
    std::vector<std::vector<uint8_t>> units;
};

class Client {
  public:
    int socket = -1;
    std::vector<int> videoSockets;
    sockaddr_in server{};
    std::string hello;
    bool reportedDisconnected = false;
    std::atomic<bool> running{true}, connected{false}, disconnectedByServer{false};
    std::atomic<int> captureWait{0};
    ClockSync clock;
    std::atomic<bool> statsVisible{false};
    std::atomic<double> displayFPS{0}, displayMbps{0};
    std::atomic<uint64_t> generation{0}, ack{0}, receivedBytes{0}, receivedFrames{0},
        expiredFrames{0}, presentedID{0};
    std::atomic<uint64_t> assemblyDrops{0}, queueDrops{0}, displayDrops{0}, incompleteFrames{0},
        refreshRequests{0};
    std::atomic<uint64_t> socketDrops{0};
    std::atomic<uint64_t> partialFrames{0}, partialRejected{0};
    std::atomic<bool> pyrowaveRecovery{false};
    std::atomic<int> chroma{1};
    std::atomic<bool> h264{false}, h264Recovery{false};
    const bool requestedH264;
    int videoLanes = 0;
    std::atomic<int> width{2240}, height{1400}, logicalWidth{1600}, logicalHeight{1000}, fps{60},
        edges{0};
    std::atomic<uint64_t> cursorSequence{0};
    std::atomic<float> cursorX{0.5f}, cursorY{0.5f};
    std::atomic<bool> cursorVisible{true};
    std::atomic<bool> remoteCursorDrawn{true};
    std::atomic<bool> separateCursor{false};
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<ReadyFrame> latest;
    std::deque<std::unique_ptr<ReadyFrame>> videoQueue;
    td::CursorState overlay;
    uint64_t cursorRevision = 0, predictedPointer = 0, predictedNs = 0;

    struct RelativePrediction {
        uint64_t seq;
        double x, y;
    };

    std::deque<RelativePrediction> relativePredictions;
    std::atomic<uint64_t> inputProbe{0};

    explicit Client(const std::string &host, const DisplayRequest &request)
        : requestedH264(request.h264) {
        std::ostringstream config;
        config << "HELLO " << request.width << ' ' << request.height << ' ' << request.retina << ' '
               << request.scale << ' ' << request.fps << ' ' << request.mbps << ' '
               << request.chroma << " CURSOR1 LINK " << request.linkMbps;
        hello = config.str();
        addrinfo hints{}, *list = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host.c_str(), "42420", &hints, &list) != 0)
            throw std::runtime_error("Cannot resolve Mac host");
        memcpy(&server, list->ai_addr, sizeof(server));
        freeaddrinfo(list);
        socket = td::bindUdp();
        videoSockets.push_back(socket);
        // Prefer one shared 32 MiB queue. If the system clamps that request,
        // retain the multi-queue fallback without changing any global sysctl.
        int singleRequest = 16 * 1024 * 1024, singleActual = 0;
        socklen_t singleSize = sizeof(singleActual);
        if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &singleRequest, sizeof(singleRequest)) != 0)
            std::cerr << "[UDP] single receive buffer request failed: " << strerror(errno) << '\n';
        if (getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &singleActual, &singleSize) != 0)
            std::cerr << "[UDP] receive buffer query failed: " << strerror(errno) << '\n';
        if (singleActual < 32 * 1024 * 1024) {
            std::cerr << "[UDP] single queue below target: actual=" << singleActual
                      << "; trying four queues\n";
            try {
                for (int i = 1; i < 4; i++)
                    videoSockets.push_back(td::bindUdp());
            } catch (const std::exception &e) {
                std::cerr << "[UDP] extra video sockets unavailable: " << e.what() << '\n';
                for (size_t i = 1; i < videoSockets.size(); i++)
                    td::closeSocket(videoSockets[i]);
                videoSockets.resize(1);
            }
        }
        int receiveBytes = 16 * 1024 * 1024 / int(videoSockets.size());
        hello += " PORTS " + std::to_string(videoSockets.size());
        for (int fd : videoSockets) {
            if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receiveBytes, sizeof(receiveBytes)) != 0)
                std::cerr << "[UDP] receive buffer request failed: " << strerror(errno) << '\n';
            hello += ' ' + std::to_string(td::socketPort(fd));
        }
        hello += std::string(" CODEC ") + (request.h264 ? "h264" : "pyrowave");
        td::setSocketTimeout(socket, 2);
        thread = std::thread([this] {
            receive();
        });
    }

    ~Client() {
        running = false;
        if (thread.joinable())
            thread.join();
        send("BYE");
        for (int fd : videoSockets)
            td::closeSocket(fd);
    }

    void send(const std::string &text) {
        ::sendto(socket, text.data(), text.size(), 0, reinterpret_cast<const sockaddr *>(&server),
                 sizeof(server));
    }

    std::unique_ptr<ReadyFrame> waitTake(const std::atomic<bool> &active, uint64_t &revision,
                                         td::CursorState &cursor, uint64_t nextRedraw, int wakeMs) {
        std::unique_lock<std::mutex> lock(mutex);
        auto available = [&] {
            return latest || !videoQueue.empty() || cursorRevision != revision || !active;
        };
        if (wakeMs)
            ready.wait_for(lock, std::chrono::milliseconds(wakeMs), available);
        else
            ready.wait(lock, available);
        if (!active || !available())
            return {};
        uint64_t now = td::nowNs();
        if (!latest && videoQueue.empty() && now < nextRedraw)
            ready.wait_for(lock, std::chrono::nanoseconds(nextRedraw - now), [&] {
                return latest || !videoQueue.empty() || !active;
            });
        if (!active)
            return {};
        revision = cursorRevision;
        cursor = overlay;
        if (!videoQueue.empty()) {
            auto frame = std::move(videoQueue.front());
            videoQueue.pop_front();
            return frame;
        }
        return std::move(latest);
    }

    void predictCursor(uint64_t seq, float x, float y) {
        if (!separateCursor)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        predictedPointer = seq;
        predictedNs = td::nowNs();
        overlay.x = x;
        overlay.y = y;
        overlay.visible = true;
        cursorRevision++;
        ready.notify_one();
    }

    void clearRelativePrediction() {
        std::lock_guard<std::mutex> lock(mutex);
        relativePredictions.clear();
        predictedPointer = predictedNs = 0;
    }

    void predictRelativeCursor(uint64_t seq, double dx, double dy) {
        if (!separateCursor || !remoteCursorDrawn)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (!remoteCursorDrawn)
            return;
        dx /= std::max(1, logicalWidth.load() - 1);
        dy /= std::max(1, logicalHeight.load() - 1);
        if (relativePredictions.size() >= 256)
            relativePredictions.pop_front();
        relativePredictions.push_back({seq, dx, dy});
        predictedPointer = seq;
        predictedNs = td::nowNs();
        overlay.x = std::clamp(double(overlay.x) + dx, 0.0, 1.0);
        overlay.y = std::clamp(double(overlay.y) + dy, 0.0, 1.0);
        overlay.visible = true;
        cursorRevision++;
        ready.notify_one();
    }

    void redraw() {
        std::lock_guard<std::mutex> lock(mutex);
        cursorRevision++;
        ready.notify_one();
    }

    void receive() {
        pthread_setname_np(pthread_self(), "td-parse");
        UdpBatch receiver(videoSockets, server);
        int receiveBuffer = 0;
        for (int fd : videoSockets) {
            int bytes = 0;
            socklen_t optionSize = sizeof(bytes);
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, &optionSize);
            receiveBuffer += bytes;
        }
        std::cerr << "[UDP] recvmmsg batch=64 sockets=" << videoSockets.size()
                  << " total_rcvbuf=" << receiveBuffer
                  << " overflow_reporting=" << receiver.dropReporting << '\n';
        if (receiveBuffer < 32 * 1024 * 1024)
            std::cerr
                << "[UDP] total receive buffer below target: requested_accounting=33554432 actual="
                << receiveBuffer << '\n';
        uint64_t lastReceiveStats = td::nowNs(), lastCalls = 0, lastDataCalls = 0,
                 lastEmptyCalls = 0, lastPackets = 0, lastDrops = 0;
        std::map<uint64_t, Frame> frames;
        uint64_t session = 0, delivered = 0, lastHello = 0, lastSync = 0, lastPacket = td::nowNs(),
                 lastComplete = lastPacket, lastRefresh = 0;
        bool needsRefresh = false;
        Frame::Unit cursorUnit;
        td::Header cursorHeader;
        uint64_t assemblingCursor = 0;
        uint64_t lastDropReport = 0, dropReports = 0, invalidHeaders = 0, metadataMismatch = 0;
        auto reportIncomplete = [&](const Frame &f, const char *reason) {
            uint64_t now = td::nowNs();
            if (dropReports >= 12 && now - lastDropReport < 1000000000)
                return;
            ++dropReports;
            lastDropReport = now;
            std::ostringstream missing;
            size_t absent = 0, partial = 0, listed = 0;
            for (size_t i = 0; i < f.units.size(); i++)
                if (!f.units[i].done) {
                    auto &u = f.units[i];
                    if (u.data.empty())
                        absent++;
                    else
                        partial++;
                    if (listed++ < 16)
                        missing << i << ':' << u.count << '/' << u.received.size() << ',';
                }
            std::cerr << "[FrameDrop] reason=" << reason << " frame=" << f.header.frame
                      << " units=" << f.completed << '/' << f.units.size() << " absent=" << absent
                      << " partial=" << partial << " datagrams=" << f.packets
                      << " age_ms=" << (now - f.firstNs) / 1e6
                      << " idle_ms=" << (now - f.lastNs) / 1e6 << " missing=" << missing.str()
                      << '\n';
        };

        while (running) {
            uint64_t now = td::nowNs();
            if (now - lastHello > 500000000) {
                send(hello);
                lastHello = now;
            }
            if (session && connected && now - lastSync > 500000000) {
                lastSync = td::nowNs();
                send("SYNC " + std::to_string(session) + " " + std::to_string(lastSync));
            }
            bool completedVideo = false;
            uint8_t *buffer = nullptr;
            uint64_t arrival = 0;
            sockaddr_in peer{};
            ssize_t n = receiver.next(buffer, peer, arrival);
            now = td::nowNs();
            socketDrops = receiver.drops.load(std::memory_order_relaxed);
            if (now - lastReceiveStats >= 1000000000) {
                double seconds = (now - lastReceiveStats) / 1e9;
                uint64_t calls = receiver.calls - lastCalls,
                         packets = receiver.packets - lastPackets;
                std::cerr << "[UDP] packets_per_s=" << packets / seconds
                          << " recv_calls_per_s=" << calls / seconds
                          << " packets_per_call=" << (calls ? double(packets) / calls : 0)
                          << " socket_drop_delta=" << receiver.drops - lastDrops
                          << " socket_drop_total=" << receiver.drops
                          << " truncated=" << receiver.truncated << '\n';
                uint64_t dataCalls = receiver.dataCalls.load(std::memory_order_relaxed),
                         emptyCalls = receiver.emptyCalls.load(std::memory_order_relaxed);
                std::cerr << "[UDPPoll] empty_calls_per_s="
                          << (emptyCalls - lastEmptyCalls) / seconds << " packets_per_data_call="
                          << (dataCalls > lastDataCalls
                                  ? double(packets) / (dataCalls - lastDataCalls)
                                  : 0)
                          << " rt_budget_yields_total="
                          << receiver.budgetYields.load(std::memory_order_relaxed) << '\n';
                lastDataCalls = dataCalls;
                lastEmptyCalls = emptyCalls;
                auto queue = receiver.takeQueueStatistics();
                std::cerr << "[UDPQueue] peak_packets=" << queue.peakPackets
                          << " wait_avg_ms=" << queue.averageMs
                          << " wait_max_ms=" << queue.maximumMs
                          << " pool_drop_total=" << queue.poolDrops
                          << " stale_video_total=" << queue.staleDrops
                          << " io_error_total=" << queue.ioErrors
                          << " invalid_header_total=" << invalidHeaders
                          << " metadata_mismatch_total=" << metadataMismatch << '\n';
                lastReceiveStats = now;
                lastCalls = receiver.calls;
                lastPackets = receiver.packets;
                lastDrops = receiver.drops;
            }
            if (n > 0 && peer.sin_addr.s_addr != server.sin_addr.s_addr)
                continue;
            if (now - lastPacket > 2000000000 && connected.exchange(false)) {
                std::cerr << "Connection lost\n";
                reportedDisconnected = true;
                std::lock_guard<std::mutex> lock(mutex);
                overlay.visible = false;
                cursorRevision++;
                this->ready.notify_one();
            }
            if (n > 0) {
                receivedBytes += n;
                lastPacket = now;
                if (n >= 5 && n < 64 && peer.sin_port == server.sin_port &&
                    memcmp(buffer, "WAIT ", 5) == 0) {
                    const std::string reason(reinterpret_cast<char *>(buffer) + 5, n - 5);
                    const int wait = reason == "WAKE"      ? 1
                                     : reason == "LOCKED"  ? 3
                                     : reason == "CAPTURE" ? 2
                                                           : 0;
                    if (wait) {
                        connected = false;
                        if (captureWait.exchange(wait) != wait) {
                            std::cerr << "Capture state: " << reason << '\n';
                            frames.clear();
                            needsRefresh = false;
                            std::lock_guard<std::mutex> lock(mutex);
                            latest.reset();
                            videoQueue.clear();
                            overlay.visible = false;
                            cursorRevision++;
                            ready.notify_one();
                        }
                    }
                } else if (size_t(n) < 512 && n >= 8 && memcmp(buffer, "WELCOME ", 8) == 0) {
                    std::istringstream msg(std::string(reinterpret_cast<char *>(buffer), n));
                    std::string type;
                    uint64_t s;
                    int w, h, lw, lh, f, c;
                    if (msg >> type >> s >> w >> h >> lw >> lh >> f >> c) {
                        if (w < 128 || h < 128 || w > 8192 || h > 8192 || f < 10 || f > 144)
                            continue;
                        if (captureWait.exchange(0))
                            std::cerr << "Capture state: READY\n";
                        if (reportedDisconnected) {
                            std::cerr << "Connection restored\n";
                            reportedDisconnected = false;
                        }
                        if (session != s) {
                            frames.clear();
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                latest.reset();
                                videoQueue.clear();
                                overlay = {};
                                relativePredictions.clear();
                                predictedPointer = predictedNs = 0;
                                cursorRevision++;
                                this->ready.notify_one();
                            }
                            h264Recovery = false;
                            pyrowaveRecovery = false;
                            cursorSequence = 0;
                            remoteCursorDrawn = true;
                            assemblingCursor = 0;
                            cursorUnit = {};
                            delivered = 0;
                            presentedID = 0;
                            needsRefresh = false;
                            lastComplete = now;
                            lastRefresh = 0;
                            session = s;
                            videoLanes = 0;
                            clock.reset(s);
                            lastSync = 0;
                            ack = 0;
                            generation++;
                            std::cerr << "Connected: " << w << "x" << h << " @ " << f << " Hz, UI "
                                      << lw << "x" << lh << ", "
                                      << (requestedH264 ? "H264" : "PyroWave") << ' '
                                      << (c ? "4:4:4" : "4:2:0") << '\n';
                        }
                        int edgeMask = 0, cursorMode = 0;
                        msg >> edgeMask >> cursorMode;
                        std::string laneTag;
                        int lanes = 1;
                        if (msg >> laneTag && laneTag == "LANES") {
                            if (!(msg >> lanes) || lanes < 1 || lanes > int(videoSockets.size()))
                                lanes = 1;
                        }
                        std::string codecTag, codecName = "pyrowave";
                        if (msg >> codecTag) {
                            if (codecTag != "CODEC" || !(msg >> codecName) ||
                                (codecName != "h264" && codecName != "pyrowave"))
                                continue;
                        }
                        if (requestedH264 != (codecName == "h264")) {
                            std::cerr << "Mac: Codec negotiation mismatch; update the Mac sender\n";
                            disconnectedByServer = true;
                            running = false;
                            continue;
                        }
                        h264 = codecName == "h264";
                        if (videoLanes != lanes) {
                            videoLanes = lanes;
                            std::cerr << "Video lanes active: " << lanes << '\n';
                        }
                        separateCursor = cursorMode == 1;
                        chroma = c;
                        width = w;
                        height = h;
                        logicalWidth = lw;
                        logicalHeight = lh;
                        edges = edgeMask;
                        fps = f;
                        connected = true;
                    }
                } else if (n == 7 && peer.sin_port == server.sin_port &&
                           memcmp(buffer, "STOPPED", 7) == 0) {
                    connected = false;
                    disconnectedByServer = true;
                    running = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        latest.reset();
                        videoQueue.clear();
                        overlay.visible = false;
                        cursorRevision++;
                        ready.notify_all();
                    }
                    std::cerr << "Disconnected by Mac\n";
                    break;
                } else if (n >= 11 && n < 64 && memcmp(buffer, "DISCONNECT ", 11) == 0) {
                    std::istringstream message(
                        std::string(reinterpret_cast<char *>(buffer) + 11, n - 11));
                    uint64_t target = 0;
                    if (peer.sin_port == server.sin_port && message >> target &&
                        target == session && session) {
                        connected = false;
                        disconnectedByServer = true;
                        running = false;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            latest.reset();
                            videoQueue.clear();
                            overlay.visible = false;
                            cursorRevision++;
                            ready.notify_all();
                        }
                        std::cerr << "Disconnected by Mac\n";
                        break;
                    }
                } else if (n >= 5 && n < 256 && peer.sin_port == server.sin_port &&
                           memcmp(buffer, "SYNC ", 5) == 0) {
                    std::istringstream sync(
                        std::string(reinterpret_cast<char *>(buffer) + 5, n - 5));
                    uint64_t id = 0, t1 = 0, t2 = 0, t3 = 0;
                    if (sync >> id >> t1 >> t2 >> t3 && id == session && t1 == lastSync)
                        clock.accept(id, t1, t2, t3, arrival);
                } else if (n >= 10 && n < 256 && peer.sin_port == server.sin_port &&
                           memcmp(buffer, "INPUT_ACK ", 10) == 0) {
                    std::istringstream message(
                        std::string(reinterpret_cast<char *>(buffer) + 10, n - 10));
                    uint64_t id = 0, seq = 0, t1 = 0, t2 = 0, t3 = 0;
                    if (message >> id >> seq >> t1 >> t2 >> t3 && id == session &&
                        t1 == inputProbe && t1 && now >= t1 && now - t1 < 250000000 && t3 >= t2) {
                        auto estimate = clock.estimate(session, now);
                        std::cerr << "[InputLatency] seq=" << seq
                                  << " send_to_post_ack_ms=" << (now - t1) / 1e6
                                  << " mac_receive_to_post_ms=" << (t3 - t2) / 1e6;
                        if (estimate.valid) {
                            const int64_t elapsed =
                                int64_t(t3) - int64_t(t1) - estimate.serverMinusClient;
                            std::cerr << " send_to_post_est_ms=" << elapsed / 1e6
                                      << " sync_error_ms=" << estimate.errorNs / 1e6;
                        }
                        std::cerr
                            << " (CGEventPost submission, not game consumption or screen scan)\n";
                    }
                } else if (n >= 4 && n < 64 && memcmp(buffer, "ACK ", 4) == 0) {
                    try {
                        auto a =
                            std::stoull(std::string(reinterpret_cast<char *>(buffer) + 4, n - 4));
                        if (a > ack)
                            ack = a;
                    } catch (...) {
                    }
                } else if (n >= 7 && n < 256 && memcmp(buffer, "CURSOR ", 7) == 0) {
                    std::istringstream cursor(std::string(reinterpret_cast<char *>(buffer), n));
                    std::string type;
                    uint64_t seq = 0, shape = 0, cursorSession = 0, pointerAck = 0;
                    float x = 0, y = 0;
                    int inside = 0, drawn = 0;
                    if (cursor >> type >> seq >> x >> y >> inside && std::isfinite(x) &&
                        std::isfinite(y)) {
                        bool separate = separateCursor;
                        if (separate &&
                            (!(cursor >> shape >> cursorSession >> pointerAck >> drawn) ||
                             cursorSession != session))
                            continue;
                        if (seq > cursorSequence) {
                            cursorX = x;
                            cursorY = y;
                            cursorVisible = inside != 0;
                            cursorSequence = seq;
                            if (separate)
                                remoteCursorDrawn = drawn != 0;
                            if (separate) {
                                std::lock_guard<std::mutex> lock(mutex);
                                const bool visible = drawn != 0;
                                const bool predictionExpired =
                                    now >= predictedNs && now - predictedNs > 50000000;
                                if (!visible || predictionExpired)
                                    relativePredictions.clear();
                                if (!relativePredictions.empty()) {
                                    while (!relativePredictions.empty() &&
                                           relativePredictions.front().seq <= pointerAck)
                                        relativePredictions.pop_front();
                                    double px = x, py = y;
                                    for (const auto &motion : relativePredictions) {
                                        px = std::clamp(px + motion.x, 0.0, 1.0);
                                        py = std::clamp(py + motion.y, 0.0, 1.0);
                                    }
                                    x = px;
                                    y = py;
                                    if (overlay.x != x || overlay.y != y ||
                                        overlay.visible != visible) {
                                        overlay.x = x;
                                        overlay.y = y;
                                        overlay.visible = visible;
                                        cursorRevision++;
                                        this->ready.notify_one();
                                    }
                                } else if (!visible || pointerAck >= predictedPointer ||
                                           predictionExpired) {
                                    if (overlay.x != x || overlay.y != y ||
                                        overlay.visible != visible) {
                                        overlay.x = x;
                                        overlay.y = y;
                                        overlay.visible = visible;
                                        cursorRevision++;
                                        this->ready.notify_one();
                                    }
                                }
                            }
                        }
                    }
                } else if (n >= 6 && n < 512 && memcmp(buffer, "ERROR ", 6) == 0) {
                    std::cerr << "Mac: " << std::string(reinterpret_cast<char *>(buffer) + 6, n - 6)
                              << '\n';
                } else {
                    td::Header h;
                    if (captureWait)
                        continue;
                    if (!td::decodeHeader(buffer, n, h)) {
                        if (n >= 4 && !memcmp(buffer, "TDP1", 4)) {
                            if (invalidHeaders++ < 8)
                                std::cerr << "[FrameReject] invalid header size=" << n
                                          << " frame=" << h.frame << " unit=" << h.unit << '/'
                                          << h.units << " bytes=" << h.unitSize
                                          << " fragment=" << h.fragment << '/' << h.fragments
                                          << " payload=" << h.payload << '\n';
                        }
                        continue;
                    }
                    if (h.session != session)
                        continue;
                    if (h.type == 2) {
                        if (!separateCursor)
                            continue;
                        uint64_t installed = 0;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (overlay.image)
                                installed = overlay.image->id;
                        }
                        if (h.frame == installed) {
                            send("CURSOR_ACK " + std::to_string(session) + " " +
                                 std::to_string(h.frame));
                            continue;
                        }
                        if (h.frame < installed || h.frame < assemblingCursor)
                            continue;
                        if (h.frame != assemblingCursor) {
                            assemblingCursor = h.frame;
                            cursorHeader = h;
                            cursorUnit = {};
                            cursorUnit.data.resize(h.unitSize);
                            cursorUnit.received.resize(h.fragments);
                        }
                        if (h.unitSize != cursorHeader.unitSize ||
                            h.fragments != cursorHeader.fragments ||
                            h.width != cursorHeader.width || h.height != cursorHeader.height ||
                            h.epoch != cursorHeader.epoch || h.flags != cursorHeader.flags)
                            continue;
                        if (!cursorUnit.received[h.fragment]) {
                            memcpy(cursorUnit.data.data() + h.offset, buffer + td::HeaderSize,
                                   h.payload);
                            cursorUnit.received[h.fragment] = true;
                            cursorUnit.count++;
                        }
                        if (cursorUnit.count == h.fragments) {
                            auto image = std::make_shared<td::CursorImage>();
                            image->id = h.frame;
                            image->width = h.width;
                            image->height = h.height;
                            image->hotX = h.epoch >> 16;
                            image->hotY = h.epoch & 65535;
                            if (td::decodeCursor(cursorUnit.data, h.flags != 0,
                                                 size_t(h.width) * h.height, image->rgba)) {
                                {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    overlay.image = std::move(image);
                                    cursorRevision++;
                                    this->ready.notify_one();
                                }
                                send("CURSOR_ACK " + std::to_string(session) + " " +
                                     std::to_string(h.frame));
                                std::cerr << "Cursor shape: " << h.width << 'x' << h.height
                                          << " hotspot " << (h.epoch >> 16) << ','
                                          << (h.epoch & 65535) << '\n';
                            } else {
                                assemblingCursor = 0;
                                cursorUnit = {};
                            }
                        }
                        continue;
                    }
                    if (bool(h.flags & td::FrameH264) != h264.load())
                        continue;
                    if (h.frame <= delivered)
                        continue;
                    if (!frames.count(h.frame)) {
                        if (frames.size() >= 3) {
                            assemblyDrops++;
                            expiredFrames++;
                            incompleteFrames++;
                            needsRefresh = true;
                            reportIncomplete(frames.begin()->second, "capacity");
                            frames.erase(frames.begin());
                        }
                        Frame frame;
                        frame.header = h;
                        frame.firstNs = arrival;
                        frame.units.resize(h.units);
                        frames.emplace(h.frame, std::move(frame));
                    }
                    auto &frame = frames[h.frame];
                    frame.packets++;
                    frame.lastNs = arrival;
                    if (frame.header.units != h.units || frame.header.width != h.width ||
                        frame.header.height != h.height || frame.header.chroma != h.chroma ||
                        ((frame.header.flags ^ h.flags) & (td::FrameH264 | td::FrameKeyframe))) {
                        metadataMismatch++;
                        continue;
                    }
                    auto &unit = frame.units[h.unit];
                    if (unit.data.empty()) {
                        if (frame.bytes + h.unitSize > 16 * 1024 * 1024) {
                            frames.erase(h.frame);
                            continue;
                        }
                        unit.data.resize(h.unitSize);
                        unit.received.resize(h.fragments);
                        frame.bytes += h.unitSize;
                    }
                    if (unit.data.size() != h.unitSize || unit.received.size() != h.fragments) {
                        metadataMismatch++;
                        continue;
                    }
                    if (!unit.received[h.fragment]) {
                        memcpy(unit.data.data() + h.offset, buffer + td::HeaderSize, h.payload);
                        unit.received[h.fragment] = true;
                        if (++unit.count == h.fragments) {
                            unit.done = true;
                            frame.completed++;
                        }
                    }
                    completedVideo = frame.completed == frame.units.size();
                }
            }
            // Check housekeeping once per batch, but publish a complete frame without
            // waiting for the rest of that batch or for the socket queue to drain.
            if (n > 0 && !completedVideo && !receiver.batchFinished())
                continue;
            // Give complete frames priority. Once a PyroWave frame expires, let the
            // decoder decide whether its complete packets can reconstruct a usable image.
            std::unique_ptr<ReadyFrame> ready;
            std::deque<std::unique_ptr<ReadyFrame>> ordered;
            uint64_t newest = 0;
            for (auto it = frames.begin(); it != frames.end();) {
                auto &f = it->second;
                bool full = f.completed == f.units.size();
                uint64_t deadline =
                    (f.header.flags & td::FrameKeyframe)
                        ? 100000000
                        : std::max<uint64_t>(25000000, 1500000000 / std::max(10, fps.load()));
                const bool isH264 = (f.header.flags & td::FrameH264) != 0;
                if (!full && (now - f.firstNs < deadline || (isH264 && n > 0))) {
                    if (h264)
                        break;
                    ++it;
                    continue;
                }
                // Unit zero contains the PyroWave sequence header. Never feed a torn
                // packet: the unwritten bytes in a fragmented unit are not valid data.
                bool recoverable = !isH264 && !f.units.empty() && f.units[0].done;
                if ((full || recoverable) && it->first > newest &&
                    (full || !ready || ready->partial)) {
                    auto r = std::make_unique<ReadyFrame>();
                    r->width = f.header.width;
                    r->height = f.header.height;
                    r->chroma = f.header.chroma;
                    r->id = it->first;
                    r->firstNs = f.firstNs;
                    r->readyNs = now;
                    r->generation = generation;
                    r->h264 = (f.header.flags & td::FrameH264) != 0;
                    r->key = (f.header.flags & td::FrameKeyframe) != 0;
                    r->partial = !full;
                    r->session = f.header.session;
                    r->captureNs = f.header.captureNs;
                    r->timed = (f.header.flags & td::FrameTimeValid) != 0;
                    r->callbackTime = (f.header.flags & td::FrameCallbackTime) != 0;
                    r->units.resize(f.units.size());
                    for (size_t i = 0; i < f.units.size(); i++)
                        if (f.units[i].done)
                            r->units[i] = std::move(f.units[i].data);
                    newest = it->first;
                    if (h264)
                        ordered.push_back(std::move(r));
                    else {
                        if (ready) {
                            queueDrops++;
                            expiredFrames++;
                        }
                        ready = std::move(r);
                    }
                } else {
                    assemblyDrops++;
                    expiredFrames++;
                    if (!full) {
                        incompleteFrames++;
                        needsRefresh = true;
                        reportIncomplete(f, "deadline");
                    }
                }
                it = frames.erase(it);
            }
            if (!ordered.empty()) {
                delivered = ordered.back()->id;
                lastComplete = now;
                needsRefresh = false;
                std::lock_guard<std::mutex> lock(mutex);
                if (videoQueue.size() + ordered.size() > 8) {
                    queueDrops += videoQueue.size();
                    expiredFrames += videoQueue.size();
                    videoQueue.clear();
                    h264Recovery = true;
                }
                while (!ordered.empty()) {
                    videoQueue.push_back(std::move(ordered.front()));
                    ordered.pop_front();
                    receivedFrames++;
                }
                this->ready.notify_one();
            }
            if (ready) {
                delivered = ready->id;
                if (!ready->partial)
                    lastComplete = now;
                needsRefresh = ready->partial;
                for (auto it = frames.begin(); it != frames.end() && it->first <= delivered;) {
                    assemblyDrops++;
                    expiredFrames++;
                    if (it->second.completed != it->second.units.size()) {
                        incompleteFrames++;
                        reportIncomplete(it->second, "newer_complete");
                    }
                    it = frames.erase(it);
                }
                std::lock_guard<std::mutex> lock(mutex);
                if (latest && !latest->partial && ready->partial) {
                    // A partial update must not evict a complete image already waiting
                    // for the renderer. Keep requesting a clean refresh if needed.
                    queueDrops++;
                    expiredFrames++;
                } else {
                    if (latest) {
                        queueDrops++;
                        expiredFrames++;
                    }
                    latest = std::move(ready);
                    receivedFrames++;
                    this->ready.notify_one();
                }
            }
            // The last desktop update may be lost just before SCK goes idle. Request
            // its cached image after a gap instead of resending on every dropped frame.
            if ((needsRefresh || (!h264 && pyrowaveRecovery) ||
                 (h264 && (h264Recovery || !delivered))) &&
                connected && now - lastComplete >= 100000000 && now - lastRefresh >= 100000000) {
                send("REFRESH");
                lastRefresh = now;
                refreshRequests++;
            }
        }
    }
};

class RenderLoop {
  public:
    std::atomic<uint64_t> drawn{0}, cursorDrawn{0};

  private:
    Client &client;
    VulkanView &view;
    std::atomic<bool> active{true}, failed{false};
    std::exception_ptr error;
    std::thread thread;

    void run() {
        try {
            uint64_t last = td::nowNs(), samples = 0, gpuSamples = 0, cursorRevision = 0,
                     nextRedraw = 0, lastOverlay = 0, lastLatencyLog = 0;
            unsigned redrawRetries = 0;
            double assembly = 0, queued = 0, fence = 0, feed = 0, acquire = 0, record = 0,
                   present = 0, decodeGPU = 0, presentGPU = 0, maxAge = 0;

            struct FrameTime {
                uint64_t id = 0, session = 0, capture = 0, generation = 0;
                bool valid = false, callbackTime = false;
            } pending;

            std::unique_ptr<H264Decoder> avc;
            YuvFrame decodedYuv;
            uint64_t h264Generation = 0, h264Previous = 0, h264Refresh = 0;
            bool waitingKey = true;
            double h264DecodeMs = 0;
            double latency = 0, lastError = 0, lastRTT = 0, lastDecode = 0, lastDraw = 0,
                   lastAssembly = 0, lastQueue = 0;
            uint64_t lastLatency = 0, latencyGeneration = 0;
            bool overlayShown = false, lastCallbackTime = false;
            auto collect = [&](bool pollGPU) {
                VulkanView::Completion done;
                if (!view.pollCompletion(done, pollGPU))
                    return;
                if (done.decoded) {
                    gpuSamples++;
                    decodeGPU += done.decodeGPU;
                    presentGPU += done.presentGPU;
                    lastDecode = done.decodeGPU;
                    lastDraw = done.presentGPU;
                }
                if (done.decoded && pending.valid && pending.generation == client.generation &&
                    client.connected && done.hostNs) {
                    auto estimate = client.clock.estimate(pending.session, td::nowNs());
                    if (estimate.valid) {
                        int64_t elapsed = int64_t(done.hostNs) + estimate.serverMinusClient -
                                          int64_t(pending.capture);
                        if (elapsed > 0 && elapsed < 500000000) {
                            uint64_t now = td::nowNs();
                            if (lastCallbackTime != pending.callbackTime) {
                                lastLatency = 0;
                                lastCallbackTime = pending.callbackTime;
                            }
                            double sample = elapsed / 1e6,
                                   alpha =
                                       lastLatency && now - lastLatency < 2000000000
                                           ? 1 - std::exp(-double(now - lastLatency) / 500000000.0)
                                           : 1;
                            latency = lastLatency ? latency + (sample - latency) * alpha : sample;
                            lastLatency = now;
                            lastError = (estimate.errorNs + done.errorNs) / 1e6;
                            lastRTT = estimate.rttNs / 1e6;
                            if (now - lastLatencyLog > 1000000000) {
                                lastLatencyLog = now;
                                std::cerr
                                    << "[Latency] frame=" << pending.id
                                    << " capture_to_gpu_ms=" << sample << " smooth_ms=" << latency
                                    << " clock_error_est_ms=" << lastError << " rtt_ms=" << lastRTT
                                    << " basis="
                                    << (lastCallbackTime ? "capture_callback" : "display_timestamp")
                                    << '\n';
                            }
                        }
                    }
                }
                pending = {};
            };
            while (active) {
                collect(true);
                const uint64_t oldRevision = cursorRevision;
                const bool visible = client.statsVisible;
                int wake = visible ? (view.completionPending() ? 4 : 250) : 0;
                td::CursorState cursor;
                auto frame = client.waitTake(active, cursorRevision, cursor, nextRedraw, wake);
                if (!active)
                    break;
                collect(true);
                uint64_t now = td::nowNs();
                if (latencyGeneration != client.generation) {
                    latencyGeneration = client.generation;
                    lastLatency = 0;
                    latency = 0;
                }
                bool overlayChanged =
                    visible != overlayShown || (visible && now - lastOverlay > 500000000);
                if (overlayChanged) {
                    overlayShown = visible;
                    lastOverlay = now;
                    std::ostringstream text;
                    text << std::fixed << std::setprecision(1);
                    if (visible) {
                        text << "ThunderDisplay  " << client.width.load() << "x"
                             << client.height.load() << "  " << (client.h264 ? "H264" : "PyroWave")
                             << ' ' << (client.chroma.load() == 1 ? "4:4:4" : "4:2:0") << '\n';
                        text << client.displayFPS.load() << " FPS   " << client.displayMbps.load()
                             << " Mbps / ";
                        if (lastLatency)
                            text << latency << " ms\n";
                        else
                            text << "-- ms\n";
                        if (lastLatency)
                            text << "Last frame age: " << (now - lastLatency) / 1e9
                                 << " s   sync +/- " << lastError << " ms\n";
                        else
                            text << "Frame latency: waiting for calibrated frame\n";
                        if (client.h264) {
                            text << "Decode + download wall: " << h264DecodeMs << " ms\n";
                            if (avc)
                                text << avc->name() << '\n';
                        }
                        text << (client.h264 ? "GPU upload / draw: " : "GPU decode / draw: ")
                             << lastDecode << " / " << lastDraw << " ms\n";
                        text << "Assembly / queue: " << lastAssembly << " / " << lastQueue
                             << " ms\n";
                        text << "Clock RTT: " << lastRTT << " ms (offset estimate)\n";
                        text << "Dropped: " << client.expiredFrames.load()
                             << "   incomplete: " << client.incompleteFrames.load()
                             << "   socket: " << client.socketDrops.load() << '\n';
                        if (!client.h264)
                            text << "Partial: " << client.partialFrames.load() << " shown / "
                                 << client.partialRejected.load() << " rejected\n";
                        text << (lastCallbackTime ? "Capture callback -> client GPU ready"
                                                  : "Source display -> client GPU ready")
                             << ", not screen scan\nCtrl+Alt+Shift+S  Hide statistics";
                    }
                    view.setStatsOverlay(text.str());
                }
                if (!frame && cursorRevision == oldRevision && !overlayChanged)
                    continue;
                if (frame || cursorRevision != oldRevision)
                    view.setCursor(cursor);
                nextRedraw = td::nowNs() + 1000000000 / std::clamp(client.fps.load(), 30, 120);
                if (!frame) {
                    if (view.redraw()) {
                        cursorDrawn++;
                        redrawRetries = 0;
                    } else if (redrawRetries++ < 3)
                        client.redraw();
                    collect(false);
                    continue;
                }
                redrawRetries = 0;
                if (frame->generation != client.generation)
                    continue;
                uint64_t began = td::nowNs();
                bool shown = false;
                if (frame->h264) {
                    auto refresh = [&] {
                        client.h264Recovery = true;
                        const uint64_t now = td::nowNs();
                        if (now - h264Refresh >= 100000000) {
                            client.send("REFRESH");
                            client.refreshRequests++;
                            h264Refresh = now;
                        }
                    };
                    if (!avc)
                        avc = std::make_unique<H264Decoder>();
                    if (h264Generation != frame->generation) {
                        h264Generation = frame->generation;
                        h264Previous = 0;
                        waitingKey = true;
                        avc->reset();
                    }
                    if (h264Previous && frame->id != h264Previous + 1) {
                        if (!waitingKey)
                            std::cerr << "[H264] sequence gap previous=" << h264Previous
                                      << " received=" << frame->id << "; waiting for IDR\n";
                        waitingKey = true;
                        avc->reset();
                    }
                    if (waitingKey && !frame->key) {
                        refresh();
                        continue;
                    }
                    if (frame->key) {
                        if (waitingKey) {
                            avc->reset();
                            std::cerr << "[H264] IDR recovered frame=" << frame->id << '\n';
                        }
                        waitingKey = false;
                    }
                    h264Previous = frame->id;
                    const uint64_t decodeBegan = td::nowNs();
                    if (!avc->decode(frame->units, decodedYuv)) {
                        waitingKey = true;
                        refresh();
                        continue;
                    }
                    if (decodedYuv.width != frame->width || decodedYuv.height != frame->height)
                        throw std::runtime_error("H264 frame size differs from negotiated display");
                    client.h264Recovery = false;
                    h264DecodeMs = (td::nowNs() - decodeBegan) / 1e6;
                    shown = view.showYuv(decodedYuv);
                } else {
                    shown = view.show(frame->width, frame->height, frame->chroma, frame->units,
                                      frame->partial);
                    if (frame->generation == client.generation) {
                        if (shown) {
                            client.pyrowaveRecovery = view.timings().partial;
                            if (view.timings().partial)
                                client.partialFrames++;
                        } else {
                            client.pyrowaveRecovery = true;
                            if (frame->partial)
                                client.partialRejected++;
                        }
                    }
                }
                collect(false);
                if (shown) {
                    drawn++;
                    if (frame->generation == client.generation)
                        client.presentedID = frame->id;
                    pending = {frame->id,         frame->session, frame->captureNs,
                               frame->generation, frame->timed,   frame->callbackTime};
                } else {
                    client.displayDrops++;
                    client.expiredFrames++;
                }
                auto &t = view.timings();
                samples++;
                lastAssembly = (frame->readyNs - frame->firstNs) / 1e6;
                lastQueue = (began - frame->readyNs) / 1e6;
                assembly += lastAssembly;
                queued += lastQueue;
                fence += t.fence;
                feed += t.feed;
                acquire += t.acquire;
                record += t.record;
                present += t.presentCPU;
                maxAge = std::max(maxAge, (td::nowNs() - frame->firstNs) / 1e6);
                now = td::nowNs();
                if (now - last > 1000000000) {
                    if (frame->h264)
                        std::cerr << "[H264Pipeline] decoder=" << avc->name()
                                  << " decode_download_wall_ms=" << h264DecodeMs << '\n';
                    fprintf(
                        stderr,
                        "[ReceiverPipeline] avg_ms assembly=%.2f render_wait=%.2f gpu_fence=%.2f feed=%.2f acquire=%.2f record=%.2f present_cpu=%.2f gpu_decode=%.2f gpu_present=%.2f first_packet_to_submit_max=%.2f\n",
                        assembly / samples, queued / samples, fence / samples, feed / samples,
                        acquire / samples, record / samples, present / samples,
                        gpuSamples ? decodeGPU / gpuSamples : -1.0,
                        gpuSamples ? presentGPU / gpuSamples : -1.0, maxAge);
                    last = now;
                    samples = gpuSamples = 0;
                    assembly = queued = fence = feed = acquire = record = present = decodeGPU =
                        presentGPU = maxAge = 0;
                }
            }
        } catch (...) {
            error = std::current_exception();
            failed = true;
        }
    }

  public:
    RenderLoop(Client &c, VulkanView &v)
        : client(c), view(v), thread([this] {
              run();
          }) {
    }

    ~RenderLoop() {
        {
            std::lock_guard<std::mutex> lock(client.mutex);
            active = false;
        }
        client.ready.notify_all();
        if (thread.joinable())
            thread.join();
    }

    void check() {
        if (failed)
            std::rethrow_exception(error);
    }
};
}

int runReceiver(const std::string &host, bool fullscreen, const DisplayRequest &request,
                const KeyboardOptions &keyboard, bool settingsStdin) {
    ReleaseShortcut release;
    if (!parseReleaseShortcut(keyboard.releaseShortcut, release))
        throw std::runtime_error("Invalid release shortcut; use a key such as F8 or Ctrl+Alt+F8");
    SDL_Scancode releaseKey = SDL_GetScancodeFromName(release.key.c_str());
    if (releaseKey == SDL_SCANCODE_UNKNOWN)
        throw std::runtime_error("Unsupported release shortcut key");
    ReleaseShortcut fullscreenBinding;
    if (!parseReleaseShortcut(keyboard.fullscreenShortcut, fullscreenBinding))
        throw std::runtime_error("Invalid fullscreen shortcut");
    SDL_Scancode fullscreenKey = SDL_GetScancodeFromName(fullscreenBinding.key.c_str());
    if (fullscreenKey == SDL_SCANCODE_UNKNOWN)
        throw std::runtime_error("Unsupported fullscreen shortcut key");
    if (releaseKey == fullscreenKey && release.modifiers == fullscreenBinding.modifiers)
        throw std::runtime_error("Release and fullscreen shortcuts must be different");
    if ((releaseKey == SDL_SCANCODE_S && release.modifiers == 7) ||
        (fullscreenKey == SDL_SCANCODE_S && fullscreenBinding.modifiers == 7))
        throw std::runtime_error("Ctrl+Alt+Shift+S is reserved for client statistics");
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
    stopRequested = 0;
    std::signal(SIGTERM, requestStop);
    std::signal(SIGINT, requestStop);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        throw std::runtime_error(SDL_GetError());
    SDL_Window *window =
        SDL_CreateWindow("ThunderDisplay · connecting to Mac", 1120, 700,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                             (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window)
        throw std::runtime_error(SDL_GetError());
    try {
        VulkanView view(window);
        Client client(host, request);
        RenderLoop renderer(client, view);
        bool quit = false, capture = !keyboard.startReleased, relative = false,
             handoffOutside = false, cursorHidden = false, keyboardGrabbed = false,
             releaseHeld = false, fullscreenHeld = false, statsHeld = false,
             fullscreenRequested = fullscreen;
        std::array<bool, SDL_SCANCODE_COUNT> pressedKeys{};
        int ignoreWarp = 0;
        SDL_SetWindowMouseGrab(window, false);
        uint64_t generation = 0, eventSeq = 0, pointerSeq = 0, lastResend = 0,
                 lastStats = td::nowNs(), statsFrames = 0, statsBytes = 0,
                 previousLoop = td::nowNs();
        double inputLoopMax = 0;
        unsigned buttons = 0;
        double nx = 0.5, ny = 0.5;
        std::map<uint64_t, std::string> pending;
        std::string settingsInput;
        bool settingsOpen = settingsStdin;
        std::cerr << "Local shortcuts: release=" << keyboard.releaseShortcut
                  << " (scan=" << releaseKey << ", mods=" << release.modifiers
                  << "), fullscreen=" << keyboard.fullscreenShortcut << " (scan=" << fullscreenKey
                  << ", mods=" << fullscreenBinding.modifiers << ")\n";
        int sentCaps = -1;
        bool gameMouse = keyboard.gameMouse, gameRelative = false, gameLockFailed = false,
             desktopGameMotion = false;
        int sentGameMode = -1;
        int previousCaptureWait = 0;
        uint64_t gameEpoch = 0;
        double gameX = 0, gameY = 0, sentGameX = 0, sentGameY = 0;
        uint64_t motionEvents = 0, motionPackets = 0, eventAgeSamples = 0, lastInputProbe = 0;
        double eventAgeTotal = 0, eventAgeMax = 0;
        SDL_Event queuedEvent{};
        bool hasQueuedEvent = false;
        auto sendEvent = [&](const std::string &text) {
            if (!client.connected)
                return;
            if (pending.size() >= 250)
                throw std::runtime_error("Input channel stalled; reconnect to release keys");
            auto seq = ++eventSeq;
            std::string message = "E " + std::to_string(seq) + " " + text;
            pending.emplace(seq, message);
            client.send(message);
            if (text == "RESET") {
                client.clearRelativePrediction();
                sentGameMode = -1;
                gameX = gameY = sentGameX = sentGameY = 0;
            }
        };
        auto pointer = [&](float x, float y) {
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            double fit =
                std::min(double(w) / client.width.load(), double(h) / client.height.load());
            double dw = client.width * fit, dh = client.height * fit, dx = (w - dw) / 2,
                   dy = (h - dh) / 2;
            nx = std::clamp((x - dx) / dw, 0.0, 1.0);
            ny = std::clamp((y - dy) / dh, 0.0, 1.0);
        };
        while (!quit && !stopRequested && !client.disconnectedByServer) {
            renderer.check();
            uint64_t loopNow = td::nowNs();
            inputLoopMax = std::max(inputLoopMax, (loopNow - previousLoop) / 1e6);
            previousLoop = loopNow;
            if (settingsOpen) {
                pollfd input{STDIN_FILENO, POLLIN, 0};
                if (poll(&input, 1, 0) > 0) {
                    if (input.revents & POLLIN) {
                        char data[256];
                        ssize_t count = read(STDIN_FILENO, data, sizeof(data));
                        if (count > 0) {
                            settingsInput.append(data, size_t(count));
                            size_t end;
                            while ((end = settingsInput.find('\n')) != std::string::npos) {
                                std::istringstream message(settingsInput.substr(0, end));
                                settingsInput.erase(0, end + 1);
                                std::string kind, extra;
                                message >> kind;
                                if (kind == "GAME_MOUSE") {
                                    int enabled = 0;
                                    if (message >> enabled && (enabled == 0 || enabled == 1) &&
                                        !(message >> extra)) {
                                        gameMouse = enabled != 0;
                                        gameLockFailed = false;
                                    }
                                } else if (kind == "DEBUG_OVERLAY") {
                                    int enabled = 0;
                                    if (message >> enabled && (enabled == 0 || enabled == 1) &&
                                        !(message >> extra)) {
                                        client.statsVisible = enabled != 0;
                                        client.redraw();
                                        std::cerr
                                            << "Statistics overlay: " << (enabled ? "on" : "off")
                                            << '\n';
                                    }
                                }
                            }
                            if (settingsInput.size() > 1024)
                                settingsInput.clear();
                        } else if (count == 0)
                            settingsOpen = false;
                    } else if (input.revents & (POLLHUP | POLLERR | POLLNVAL))
                        settingsOpen = false;
                }
            }
            const int waiting = client.captureWait.load();
            if (waiting != previousCaptureWait) {
                previousCaptureWait = waiting;
                sentGameMode = -1;
                sentCaps = -1;
                buttons = 0;
                pressedKeys.fill(false);
            }
            if (client.generation != generation) {
                generation = client.generation;
                eventSeq = pointerSeq = 0;
                pending.clear();
                buttons = 0;
                sentCaps = -1;
                sendEvent("RESET");
            }
            if (gameRelative && !SDL_GetWindowRelativeMouseMode(window))
                gameRelative = false;
            bool wantGame =
                gameMouse && capture && client.connected && SDL_GetKeyboardFocus() == window;
            if (!wantGame)
                gameLockFailed = false;
            if (wantGame && !gameRelative && !gameLockFailed) {
                relative = false;
                handoffOutside = false;
                ignoreWarp = 0;
                gameRelative = SDL_SetWindowRelativeMouseMode(window, true);
                gameLockFailed = !gameRelative;
                if (gameRelative)
                    std::cerr << "Game mouse: locked\n";
                else
                    std::cerr << "ThunderDisplay: 游戏鼠标锁定失败：" << SDL_GetError() << '\n';
            } else if (!wantGame && gameRelative) {
                SDL_SetWindowRelativeMouseMode(window, false);
                gameRelative = false;
                ignoreWarp = 0;
                std::cerr << "Game mouse: released\n";
            }
            const bool wantDesktopGameMotion = gameRelative && client.remoteCursorDrawn;
            if (wantDesktopGameMotion != desktopGameMotion) {
                desktopGameMotion = wantDesktopGameMotion;
                client.clearRelativePrediction();
                SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, desktopGameMotion ? "1" : "0");
                std::cerr << "Mouse motion: "
                          << (desktopGameMotion ? "desktop system speed + display mapping"
                                                : "raw relative")
                          << '\n';
            }
            if (client.connected && sentGameMode != int(gameRelative)) {
                client.clearRelativePrediction();
                gameX = gameY = sentGameX = sentGameY = 0;
                gameEpoch = eventSeq + 1;
                sendEvent("MOUSE_MODE " + std::to_string(gameRelative ? 1 : 0) + " " +
                          std::to_string(gameEpoch));
                sentGameMode = gameRelative ? 1 : 0;
            }
            bool grab = keyboard.exclusive && capture && client.connected &&
                        SDL_GetKeyboardFocus() == window;
            if (grab != keyboardGrabbed) {
                keyboardGrabbed = grab;
                if (SDL_SetWindowKeyboardGrab(window, grab))
                    std::cerr << "Keyboard grab: " << (grab ? "enabled" : "released")
                              << "; release shortcut=" << keyboard.releaseShortcut << '\n';
                else
                    std::cerr << "Keyboard grab unavailable: " << SDL_GetError() << '\n';
            }
            bool hide = capture && client.connected && SDL_GetMouseFocus() == window;
            if (hide != cursorHidden) {
                if (hide)
                    SDL_HideCursor();
                else
                    SDL_ShowCursor();
                cursorHidden = hide;
            }
            if (relative && !gameRelative) {
                if (!client.cursorVisible)
                    handoffOutside = true;
                if ((handoffOutside && client.cursorVisible) || !client.connected) {
                    relative = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                    if (client.connected) {
                        int ww, wh;
                        SDL_GetWindowSize(window, &ww, &wh);
                        double fit = std::min(double(ww) / client.width.load(),
                                              double(wh) / client.height.load());
                        float x = float((ww - client.width * fit) / 2 +
                                        std::clamp(client.cursorX.load(), 0.f, 1.f) * client.width *
                                            fit),
                              y = float((wh - client.height * fit) / 2 +
                                        std::clamp(client.cursorY.load(), 0.f, 1.f) *
                                            client.height * fit);
                        SDL_WarpMouseInWindow(window, x, y);
                        ignoreWarp = 2;
                    }
                }
            }
            SDL_Event event;
            auto nextEvent = [&] {
                if (hasQueuedEvent) {
                    event = queuedEvent;
                    hasQueuedEvent = false;
                    return true;
                }
                return SDL_PollEvent(&event);
            };
            while (nextEvent()) {
                if (event.type == SDL_EVENT_QUIT)
                    quit = true;
                else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                         event.type == SDL_EVENT_WINDOW_RESIZED) {
                    view.resize();
                    client.redraw();
                } else if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
                           event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
                    fullscreen = event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
                    fullscreenRequested = fullscreen;
                    view.resize();
                    client.redraw();
                    std::cerr << "Fullscreen: " << (fullscreen ? "on" : "off") << '\n';
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    int count = 0;
                    const bool *state = SDL_GetKeyboardState(&count);
                    std::copy_n(state, std::min(count, int(pressedKeys.size())),
                                pressedKeys.begin());
                    sentCaps = -1;
                    if (keyboard.diagnostics)
                        std::cerr << "[Shortcut] keyboard focus gained\n";
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    pressedKeys.fill(false);
                    releaseHeld = fullscreenHeld = statsHeld = false;
                    sendEvent("RESET");
                    buttons = 0;
                    relative = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                    if (keyboard.diagnostics)
                        std::cerr << "[Shortcut] keyboard focus lost\n";
                } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                    if (event.key.scancode > SDL_SCANCODE_UNKNOWN &&
                        event.key.scancode < SDL_SCANCODE_COUNT)
                        pressedKeys[event.key.scancode] = event.type == SDL_EVENT_KEY_DOWN;
                    const unsigned modifiers = ((event.key.mod & SDL_KMOD_CTRL) ? 1 : 0) |
                                               ((event.key.mod & SDL_KMOD_SHIFT) ? 2 : 0) |
                                               ((event.key.mod & SDL_KMOD_ALT) ? 4 : 0) |
                                               ((event.key.mod & SDL_KMOD_GUI) ? 8 : 0);
                    const unsigned tracked =
                        ((pressedKeys[SDL_SCANCODE_LCTRL] || pressedKeys[SDL_SCANCODE_RCTRL]) ? 1
                                                                                              : 0) |
                        ((pressedKeys[SDL_SCANCODE_LSHIFT] || pressedKeys[SDL_SCANCODE_RSHIFT])
                             ? 2
                             : 0) |
                        ((pressedKeys[SDL_SCANCODE_LALT] || pressedKeys[SDL_SCANCODE_RALT]) ? 4
                                                                                            : 0) |
                        ((pressedKeys[SDL_SCANCODE_LGUI] || pressedKeys[SDL_SCANCODE_RGUI]) ? 8
                                                                                            : 0);
                    auto matches = [&](unsigned required) {
                        return required ? (modifiers == required || tracked == required)
                                        : (modifiers == 0 && tracked == 0);
                    };
                    if (event.key.scancode == SDL_SCANCODE_S) {
                        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && matches(7)) {
                            statsHeld = true;
                            client.statsVisible = !client.statsVisible;
                            client.redraw();
                            // Release only the chord modifiers already forwarded before S. Keep
                            // game mouse mode, held movement keys and mouse buttons intact.
                            for (auto key :
                                 {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_LSHIFT,
                                  SDL_SCANCODE_RSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT})
                                if (pressedKeys[key])
                                    sendEvent(
                                        "K " + std::to_string(key) + " 0 " +
                                        std::to_string((event.key.mod & SDL_KMOD_CAPS) ? 1 : 0));
                            std::cerr
                                << "Statistics overlay: " << (client.statsVisible ? "on" : "off")
                                << '\n';
                            continue;
                        }
                        if (statsHeld) {
                            if (event.type == SDL_EVENT_KEY_UP)
                                statsHeld = false;
                            continue;
                        }
                    }
                    if (keyboard.diagnostics &&
                        (event.key.scancode >= SDL_SCANCODE_CAPSLOCK ||
                         event.key.scancode == releaseKey || event.key.scancode == fullscreenKey))
                        std::cerr << "[Shortcut] "
                                  << (event.type == SDL_EVENT_KEY_DOWN ? "down" : "up")
                                  << " scan=" << event.key.scancode
                                  << " name=" << SDL_GetScancodeName(event.key.scancode)
                                  << " raw=" << event.key.raw << " event_mods=" << modifiers
                                  << " held_mods=" << tracked << " repeat=" << event.key.repeat
                                  << '\n';
                    if (event.key.scancode == releaseKey) {
                        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                            matches(release.modifiers)) {
                            releaseHeld = true;
                            capture = !capture;
                            sentCaps = -1;
                            sendEvent("RESET");
                            buttons = 0;
                            SDL_SetWindowMouseGrab(window, false);
                            relative = false;
                            SDL_SetWindowRelativeMouseMode(window, false);
                            std::cerr << "Input capture: " << (capture ? "on" : "off") << '\n';
                            continue;
                        }
                        if (releaseHeld) {
                            if (event.type == SDL_EVENT_KEY_UP)
                                releaseHeld = false;
                            continue;
                        }
                    }
                    if (event.key.scancode == fullscreenKey) {
                        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                            matches(fullscreenBinding.modifiers)) {
                            fullscreenHeld = true;
                            sendEvent("RESET");
                            buttons = 0;
                            if (SDL_SetWindowFullscreen(window, !fullscreenRequested)) {
                                fullscreenRequested = !fullscreenRequested;
                                std::cerr << "Fullscreen requested: "
                                          << (fullscreenRequested ? "on" : "off") << '\n';
                            } else
                                std::cerr << "Fullscreen unavailable: " << SDL_GetError() << '\n';
                            continue;
                        }
                        if (fullscreenHeld) {
                            if (event.type == SDL_EVENT_KEY_UP)
                                fullscreenHeld = false;
                            continue;
                        }
                    }
                    if (capture) {
                        const int caps = (event.key.mod & SDL_KMOD_CAPS) ? 1 : 0;
                        sendEvent("K " + std::to_string(event.key.scancode) + " " +
                                  std::to_string(event.type == SDL_EVENT_KEY_DOWN ? 1 : 0) + " " +
                                  std::to_string(caps));
                        sentCaps = caps;
                    }
                } else if (event.type == SDL_EVENT_MOUSE_MOTION && capture && client.connected) {
                    if (gameRelative) {
                        motionEvents++;
                        const uint64_t sampled = SDL_GetTicksNS();
                        if (event.motion.timestamp && sampled >= event.motion.timestamp &&
                            sampled - event.motion.timestamp < 1000000000) {
                            const double age = (sampled - event.motion.timestamp) / 1e6;
                            eventAgeTotal += age;
                            eventAgeMax = std::max(eventAgeMax, age);
                            eventAgeSamples++;
                        }
                        double dx = event.motion.xrel, dy = event.motion.yrel;
                        if (desktopGameMotion) {
                            int ww, wh;
                            SDL_GetWindowSize(window, &ww, &wh);
                            const double fit = std::min(double(ww) / client.width.load(),
                                                        double(wh) / client.height.load());
                            if (fit <= 0)
                                continue;
                            dx *= double(client.logicalWidth.load()) / (client.width.load() * fit);
                            dy *=
                                double(client.logicalHeight.load()) / (client.height.load() * fit);
                        }
                        gameX += dx;
                        gameY += dy;
                        continue;
                    }
                    if (ignoreWarp > 0) {
                        ignoreWarp--;
                        continue;
                    }
                    pointer(event.motion.x, event.motion.y);
                    int ww, wh;
                    SDL_GetWindowSize(window, &ww, &wh);
                    double fit = std::min(double(ww) / client.width.load(),
                                          double(wh) / client.height.load());
                    unsigned edgeMask = client.edges;
                    bool cross =
                        fullscreen && ((nx < 0.004 && event.motion.xrel < 0 && (edgeMask & 1)) ||
                                       (nx > 0.996 && event.motion.xrel > 0 && (edgeMask & 2)) ||
                                       (ny < 0.004 && event.motion.yrel < 0 && (edgeMask & 4)) ||
                                       (ny > 0.996 && event.motion.yrel > 0 && (edgeMask & 8)));
                    if (cross && !relative) {
                        std::ostringstream edge;
                        edge << "P " << ++pointerSeq << ' ' << nx << ' ' << ny;
                        client.send(edge.str());
                        relative = SDL_SetWindowRelativeMouseMode(window, true);
                        handoffOutside = false;
                    }
                    std::ostringstream message;
                    if (relative)
                        message << "R " << ++pointerSeq << ' '
                                << event.motion.xrel * client.logicalWidth / (client.width * fit)
                                << ' '
                                << event.motion.yrel * client.logicalHeight / (client.height * fit);
                    else {
                        message << "P " << ++pointerSeq << ' ' << nx << ' ' << ny;
                        client.predictCursor(pointerSeq, nx, ny);
                    }
                    client.send(message.str());
                } else if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                           capture) {
                    if (!gameRelative)
                        pointer(event.button.x, event.button.y);
                    unsigned bit = event.button.button == SDL_BUTTON_LEFT     ? 1
                                   : event.button.button == SDL_BUTTON_RIGHT  ? 2
                                   : event.button.button == SDL_BUTTON_MIDDLE ? 4
                                                                              : 0;
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                        buttons |= bit;
                    else
                        buttons &= ~bit;
                    std::ostringstream message;
                    if (relative || gameRelative)
                        message << "G " << buttons;
                    else
                        message << "B " << nx << ' ' << ny << ' ' << buttons;
                    sendEvent(message.str());
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL && capture) {
                    std::ostringstream message;
                    double direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
                    message << "W " << event.wheel.x * 40 * direction << ' '
                            << event.wheel.y * 40 * direction;
                    sendEvent(message.str());
                }
            }
            if (gameRelative && capture && client.connected && client.ack >= gameEpoch &&
                (gameX != sentGameX || gameY != sentGameY)) {
                std::ostringstream motion;
                motion.precision(15);
                motion << "M " << ++pointerSeq << ' ' << gameEpoch << ' ' << gameX << ' ' << gameY;
                const uint64_t sentAt = td::nowNs();
                if (sentAt - lastInputProbe >= 250000000) {
                    lastInputProbe = sentAt;
                    client.inputProbe = sentAt;
                    motion << " PROBE " << sentAt;
                }
                if (desktopGameMotion)
                    client.predictRelativeCursor(pointerSeq, gameX - sentGameX, gameY - sentGameY);
                client.send(motion.str());
                motionPackets++;
                sentGameX = gameX;
                sentGameY = gameY;
            }
            if (capture && client.connected && SDL_GetKeyboardFocus() == window) {
                const int caps = (SDL_GetModState() & SDL_KMOD_CAPS) ? 1 : 0;
                if (caps != sentCaps) {
                    sendEvent("LOCK " + std::to_string(caps));
                    sentCaps = caps;
                    std::cerr << "CapsLock sync: " << (caps ? "on" : "off") << '\n';
                }
            }
            uint64_t now = td::nowNs();
            if (now - lastResend > 20000000) {
                uint64_t ack = client.ack;
                for (auto it = pending.begin(); it != pending.end() && it->first <= ack;)
                    it = pending.erase(it);
                for (auto &entry : pending)
                    client.send(entry.second);
                lastResend = now;
            }
            now = td::nowNs();
            if (now - lastStats > 1000000000) {
                double seconds = (now - lastStats) / 1e9;
                if (motionEvents)
                    std::cerr << "[MouseInput] mode="
                              << (desktopGameMotion ? "desktop predicted" : "game raw")
                              << " events_per_s=" << motionEvents / seconds
                              << " packets_per_s=" << motionPackets / seconds
                              << " event_age_avg_ms="
                              << (eventAgeSamples ? eventAgeTotal / eventAgeSamples : 0)
                              << " event_age_max_ms=" << eventAgeMax << '\n';
                motionEvents = motionPackets = eventAgeSamples = 0;
                eventAgeTotal = eventAgeMax = 0;
                client.displayFPS = (renderer.drawn.load() - statsFrames) / seconds;
                client.displayMbps = (client.receivedBytes - statsBytes) * 8 / seconds / 1e6;
                std::ostringstream title;
                title << "ThunderDisplay · " << int((renderer.drawn.load() - statsFrames) / seconds)
                      << " FPS · " << int((client.receivedBytes - statsBytes) * 8 / seconds / 1e6)
                      << " Mbps · " << keyboard.releaseShortcut
                      << (capture ? " release input" : " capture input");
                const int wait = client.captureWait.load();
                const char *waitTitle = wait == 1   ? "ThunderDisplay · 正在唤醒 Mac"
                                        : wait == 3 ? "ThunderDisplay · 等待 Mac 解锁"
                                                    : "ThunderDisplay · 等待 Mac 显示会话";
                SDL_SetWindowTitle(window, wait ? waitTitle : title.str().c_str());
                std::cerr << title.str() << " · dropped " << client.expiredFrames.load()
                          << " (assembly=" << client.assemblyDrops.load()
                          << " queue=" << client.queueDrops.load()
                          << " display=" << client.displayDrops.load()
                          << ") incomplete_drop=" << client.incompleteFrames.load()
                          << " partial_shown=" << client.partialFrames.load()
                          << " partial_rejected=" << client.partialRejected.load()
                          << " refresh=" << client.refreshRequests.load()
                          << " cursor_draws=" << renderer.cursorDrawn.load()
                          << " input_loop_max_ms=" << inputLoopMax << '\n';
                inputLoopMax = 0;
                statsFrames = renderer.drawn;
                statsBytes = client.receivedBytes;
                lastStats = now;
            }
            // Wake as soon as input arrives, rather than sleeping through an event.
            // Keep the consumed event for the next iteration; maintenance still runs
            // at least once per timeout without a busy-polling input thread.
            hasQueuedEvent = SDL_WaitEventTimeout(&queuedEvent, 1);
        }
        client.send("BYE");
    } catch (...) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        throw;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
