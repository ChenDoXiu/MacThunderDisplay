#import "Bridge.h"
#import "VirtualDisplay.h"
#import <Cocoa/Cocoa.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <IOSurface/IOSurface.h>
#import <Carbon/Carbon.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <IOKit/hidsystem/IOHIDParameter.h>
#include "Transport.hpp"
#include "Cursor.hpp"
#include "DisplayLayout.hpp"
#include "RealtimeThread.hpp"
#include "H264Encoder.hpp"
#include "pyrowave_metal.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <set>
#include <map>
#include <sstream>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <sys/uio.h>
#include <cerrno>
#include <mach/mach_time.h>
#include <time.h>

static std::mutex statusMutex;
static std::string statusText = "准备就绪";

static void status(const std::string &s) {
    std::lock_guard<std::mutex> lock(statusMutex);
    statusText = s;
    fprintf(stderr, "[ThunderDisplay] %s\n", s.c_str());
    fflush(stderr);
}

static CGVirtualDisplay *virtualDisplay;
static std::atomic<int> captureAccess{0};
static std::atomic<bool> running{false}, busy{false}, configured{false}, configPending{false};
static const char *configureDisplay(int, int, int, double, int, int, int, int, int, int);
static void stopDisplay();
static std::string appliedConfig;
static std::mutex configMutex;
static std::string configError, configErrorRequest;
static std::atomic<uint32_t> displayID{0};
static std::atomic<int> listener{-1};
static std::thread controlThread;
static std::mutex connectionMutex;

struct Connection {
    sockaddr_in peer{};
    uint64_t session = 0;
    int udp = -1;
    std::mutex udpSendMutex;
    std::vector<int> extraUDP;
    size_t nextLane = 0;
    std::atomic<uint64_t> cursorAck{0};

    ~Connection() {
        for (int fd : extraUDP)
            td::closeSocket(fd);
        td::closeSocket(udp);
    }
};

static std::shared_ptr<Connection> connection;
static std::atomic<bool> disconnectRequested{false};
static SCStream *stream;
static std::atomic<bool> captureReady{false}, captureFailed{false}, captureNativePacing{false};
static std::atomic<uint64_t> captureAttemptNs{0}, lastCaptureEventNs{0};
static uint64_t captureEpoch = 0; // Main queue owns stream lifecycle and epoch.
static int captureRetina = 1;
static void restartConfiguredCapture();
static void pauseConfiguredCapture();

static void captureFailure(NSError *error, const char *phase) {
    captureReady = false;
    captureFailed = true;
    fprintf(stderr, "[CaptureLifecycle] %s domain=%s code=%ld message=%s\n", phase,
            error.domain.UTF8String, (long)error.code, error.localizedDescription.UTF8String);
    status(error.localizedDescription.UTF8String ?: "采集中断，正在恢复");
}

static bool screenLocked() {
    // This advisory key is not a public lock-state API. Missing keys do not
    // block capture or change authentication; it only improves wait messages.
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return [session[@"CGSSessionScreenIsLocked"] boolValue];
}

struct RemoteDisplayActivity {
    IOPMAssertionID assertion = kIOPMNullAssertionID;
    uint64_t lastWake = 0, lastHello = 0, settleUntil = 0;

    void release() {
        if (assertion != kIOPMNullAssertionID) {
            IOPMAssertionRelease(assertion);
            assertion = kIOPMNullAssertionID;
        }
    }

    bool wake(uint64_t now) {
        if (lastWake && now - lastWake < 5000000000)
            return false;
        lastWake = now;
        IOReturn result = IOPMAssertionDeclareUserActivity(
            CFSTR("ThunderDisplay remote display connection"), kIOPMUserActiveRemote, &assertion);
        settleUntil = now + 700000000;
        fprintf(stderr, "[DisplayWake] remote user activity result=0x%x assertion=%u\n",
                unsigned(result), unsigned(assertion));
        return true;
    }

    ~RemoteDisplayActivity() {
        release();
    }
};

static std::atomic<bool> separateCursor{false};
static dispatch_source_t cursorTimer;

struct EncodedCursor {
    td::CursorImage image;
    bool rle = false;
    std::vector<uint8_t> data;
};

static std::mutex cursorMutex;
static std::shared_ptr<const EncodedCursor> cursorShape;
static uint64_t cursorShapeCounter = 0;
static dispatch_source_t patternTimer;
static CVPixelBufferRef patternBuffer = nullptr;
static id<MTLComputePipelineState> patternPipeline;
static dispatch_queue_t captureQueue, encodeQueue;
static id<MTLDevice> mtl;
static id<MTLCommandQueue> conversionQueue;
static id<MTLComputePipelineState> conversionPipeline;
static id<MTLBuffer> colorTileBuffer;
static MPSImageLanczosScale *resizeKernel;
static MPSImageBilinearScale *bilinearKernel;
static id<MTLTexture> resizeTexture;
static NSUInteger resizeSourceWidth = 0, resizeSourceHeight = 0;
static bool nativeScaling = true;
static bool splitScaling = false, bilinearScaling = false;
static pyrowave_device codecDevice = nullptr;
static pyrowave_encoder encoder = nullptr;
static id<MTLTexture> planeTextures[3];
static int outputWidth = 0, outputHeight = 0, logicalWidth = 0, logicalHeight = 0,
           framesPerSecond = 60, bitrate = 400;
static bool use444 = true, useH264 = false;
static std::unique_ptr<H264Encoder> h264Encoder;
static std::atomic<bool> h264Keyframe{true};
static int linkSpeedMbps = 10000;
static uint64_t frameID = 0;
static std::mutex pixelMutex;
static CVPixelBufferRef latestPixel = nullptr;
static uint64_t latestPixelNs = 0, latestDisplayNs = 0;
static std::mutex pendingMutex;
static CVPixelBufferRef pendingPixel = nullptr;
static uint64_t pendingPixelNs = 0, pendingDisplayNs = 0;

struct Damage {
    bool full = true;
    CGRect bounds = CGRectNull;
    std::vector<CGRect> regions;

    void merge(const Damage &other) {
        full = full || other.full;
        bounds = CGRectUnion(bounds, other.bounds);
        if (!full && regions.size() + other.regions.size() <= 128)
            regions.insert(regions.end(), other.regions.begin(), other.regions.end());
        else {
            full = true;
            regions.clear();
        }
    }
};

static Damage pendingDamage;
// Only touched by the serial encode queue (or while that queue is stopped).
static bool colorCacheValid = false, dirtyEnabled = true;
static uint64_t lastFullRefresh = 0;
static void submitPixel(CVPixelBufferRef pixel, uint64_t timestamp, uint64_t displayNs = 0,
                        Damage damage = {});
static std::atomic<uint64_t> sentFrames{0}, sentBytes{0}, busyFrames{0}, abortedFrames{0},
    capturedFrames{0}, idleFrames{0}, networkDropped{0};

struct PipelineStats {
    uint64_t began = 0, frames = 0, bytes = 0, captured = 0, idle = 0, busy = 0, aborted = 0,
             samples = 0;

    struct SendCPUStats {
        uint64_t frames = 0, calls = 0, packets = 0, bytes = 0;
        double cpuMs = 0;
    };

    std::array<SendCPUStats, 2> sendCPU{};
    double conversion = 0, encoding = 0, packetization = 0, sending = 0, maximum = 0, delivery = 0,
           encodeWait = 0, sendWait = 0, gpu = 0, dirty = 0, dwt = 0, quant = 0, bbox = 0;
    uint64_t reused = 0;
    uint64_t rdoHits = 0, rdoMisses = 0;
    double rdoHitGPU = 0, rdoMissGPU = 0;
    uint64_t deliverySamples = 0, gpuSamples = 0;
    uint64_t paceClamps = 0;
    double paceDebtMax = 0;
};

static PipelineStats pipelineStats;
static uint64_t pipelineSession = 0;

struct EncodedFrame {
    std::shared_ptr<Connection> connection;
    td::Header header;
    std::vector<pyrowave_packet> packets;
    std::vector<uint8_t> data;
    size_t critical = 0;
    uint64_t callbackNs = 0, readyNs = 0, budgetNs = 0;
    int paceMbps = 2000;
    double conversion = 0, encoding = 0, packetization = 0, delivery = -1, encodeWait = 0, gpu = -1,
           dirty = 1, dwt = 1, quant = 1, bbox = 1;
    bool reused = false;
    bool rdoCached = false;
};

static std::mutex sendMutex;
static std::unique_ptr<EncodedFrame> pendingEncoded;
static std::condition_variable senderWake;
static std::thread senderThread;
static bool senderStopping = true, senderAccepting = false;
static void startSender();
static void stopSender();
static void queueEncoded(std::unique_ptr<EncodedFrame> frame);

static int macKey(int usage) {
    static const int letters[] = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
        kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
        kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
        kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z};
    static const int digits[] = {kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5,
                                 kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9, kVK_ANSI_0};
    if (usage >= 4 && usage <= 29)
        return letters[usage - 4];
    if (usage >= 30 && usage <= 39)
        return digits[usage - 30];
    switch (usage) {
    case 40:
        return kVK_Return;
    case 41:
        return kVK_Escape;
    case 42:
        return kVK_Delete;
    case 43:
        return kVK_Tab;
    case 44:
        return kVK_Space;
    case 45:
        return kVK_ANSI_Minus;
    case 46:
        return kVK_ANSI_Equal;
    case 47:
        return kVK_ANSI_LeftBracket;
    case 48:
        return kVK_ANSI_RightBracket;
    case 49:
    case 50:
        return kVK_ANSI_Backslash;
    case 51:
        return kVK_ANSI_Semicolon;
    case 52:
        return kVK_ANSI_Quote;
    case 53:
        return kVK_ANSI_Grave;
    case 54:
        return kVK_ANSI_Comma;
    case 55:
        return kVK_ANSI_Period;
    case 56:
        return kVK_ANSI_Slash;
    case 57:
        return kVK_CapsLock;
    case 58:
        return kVK_F1;
    case 59:
        return kVK_F2;
    case 60:
        return kVK_F3;
    case 61:
        return kVK_F4;
    case 62:
        return kVK_F5;
    case 63:
        return kVK_F6;
    case 64:
        return kVK_F7;
    case 65:
        return kVK_F8;
    case 66:
        return kVK_F9;
    case 67:
        return kVK_F10;
    case 68:
        return kVK_F11;
    case 69:
        return kVK_F12;
    case 73:
        return kVK_Help;
    case 74:
        return kVK_Home;
    case 75:
        return kVK_PageUp;
    case 76:
        return kVK_ForwardDelete;
    case 77:
        return kVK_End;
    case 78:
        return kVK_PageDown;
    case 79:
        return kVK_RightArrow;
    case 80:
        return kVK_LeftArrow;
    case 81:
        return kVK_DownArrow;
    case 82:
        return kVK_UpArrow;
    case 84:
        return kVK_ANSI_KeypadDivide;
    case 85:
        return kVK_ANSI_KeypadMultiply;
    case 86:
        return kVK_ANSI_KeypadMinus;
    case 87:
        return kVK_ANSI_KeypadPlus;
    case 88:
        return kVK_ANSI_KeypadEnter;
    case 89:
        return kVK_ANSI_Keypad1;
    case 90:
        return kVK_ANSI_Keypad2;
    case 91:
        return kVK_ANSI_Keypad3;
    case 92:
        return kVK_ANSI_Keypad4;
    case 93:
        return kVK_ANSI_Keypad5;
    case 94:
        return kVK_ANSI_Keypad6;
    case 95:
        return kVK_ANSI_Keypad7;
    case 96:
        return kVK_ANSI_Keypad8;
    case 97:
        return kVK_ANSI_Keypad9;
    case 98:
        return kVK_ANSI_Keypad0;
    case 99:
        return kVK_ANSI_KeypadDecimal;
    case 224:
        return kVK_Control;
    case 225:
        return kVK_Shift;
    case 226:
        return kVK_Option;
    case 227:
        return kVK_Command;
    case 228:
        return kVK_RightControl;
    case 229:
        return kVK_RightShift;
    case 230:
        return kVK_RightOption;
    case 231:
        return kVK_RightCommand;
    default:
        return -1;
    }
}

struct InputState {
    std::set<int> keys;
    unsigned buttons = 0;
    CGPoint point{};
    bool relativeMode = false;
    uint64_t relativeEpoch = 0, relativeEvents = 0;
    double relativeX = 0, relativeY = 0;
    bool relativePointValid = false;
    uint64_t relativePostNs = 0;
    bool cursorVisibleObserved = false, cursorDrawn = true, cursorQueryLogged = false;
    uint64_t cursorHiddenSince = 0;

    void setRelativeMode(bool enabled, uint64_t epoch = 0) {
        relativeMode = enabled;
        relativeEpoch = enabled ? epoch : 0;
        relativeX = relativeY = 0;
        relativeEvents = 0;
        relativePointValid = false;
        relativePostNs = 0;
        cursorDrawn = true;
        cursorHiddenSince = 0;
        fprintf(stderr, "[Input] Game mouse=%s epoch=%llu\n", enabled ? "on" : "off",
                (unsigned long long)relativeEpoch);
    }

    bool updateCursorVisibility(uint64_t now) {
        // This deprecated query is advisory. An always-false implementation must
        // never hide the desktop cursor; require an observed visible state first.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const bool visible = CGCursorIsVisible();
#pragma clang diagnostic pop
        if (!cursorQueryLogged || (visible && !cursorVisibleObserved)) {
            fprintf(stderr, "[Input] Cursor visibility query=%s calibrated=%d\n",
                    visible ? "visible" : "hidden", visible || cursorVisibleObserved);
            cursorQueryLogged = true;
        }
        if (visible)
            cursorVisibleObserved = true;
        bool drawn = true;
        if (relativeMode && !visible && cursorVisibleObserved) {
            if (!cursorHiddenSince)
                cursorHiddenSince = now;
            drawn = now - cursorHiddenSince < 150000000;
        } else
            cursorHiddenSince = 0;
        if (drawn != cursorDrawn) {
            relativePointValid = false;
            fprintf(stderr, "[Input] Game cursor=%s\n", drawn ? "visible" : "hidden");
        }
        cursorDrawn = drawn;
        return drawn;
    }

    bool relativeMotion(uint64_t epoch, double totalX, double totalY) {
        if (!relativeMode || epoch != relativeEpoch || !std::isfinite(totalX) ||
            !std::isfinite(totalY))
            return false;
        double dx = std::clamp(totalX - relativeX, -4096.0, 4096.0),
               dy = std::clamp(totalY - relativeY, -4096.0, 4096.0);
        relativeX = totalX;
        relativeY = totalY;
        if (dx == 0 && dy == 0)
            return false;
        CGEventRef current = CGEventCreate(nullptr);
        if (!current)
            return false;
        CGPoint location = CGEventGetLocation(current);
        CFRelease(current);
        CGRect bounds = CGDisplayBounds(displayID.load());
        if (bounds.size.width < 1 || bounds.size.height < 1)
            return false;
        const uint64_t now = td::nowNs();
        // CGEventPost queues an event; a subsequent global position query can still
        // describe the previous event. Integrate active desktop motion from our last
        // submitted point, and resync to the native cursor after idle/mode changes.
        const CGPoint base =
            relativePointValid && now - relativePostNs < 50000000 ? point : location;
        point =
            cursorDrawn
                ? CGPointMake(std::clamp(base.x + dx, bounds.origin.x, CGRectGetMaxX(bounds) - 1),
                              std::clamp(base.y + dy, bounds.origin.y, CGRectGetMaxY(bounds) - 1))
                : location;
        CGEventType move = (buttons & 1)   ? kCGEventLeftMouseDragged
                           : (buttons & 2) ? kCGEventRightMouseDragged
                           : (buttons & 4) ? kCGEventOtherMouseDragged
                                           : kCGEventMouseMoved;
        CGEventRef event = CGEventCreateMouseEvent(nullptr, move, point,
                                                   (buttons & 2)   ? kCGMouseButtonRight
                                                   : (buttons & 4) ? kCGMouseButtonCenter
                                                                   : kCGMouseButtonLeft);
        if (!event) {
            relativePointValid = false;
            return false;
        }
        // Games using Cocoa/GLFW consume movement deltas while their cursor is
        // disassociated. Preserve these even when the absolute point hits an edge.
        CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, dx);
        CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, dy);
        if (++relativeEvents == 1)
            fprintf(stderr, "[Input] Game motion epoch=%llu delta=%.2f,%.2f\n",
                    (unsigned long long)epoch,
                    CGEventGetDoubleValueField(event, kCGMouseEventDeltaX),
                    CGEventGetDoubleValueField(event, kCGMouseEventDeltaY));
        CGEventSetFlags(event, flags());
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        relativePostNs = td::nowNs();
        relativePointValid = cursorDrawn;
        return true;
    }

    bool capsLock = (CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) &
                     kCGEventFlagMaskAlphaShift) != 0;
    io_connect_t hidConnection = IO_OBJECT_NULL;
    uint64_t clickTimes[3] = {};
    CGPoint clickPoints[3] = {};
    int clickCounts[3] = {};

    CGEventFlags flags() const {
        CGEventFlags f = capsLock ? kCGEventFlagMaskAlphaShift : 0;
        for (int k : keys) {
            if (k == kVK_Shift || k == kVK_RightShift)
                f |= kCGEventFlagMaskShift;
            if (k == kVK_Control || k == kVK_RightControl)
                f |= kCGEventFlagMaskControl;
            if (k == kVK_Option || k == kVK_RightOption)
                f |= kCGEventFlagMaskAlternate;
            if (k == kVK_Command || k == kVK_RightCommand)
                f |= kCGEventFlagMaskCommand;
        }
        return f;
    }

    void setCapsLock(bool enabled) {
        if (capsLock == enabled)
            return;
        capsLock = enabled;
        if (hidConnection == IO_OBJECT_NULL) {
            io_service_t service = IOServiceGetMatchingService(
                kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
            if (service) {
                IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &hidConnection);
                IOObjectRelease(service);
            }
        }
        kern_return_t result =
            hidConnection != IO_OBJECT_NULL
                ? IOHIDSetModifierLockState(hidConnection, kIOHIDCapsLockState, enabled)
                : kIOReturnNotOpen;
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, kVK_CapsLock, enabled);
        if (e) {
            CGEventSetType(e, kCGEventFlagsChanged);
            CGEventSetFlags(e, flags());
            CGEventPost(kCGHIDEventTap, e);
            CFRelease(e);
        }
        fprintf(stderr, "[Input] CapsLock=%s hid_result=%d\n", enabled ? "on" : "off", int(result));
    }

    void key(int usage, bool down, int caps = -1) {
        if (caps >= 0)
            setCapsLock(caps != 0);
        int k = macKey(usage);
        if (k < 0)
            return;
        bool repeat = false;
        if (down) {
            repeat = !keys.insert(k).second;
        } else if (!keys.erase(k))
            return;
        if (k == kVK_CapsLock) {
            if (caps < 0 && down && !repeat)
                setCapsLock(!capsLock);
            return;
        }
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, k, down);
        if (repeat)
            CGEventSetIntegerValueField(e, kCGKeyboardEventAutorepeat, 1);
        CGEventSetFlags(e, flags());
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    void pointer(double x, double y, unsigned next) {
        if (!std::isfinite(x) || !std::isfinite(y))
            return;
        CGRect bounds = CGDisplayBounds(displayID.load());
        point = CGPointMake(
            bounds.origin.x + std::clamp(x, -16.0, 16.0) * std::max(0.0, bounds.size.width - 1),
            bounds.origin.y + std::clamp(y, -16.0, 16.0) * std::max(0.0, bounds.size.height - 1));
        CGEventType move = (next & 1)   ? kCGEventLeftMouseDragged
                           : (next & 2) ? kCGEventRightMouseDragged
                           : (next & 4) ? kCGEventOtherMouseDragged
                                        : kCGEventMouseMoved;
        CGEventRef e = CGEventCreateMouseEvent(nullptr, move, point,
                                               (next & 2)   ? kCGMouseButtonRight
                                               : (next & 4) ? kCGMouseButtonCenter
                                                            : kCGMouseButtonLeft);
        CGEventSetFlags(e, flags());
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        for (unsigned i = 0; i < 3; i++)
            if ((buttons ^ next) & (1u << i)) {
                CGEventType types[3][2] = {{kCGEventLeftMouseUp, kCGEventLeftMouseDown},
                                           {kCGEventRightMouseUp, kCGEventRightMouseDown},
                                           {kCGEventOtherMouseUp, kCGEventOtherMouseDown}};
                CGMouseButton btns[] = {kCGMouseButtonLeft, kCGMouseButtonRight,
                                        kCGMouseButtonCenter};
                if (next & (1u << i)) {
                    uint64_t now = td::nowNs();
                    double distance =
                        std::hypot(point.x - clickPoints[i].x, point.y - clickPoints[i].y);
                    clickCounts[i] = (now - clickTimes[i] < 400000000 && distance < 5)
                                         ? clickCounts[i] % 3 + 1
                                         : 1;
                    clickTimes[i] = now;
                    clickPoints[i] = point;
                }
                e = CGEventCreateMouseEvent(nullptr, types[i][(next >> i) & 1], point, btns[i]);
                CGEventSetIntegerValueField(e, kCGMouseEventClickState, clickCounts[i]);
                CGEventSetFlags(e, flags());
                CGEventPost(kCGHIDEventTap, e);
                CFRelease(e);
            }
        buttons = next & 7;
    }

    void reset() {
        if (relativeMode)
            setRelativeMode(false);
        for (int k : keys) {
            if (k == kVK_CapsLock)
                continue;
            CGEventRef e = CGEventCreateKeyboardEvent(nullptr, k, false);
            CGEventSetFlags(e, capsLock ? kCGEventFlagMaskAlphaShift : 0);
            CGEventPost(kCGHIDEventTap, e);
            CFRelease(e);
        }
        keys.clear();
        if (buttons) {
            CGRect b = CGDisplayBounds(displayID.load());
            pointer((point.x - b.origin.x) / std::max(1.0, b.size.width - 1),
                    (point.y - b.origin.y) / std::max(1.0, b.size.height - 1), 0);
        }
    }

    ~InputState() {
        reset();
        if (hidConnection != IO_OBJECT_NULL)
            IOServiceClose(hidConnection);
    }
};

static unsigned neighboringEdges() {
    CGRect b = CGDisplayBounds(displayID.load());
    CGDirectDisplayID ids[32];
    uint32_t count = 0;
    unsigned edges = 0;
    if (CGGetActiveDisplayList(32, ids, &count) != kCGErrorSuccess)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i] == displayID.load())
            continue;
        CGRect other = CGDisplayBounds(ids[i]);
        bool vertical = std::min(CGRectGetMaxY(b), CGRectGetMaxY(other)) >
                        std::max(CGRectGetMinY(b), CGRectGetMinY(other));
        bool horizontal = std::min(CGRectGetMaxX(b), CGRectGetMaxX(other)) >
                          std::max(CGRectGetMinX(b), CGRectGetMinX(other));
        if (vertical && std::abs(CGRectGetMinX(b) - CGRectGetMaxX(other)) < 3)
            edges |= 1;
        if (vertical && std::abs(CGRectGetMaxX(b) - CGRectGetMinX(other)) < 3)
            edges |= 2;
        if (horizontal && std::abs(CGRectGetMinY(b) - CGRectGetMaxY(other)) < 3)
            edges |= 4;
        if (horizontal && std::abs(CGRectGetMaxY(b) - CGRectGetMinY(other)) < 3)
            edges |= 8;
    }
    return edges;
}

// AppKit cursor/image work stays on the main queue. Pixel data is canonical
// premultiplied RGBA, sized in stream pixels so the receiver uses video scaling.
static void pollCursorShape() {
    if (!configured || !captureReady || !separateCursor)
        return;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        if (!connection)
            return;
    }
    @autoreleasepool {
        NSCursor *cursor = [NSCursor currentSystemCursor] ?: [NSCursor arrowCursor];
        NSImage *image = cursor.image;
        NSSize size = image.size;
        if (size.width <= 0 || size.height <= 0 || !std::isfinite(size.width) ||
            !std::isfinite(size.height))
            return;
        double scale = double(outputWidth) / std::max(1, logicalWidth);
        scale = std::min(scale, td::MaxCursorSize / std::max(size.width, size.height));
        int w = std::clamp(int(std::ceil(size.width * scale)), 1, int(td::MaxCursorSize)),
            h = std::clamp(int(std::ceil(size.height * scale)), 1, int(td::MaxCursorSize));
        CGImageRef cg = [image CGImageForProposedRect:nullptr context:nil hints:nil];
        if (!cg)
            return;
        auto next = std::make_shared<EncodedCursor>();
        auto &i = next->image;
        i.width = w;
        i.height = h;
        i.hotX = std::clamp(int(std::round(cursor.hotSpot.x * scale)), 0, w - 1);
        i.hotY = std::clamp(int(std::round(cursor.hotSpot.y * scale)), 0, h - 1);
        i.rgba.resize(size_t(w) * h * 4);
        CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context =
            CGBitmapContextCreate(i.rgba.data(), w, h, 8, w * 4, color,
                                  kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(color);
        if (!context)
            return;
        CGContextSetBlendMode(context, kCGBlendModeCopy);
        CGContextDrawImage(context, CGRectMake(0, 0, w, h), cg);
        CGContextRelease(context);
        // Keep the last visible shape when the system temporarily supplies a hidden cursor.
        bool hasVisiblePixel = false;
        for (size_t p = 3; p < i.rgba.size(); p += 4)
            if (i.rgba[p]) {
                hasVisiblePixel = true;
                break;
            }
        if (!hasVisiblePixel)
            return;
        {
            std::lock_guard<std::mutex> lock(cursorMutex);
            if (cursorShape) {
                auto &old = cursorShape->image;
                if (old.width == i.width && old.height == i.height && old.hotX == i.hotX &&
                    old.hotY == i.hotY && old.rgba == i.rgba)
                    return;
            }
        }
        i.id = ++cursorShapeCounter;
        next->data = td::encodeCursor(i.rgba, next->rle);
        fprintf(stderr, "[Cursor] shape=%llu %ux%u hotspot=%u,%u raw=%zu encoded=%zu\n",
                (unsigned long long)i.id, i.width, i.height, i.hotX, i.hotY, i.rgba.size(),
                next->data.size());
        std::lock_guard<std::mutex> lock(cursorMutex);
        cursorShape = std::move(next);
    }
}

static void sendCursorShape(const std::shared_ptr<Connection> &c, const EncodedCursor &shape) {
    std::lock_guard<std::mutex> sendLock(c->udpSendMutex);
    td::Header h;
    h.type = 2;
    h.session = c->session;
    h.frame = shape.image.id;
    h.width = shape.image.width;
    h.height = shape.image.height;
    h.epoch = (uint32_t(shape.image.hotX) << 16) | shape.image.hotY;
    h.flags = shape.rle ? 1 : 0;
    h.units = 1;
    h.unitSize = shape.data.size();
    h.fragments = (h.unitSize + td::ChunkSize - 1) / td::ChunkSize;
    std::array<uint8_t, td::HeaderSize + td::ChunkSize> packet;
    for (unsigned f = 0; f < h.fragments; f++) {
        h.fragment = f;
        h.offset = f * td::ChunkSize;
        h.payload = std::min<size_t>(td::ChunkSize, h.unitSize - h.offset);
        auto head = td::encodeHeader(h);
        memcpy(packet.data(), head.data(), head.size());
        memcpy(packet.data() + td::HeaderSize, shape.data.data() + h.offset, h.payload);
        send(c->udp, packet.data(), td::HeaderSize + h.payload, MSG_DONTWAIT);
    }
}

static void controlLoop(int socket) {
    RemoteDisplayActivity activity;
    bool inputSuspended = false, welcomeNeedsFrame = true;
    uint64_t nextConfigure = 0;
    InputState input;
    uint64_t lastSeen = 0, lastPointer = 0, expectedEvent = 1, lastCursor = 0, cursorSeq = 0,
             lastRefresh = 0, lastShapeSend = 0, sentShapeSession = 0, sentShapeID = 0;
    std::map<uint64_t, std::string> pending;
    sockaddr_in disconnectedPeer{}, lastHelloPeer{};
    uint64_t disconnectedSession = 0;
    auto reply = [&](const sockaddr_in &peer, const std::string &text) {
        sendto(socket, text.data(), text.size(), 0, reinterpret_cast<const sockaddr *>(&peer),
               sizeof(peer));
    };
    auto apply = [&](const std::string &text) {
        std::istringstream msg(text);
        std::string type;
        msg >> type;
        if (!captureReady && !patternTimer) {
            if (type == "RESET")
                input.reset();
            return;
        }
        if (type == "K") {
            int usage, down, caps = -1;
            if (msg >> usage >> down) {
                msg >> caps;
                input.key(usage, down != 0, caps);
            }
        } else if (type == "LOCK") {
            int caps;
            if (msg >> caps && (caps == 0 || caps == 1))
                input.setCapsLock(caps != 0);
        } else if (type == "B") {
            double x, y;
            unsigned buttons;
            if (msg >> x >> y >> buttons)
                input.pointer(x, y, buttons);
        } else if (type == "G") {
            unsigned buttons;
            if (msg >> buttons) {
                CGEventRef event = CGEventCreate(nullptr);
                if (event) {
                    CGPoint point = CGEventGetLocation(event);
                    CFRelease(event);
                    if (input.relativeMode && input.cursorDrawn && input.relativePointValid &&
                        td::nowNs() - input.relativePostNs < 50000000)
                        point = input.point;
                    CGRect b = CGDisplayBounds(displayID.load());
                    input.pointer((point.x - b.origin.x) / std::max(1.0, b.size.width - 1),
                                  (point.y - b.origin.y) / std::max(1.0, b.size.height - 1),
                                  buttons);
                }
            }
        } else if (type == "W") {
            double x, y;
            if (msg >> x >> y && std::isfinite(x) && std::isfinite(y)) {
                CGEventRef e = CGEventCreateScrollWheelEvent(
                    nullptr, kCGScrollEventUnitPixel, 2, int32_t(std::clamp(y, -4096.0, 4096.0)),
                    int32_t(std::clamp(x, -4096.0, 4096.0)));
                CGEventSetFlags(e, input.flags());
                CGEventPost(kCGHIDEventTap, e);
                CFRelease(e);
            }
        } else if (type == "MOUSE_MODE") {
            int enabled;
            uint64_t epoch;
            if (msg >> enabled >> epoch && (enabled == 0 || enabled == 1))
                input.setRelativeMode(enabled != 0, epoch);
        } else if (type == "RESET")
            input.reset();
    };
    while (running) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        char buffer[4096];
        ssize_t n = recvfrom(socket, buffer, sizeof(buffer) - 1, 0,
                             reinterpret_cast<sockaddr *>(&peer), &len);
        uint64_t now = td::nowNs();
        if (!captureReady && !patternTimer) {
            welcomeNeedsFrame = true;
            if (!inputSuspended) {
                input.reset();
                inputSuspended = true;
            }
        }
        if (captureReady || patternTimer)
            inputSuspended = false;
        if (activity.assertion != kIOPMNullAssertionID && activity.lastHello &&
            now - activity.lastHello > 2000000000) {
            activity.release();
            bool hasClient;
            {
                std::lock_guard<std::mutex> lock(connectionMutex);
                hasClient = bool(connection);
            }
            if (!hasClient && configured && !configPending) {
                configPending = true;
                dispatch_async(dispatch_get_main_queue(), ^{
                    pauseConfiguredCapture();
                    configPending = false;
                });
            }
        }
        if (disconnectRequested.exchange(false)) {
            std::shared_ptr<Connection> old;
            {
                std::lock_guard<std::mutex> lock(connectionMutex);
                old = std::move(connection);
            }
            if (old || configured) {
                disconnectedPeer = old ? old->peer : lastHelloPeer;
                disconnectedSession = old ? old->session : 0;
                if (disconnectedPeer.sin_port)
                    for (int repeat = 0; repeat < 3; repeat++)
                        reply(disconnectedPeer,
                              old ? "DISCONNECT " + std::to_string(old->session) : "STOPPED");
                input.reset();
                pending.clear();
                lastSeen = 0;
                activity.release();
                configured = false;
                configPending = true;
                dispatch_async(dispatch_get_main_queue(), ^{
                    stopDisplay();
                    appliedConfig.clear();
                    configPending = false;
                    status("客户端已断开，继续监听 UDP 42420");
                });
            }
            // Discard an already-read input/HELLO datagram from the removed connection.
            continue;
        }
        if (lastSeen && now - lastSeen > 1500000000) {
            input.reset();
            {
                std::lock_guard<std::mutex> lock(connectionMutex);
                connection.reset();
            }
            lastSeen = 0;
            activity.release();
            configPending = true;
            dispatch_async(dispatch_get_main_queue(), ^{
                pauseConfiguredCapture();
                configPending = false;
            });
            status("等待接收端 UDP 连接");
        }
        if (configured && captureReady && now - lastCursor > 8000000) {
            std::shared_ptr<Connection> active;
            {
                std::lock_guard<std::mutex> lock(connectionMutex);
                active = connection;
            }
            if (active) {
                CGEventRef event = CGEventCreate(nullptr);
                if (event) {
                    CGPoint point = CGEventGetLocation(event);
                    CFRelease(event);
                    const bool drawn = input.updateCursorVisibility(now);
                    // Pair the movement sequence ACK with its submitted desktop target. A
                    // native location query may otherwise lag that ACK and undo prediction.
                    if (input.relativeMode && drawn && input.relativePointValid &&
                        now - input.relativePostNs < 50000000)
                        point = input.point;
                    CGRect b = CGDisplayBounds(displayID.load());
                    double x = (point.x - b.origin.x) / std::max(1.0, b.size.width - 1),
                           y = (point.y - b.origin.y) / std::max(1.0, b.size.height - 1);
                    bool inside = CGRectContainsPoint(b, point);
                    std::shared_ptr<const EncodedCursor> shape;
                    {
                        std::lock_guard<std::mutex> lock(cursorMutex);
                        shape = cursorShape;
                    }
                    std::ostringstream cursor;
                    cursor << "CURSOR " << ++cursorSeq << ' ' << x << ' ' << y << ' ' << inside;
                    // Desktop mode stays visible. Relative mode may follow a stable, validated
                    // native hide request so game menus can retain their cursor.
                    if (separateCursor)
                        cursor << ' ' << (shape ? shape->image.id : 0) << ' ' << active->session
                               << ' ' << lastPointer << ' ' << (inside && drawn);
                    auto text = cursor.str();
                    ssize_t sent;
                    {
                        std::lock_guard<std::mutex> sendLock(active->udpSendMutex);
                        sent = send(active->udp, text.data(), text.size(), MSG_DONTWAIT);
                    }
                    if (sent < 0 &&
                        (errno == EPIPE || errno == ENOTCONN || errno == ECONNREFUSED ||
                         errno == ENETDOWN || errno == ENETUNREACH || errno == EHOSTUNREACH)) {
                        std::lock_guard<std::mutex> lock(connectionMutex);
                        if (connection == active)
                            connection.reset();
                    }
                    if (separateCursor && shape && active->cursorAck != shape->image.id &&
                        (sentShapeSession != active->session || sentShapeID != shape->image.id ||
                         now - lastShapeSend >= 100000000)) {
                        sendCursorShape(active, *shape);
                        lastShapeSend = now;
                        sentShapeSession = active->session;
                        sentShapeID = shape->image.id;
                    }
                }
            }
            lastCursor = now;
        }
        if (n <= 0)
            continue;
        buffer[n] = 0;
        std::string line(buffer, n);
        std::shared_ptr<Connection> c;
        {
            std::lock_guard<std::mutex> lock(connectionMutex);
            c = connection;
        }
        bool same = c && c->peer.sin_addr.s_addr == peer.sin_addr.s_addr &&
                    c->peer.sin_port == peer.sin_port;
        if (line.rfind("DISCOVER TDP1 ", 0) == 0) {
            std::istringstream probe(line);
            std::string command, protocol, nonce, extra;
            if (probe >> command >> protocol >> nonce && nonce.size() <= 64 &&
                nonce.find_first_not_of("0123456789abcdefABCDEF-") == std::string::npos &&
                !(probe >> extra)) {
                char name[256] = {};
                gethostname(name, sizeof(name) - 1);
                std::string label = name[0] ? name : "Mac";
                std::replace(label.begin(), label.end(), '\n', ' ');
                std::replace(label.begin(), label.end(), '\r', ' ');
                reply(peer, "OFFER TDP1 " + nonce + " " + label);
            }
            continue;
        }
        if (line.rfind("HELLO ", 0) == 0) {
            if (disconnectedPeer.sin_port &&
                peer.sin_addr.s_addr == disconnectedPeer.sin_addr.s_addr &&
                peer.sin_port == disconnectedPeer.sin_port) {
                reply(peer, disconnectedSession
                                ? "DISCONNECT " + std::to_string(disconnectedSession)
                                : "STOPPED");
                continue;
            }
            if (c && !same) {
                reply(peer, "ERROR Mac is in use");
                continue;
            }
            std::istringstream request(line);
            std::string command;
            int w = 0, h = 0, retina = 0, fps = 0, mbps = 0, chroma = 0;
            double scale = 0;
            request >> command;
            if (!(request >> w >> h >> retina >> scale >> fps >> mbps >> chroma) || w < 128 ||
                h < 128 || w > 8192 || h > 8192 || (w & 1) || (h & 1) ||
                int64_t(w) * h > 33554432 || !std::isfinite(scale) || scale < 0.5 || scale > 4 ||
                fps < 10 || fps > 144 || mbps < 20 || mbps > 3000 || (retina != 0 && retina != 1) ||
                (chroma != 0 && chroma != 1)) {
                reply(peer, "ERROR Invalid display configuration");
                continue;
            }
            std::string cursorCapability;
            request >> cursorCapability;
            int cursorSeparate = cursorCapability == "CURSOR1";
            int requestedLink = 10000;
            std::string option;
            if (request >> option) {
                if (option != "LINK" || !(request >> requestedLink) || requestedLink < 50 ||
                    requestedLink > 40000) {
                    reply(peer, "ERROR Invalid link speed");
                    continue;
                }
            }
            std::vector<uint16_t> videoPorts{ntohs(peer.sin_port)};
            if (request >> option) {
                int lanes = 0;
                if (option != "PORTS" || !(request >> lanes) || lanes < 1 || lanes > 4) {
                    reply(peer, "ERROR Invalid video ports");
                    continue;
                }
                videoPorts.clear();
                bool valid = true;
                for (int i = 0; i < lanes; i++) {
                    int port = 0;
                    if (!(request >> port) || port < 1 || port > 65535 ||
                        std::find(videoPorts.begin(), videoPorts.end(), port) != videoPorts.end()) {
                        valid = false;
                        break;
                    }
                    videoPorts.push_back(uint16_t(port));
                }
                if (!valid || videoPorts.empty() || videoPorts.front() != ntohs(peer.sin_port)) {
                    reply(peer, "ERROR Invalid video ports");
                    continue;
                }
            }
            int requestedCodec = 0;
            if (request >> option) {
                std::string codecName, extra;
                if (option != "CODEC" || !(request >> codecName) ||
                    (codecName != "h264" && codecName != "pyrowave") || (request >> extra)) {
                    reply(peer, "ERROR Invalid codec");
                    continue;
                }
                requestedCodec = codecName == "h264";
            }
            if (requestedCodec) {
                chroma = 0;
                if (w > 4096 || h > 4096 || int64_t(w) * h > 9437184 || mbps > 200) {
                    reply(peer, "ERROR H264 supports up to 4096 per axis, 9 MP and 200 Mbps");
                    continue;
                }
            }
            activity.lastHello = now;
            lastHelloPeer = peer;
            const bool pattern = getenv("THUNDERDISPLAY_PATTERN") != nullptr;
            if (!pattern && (!c || !captureReady)) {
                // Remote framebuffer/GPU clients need this rather than a network-only
                // assertion. Discovery packets and healthy keepalives never wake displays.
                activity.wake(now);
                if (now < activity.settleUntil) {
                    reply(peer, "WAIT WAKE");
                    if (c)
                        lastSeen = now;
                    continue;
                }
            }
            // A reconnect must never bring up the system permission prompt.
            if (!getenv("THUNDERDISPLAY_PATTERN") && !td_screen_permission(0)) {
                if (captureAccess.exchange(-1) != -1)
                    status("等待屏幕录制授权，授权后客户端会自动重连");
                reply(peer, "ERROR Screen recording permission required");
                continue;
            }
            if (configPending) {
                reply(peer, "WAIT CAPTURE");
                if (c)
                    lastSeen = now;
                continue;
            }
            if (!configured || appliedConfig != line) {
                if (now < nextConfigure) {
                    std::string error;
                    {
                        std::lock_guard<std::mutex> lock(configMutex);
                        if (configErrorRequest == line)
                            error = configError;
                    }
                    reply(peer, error.empty() ? (screenLocked() ? "WAIT LOCKED" : "WAIT CAPTURE")
                                              : "ERROR " + error);
                    continue;
                }
                nextConfigure = now + 1500000000;
                input.reset();
                {
                    std::lock_guard<std::mutex> lock(connectionMutex);
                    connection.reset();
                }
                lastSeen = 0;
                configPending = true;
                std::string requested = line;
                dispatch_async(dispatch_get_main_queue(), ^{
                    const char *error =
                        configureDisplay(w, h, retina, scale, fps, mbps, chroma, cursorSeparate,
                                         requestedLink, requestedCodec);
                    {
                        std::lock_guard<std::mutex> lock(configMutex);
                        configError = error ? error : "";
                        configErrorRequest = requested;
                    }
                    if (!error || !*error)
                        appliedConfig = requested;
                    else
                        status(error);
                    configPending = false;
                });
                reply(peer, "WAIT CAPTURE");
                continue;
            }
            if (!pattern && !captureReady) {
                if (c)
                    lastSeen = now;
                const uint64_t attempt = captureAttemptNs.load();
                if ((captureFailed || (attempt && now - attempt > 5000000000)) &&
                    now >= nextConfigure) {
                    nextConfigure = now + 3000000000;
                    configPending = true;
                    input.reset();
                    {
                        std::lock_guard<std::mutex> lock(connectionMutex);
                        connection.reset();
                    }
                    lastSeen = 0;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        restartConfiguredCapture();
                        configPending = false;
                    });
                }
                reply(peer, screenLocked() ? "WAIT LOCKED" : "WAIT CAPTURE");
                continue;
            }
            const uint64_t lastCallback = lastCaptureEventNs.load();
            if (!pattern && lastCallback && now - lastCallback > 5000000000 &&
                (!CGDisplayIsActive(displayID) || CGDisplayIsAsleep(displayID)) &&
                now >= nextConfigure) {
                captureReady = false;
                captureFailed = true;
                nextConfigure = now + 3000000000;
                configPending = true;
                input.reset();
                {
                    std::lock_guard<std::mutex> lock(connectionMutex);
                    connection.reset();
                }
                lastSeen = 0;
                dispatch_async(dispatch_get_main_queue(), ^{
                    restartConfiguredCapture();
                    configPending = false;
                });
                reply(peer, "WAIT CAPTURE");
                if (c)
                    lastSeen = now;
                continue;
            }
            bool firstConnection = !c;
            if (!c) {
                if (useH264)
                    h264Keyframe = true;
                input.reset();
                c = std::make_shared<Connection>();
                c->session = td::random64();
                c->peer = peer;
                c->udp = td::bindUdp();
                if (::connect(c->udp, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer)) <
                    0) {
                    reply(peer, "ERROR Cannot connect video socket");
                    continue;
                }
                td::setSocketTimeout(c->udp, 4);
                bool lanesReady = true;
                for (size_t i = 1; i < videoPorts.size(); i++) {
                    sockaddr_in target = peer;
                    target.sin_port = htons(videoPorts[i]);
                    int fd = -1;
                    try {
                        fd = td::bindUdp();
                    } catch (...) {
                        lanesReady = false;
                        break;
                    }
                    if (::connect(fd, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) <
                        0) {
                        td::closeSocket(fd);
                        lanesReady = false;
                        break;
                    }
                    td::setSocketTimeout(fd, 4);
                    c->extraUDP.push_back(fd);
                }
                if (!lanesReady) {
                    c.reset();
                    reply(peer, "ERROR Cannot connect video lanes");
                    continue;
                }
                fprintf(stderr, "[Transport] video_lanes=%zu send_mode=serialized_batch\n",
                        1 + c->extraUDP.size());
                lastPointer = 0;
                expectedEvent = 1;
                pending.clear();
                {
                    std::lock_guard<std::mutex> lock(connectionMutex);
                    connection = c;
                }
                disconnectedSession = 0;
                disconnectedPeer = {};
                status("接收端已连接，正在串流");
            }
            std::ostringstream out;
            out << "WELCOME " << c->session << ' ' << outputWidth << ' ' << outputHeight << ' '
                << logicalWidth << ' ' << logicalHeight << ' ' << framesPerSecond << ' '
                << (use444 ? 1 : 0) << ' ' << neighboringEdges() << ' ' << (separateCursor ? 1 : 0)
                << " LANES " << 1 + c->extraUDP.size() << " CODEC "
                << (useH264 ? "h264" : "pyrowave");
            reply(peer, out.str());
            if ((firstConnection || welcomeNeedsFrame) && !patternTimer) {
                welcomeNeedsFrame = false;
                std::lock_guard<std::mutex> lock(pixelMutex);
                if (latestPixel)
                    submitPixel(latestPixel, latestPixelNs, latestDisplayNs);
            }
            lastSeen = now;
            continue;
        }
        if (!same)
            continue;
        lastSeen = now;
        std::istringstream msg(line);
        std::string kind;
        msg >> kind;
        if (kind == "SYNC") {
            uint64_t session = 0, t1 = 0;
            if (msg >> session >> t1 && session == c->session) {
                std::ostringstream sync;
                sync << "SYNC " << session << ' ' << t1 << ' ' << now << ' ' << td::nowNs();
                reply(peer, sync.str());
            }
            continue;
        }
        if (kind == "CURSOR_ACK") {
            uint64_t session = 0, id = 0;
            if (msg >> session >> id && session == c->session)
                c->cursorAck = id;
            continue;
        }
        if (kind == "REFRESH") {
            if (useH264)
                h264Keyframe = true;
            if (configured && captureReady && !patternTimer && now - lastRefresh >= 100000000) {
                lastRefresh = now;
                std::lock_guard<std::mutex> lock(pixelMutex);
                if (latestPixel)
                    submitPixel(latestPixel, now, 0);
            }
            continue;
        }
        if ((kind == "M" || kind == "P" || kind == "R") && !captureReady && !patternTimer)
            continue;
        if (kind == "M") {
            uint64_t seq, epoch;
            double x, y;
            if (msg >> seq >> epoch >> x >> y && seq > lastPointer && input.relativeMode &&
                epoch == input.relativeEpoch && std::isfinite(x) && std::isfinite(y)) {
                lastPointer = seq;
                const bool posted = input.relativeMotion(epoch, x, y);
                const uint64_t post = td::nowNs();
                std::string tag;
                uint64_t t1 = 0;
                if (posted && msg >> tag >> t1 && tag == "PROBE" && t1) {
                    std::ostringstream trace;
                    trace << "INPUT_ACK " << c->session << ' ' << seq << ' ' << t1 << ' ' << now
                          << ' ' << post;
                    reply(peer, trace.str());
                }
            }
        } else if (kind == "P" || kind == "R") {
            uint64_t seq;
            double x, y;
            if (msg >> seq >> x >> y && seq > lastPointer && !input.relativeMode &&
                std::isfinite(x) && std::isfinite(y)) {
                lastPointer = seq;
                if (kind == "R") {
                    CGEventRef event = CGEventCreate(nullptr);
                    if (event) {
                        CGPoint point = CGEventGetLocation(event);
                        CFRelease(event);
                        CGRect b = CGDisplayBounds(displayID.load());
                        x = (point.x - b.origin.x + std::clamp(x, -1024.0, 1024.0)) /
                            std::max(1.0, b.size.width - 1);
                        y = (point.y - b.origin.y + std::clamp(y, -1024.0, 1024.0)) /
                            std::max(1.0, b.size.height - 1);
                    }
                }
                input.pointer(x, y, input.buttons);
            }
        } else if (kind == "E") {
            uint64_t seq;
            if (!(msg >> seq))
                continue;
            std::string text;
            std::getline(msg, text);
            if (seq >= expectedEvent && seq < expectedEvent + 256)
                pending.emplace(seq, text);
            while (pending.count(expectedEvent)) {
                apply(pending[expectedEvent]);
                pending.erase(expectedEvent++);
            }
            reply(peer, "ACK " + std::to_string(expectedEvent - 1));
        } else if (kind == "BYE") {
            input.reset();
            {
                std::lock_guard<std::mutex> lock(connectionMutex);
                connection.reset();
            }
            lastSeen = 0;
            activity.release();
            configPending = true;
            dispatch_async(dispatch_get_main_queue(), ^{
                pauseConfiguredCapture();
                configPending = false;
            });
            status("客户端已断开，采集已暂停");
        }
    }
}

static bool prepareCodec() {
    colorCacheValid = false;
    lastFullRefresh = 0;
    const char *dirtyMode = getenv("THUNDERDISPLAY_DIRTY");
    dirtyEnabled = !dirtyMode || strcmp(dirtyMode, "0");
    const char *scaleFilter = getenv("THUNDERDISPLAY_SCALE_FILTER");
    nativeScaling = (use444 || useH264) && (!scaleFilter || strcmp(scaleFilter, "system"));
    splitScaling = scaleFilter && strcmp(scaleFilter, "split") == 0;
    bilinearScaling = scaleFilter && strcmp(scaleFilter, "bilinear") == 0;
    mtl = MTLCreateSystemDefaultDevice();
    if (useH264) {
        conversionQueue = [mtl newCommandQueue];
        h264Keyframe = true;
        h264Encoder =
            std::make_unique<H264Encoder>(mtl, outputWidth, outputHeight, framesPerSecond, bitrate);
        return true;
    }
    pyrowave_device_create_info deviceInfo{};
    deviceInfo.mtl_device = (__bridge void *)mtl;
    if (pyrowave_device_create(&deviceInfo, &codecDevice) != PYROWAVE_SUCCESS) {
        status("无法初始化 PyroWave Metal，请检查 Apple Silicon 支持");
        return false;
    }
    pyrowave_encoder_create_info info{codecDevice, outputWidth, outputHeight,
                                      use444 ? PYROWAVE_CHROMA_SUBSAMPLING_444
                                             : PYROWAVE_CHROMA_SUBSAMPLING_420};
    if (pyrowave_encoder_create(&info, &encoder) != PYROWAVE_SUCCESS) {
        status("无法创建 PyroWave 编码器");
        return false;
    }
    conversionQueue = [mtl newCommandQueue];
    colorTileBuffer =
        [mtl newBufferWithLength:size_t((outputWidth + 15) / 16) * ((outputHeight + 15) / 16) * 8
                         options:MTLResourceStorageModeShared];
    if (!colorTileBuffer) {
        status("无法创建变化区域列表");
        return false;
    }
    if (use444) {
        NSString *source =
            @"#include <metal_stdlib>\nusing namespace metal;\nkernel void convert(texture2d<float,access::read> src [[texture(0)]],texture2d<float,access::write> yp [[texture(1)]],texture2d<float,access::write> up [[texture(2)]],texture2d<float,access::write> vp [[texture(3)]],uint2 q [[thread_position_in_grid]],uint2 group [[threadgroup_position_in_grid]],uint2 local [[thread_position_in_threadgroup]],const device uint2*tiles [[buffer(0)]],constant uint&sparse [[buffer(1)]]){uint2 p=sparse?tiles[group.x]*16u+local:q;if(p.x>=yp.get_width()||p.y>=yp.get_height())return;float3 c=src.read(p).rgb;float y=dot(c,float3(0.2126,0.7152,0.0722));float u=(c.b-y)/1.8556;float v=(c.r-y)/1.5748;yp.write(float4((16.0+219.0*y)/255.0),p);up.write(float4((128.0+224.0*u)/255.0),p);vp.write(float4((128.0+224.0*v)/255.0),p);}";
        NSError *error = nil;
        id<MTLLibrary> lib = [mtl newLibraryWithSource:source options:nil error:&error];
        conversionPipeline =
            [mtl newComputePipelineStateWithFunction:[lib newFunctionWithName:@"convert"]
                                               error:&error];
        if (!conversionPipeline) {
            status(error.localizedDescription.UTF8String);
            return false;
        }
        for (int i = 0; i < 3; i++) {
            MTLTextureDescriptor *d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                   width:outputWidth
                                                                  height:outputHeight
                                                               mipmapped:NO];
            d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            planeTextures[i] = [mtl newTextureWithDescriptor:d];
            if (!planeTextures[i]) {
                status("无法创建 Metal YUV 纹理");
                return false;
            }
        }
    }
    return true;
}

static void sendEncoded(EncodedFrame &frame) {
    if (!captureReady && !patternTimer)
        return;
    auto c = frame.connection;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        if (connection != c)
            return;
    }
    if (pipelineSession != c->session) {
        pipelineSession = c->session;
        sentFrames = 0;
        sentBytes = 0;
        abortedFrames = 0;
        networkDropped = 0;
        pipelineStats = {};
        pipelineStats.began = td::nowNs();
        pipelineStats.captured = capturedFrames;
        pipelineStats.idle = idleFrames;
        pipelineStats.busy = busyFrames;
    }
    auto h = frame.header;
    auto &packets = frame.packets;
    auto &data = frame.data;
    size_t out = packets.size(), critical = frame.critical;

    // Serialize all writers to this connection's UDP sockets. Darwin's connected
    // sendmsg_x packet-list path can report the whole list as sent if sosendcheck
    // fails before resetting its count (e.g. another writer holds SB_LOCK).
    // Wait for the send lock rather than using MSG_DONTWAIT for a batch; socket
    // I/O timeouts and the frame deadline still bound backpressure. Never hold
    // the writer mutex while pacing or retrying. Retain the public fallback.
    struct MessageX {
        void *name;
        socklen_t namelen;
        iovec *iov;
        int iovlen;
        void *control;
        socklen_t controllen;
        int flags;
        size_t datalen;
    };

    using SendBatch = ssize_t (*)(int, const MessageX *, unsigned, int);
    // Diagnostic fallback for comparing syscall overhead without changing video.
    static const SendBatch nativeBatch =
        reinterpret_cast<SendBatch>(dlsym(RTLD_DEFAULT, "sendmsg_x"));
    static const bool forceIndividual = getenv("THUNDERDISPLAY_SEND_INDIVIDUAL") != nullptr,
                      compare = getenv("THUNDERDISPLAY_SEND_COMPARE") != nullptr;
    static const uint64_t compareStart = td::nowNs();
    // An opt-in 30-second diagnostic interleaves adjacent frames, then returns
    // to the default batch path. Normal launches always use batching.
    SendBatch sendBatch =
        (forceIndividual ||
         (compare && td::nowNs() - compareStart < 30000000000ULL && (h.frame & 1)))
            ? nullptr
            : nativeBatch;
    std::array<std::array<uint8_t, td::HeaderSize>, 32> heads;
    std::array<std::array<iovec, 2>, 32> vectors;
    std::array<MessageX, 32> messages{};
    size_t queued = 0;
    const uint64_t began = td::nowNs(), budget = frame.budgetNs;
    bool aborted = false;
    const uint64_t cpuBegan = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID),
                   bytesBegan = sentBytes.load();
    uint64_t calls = 0, datagrams = 0;
    uint64_t paceDue = began;
    const uint64_t catchupNs =
        uint64_t(64) * (td::HeaderSize + td::ChunkSize) * 8000 / uint64_t(frame.paceMbps);
    // A late wake must not turn the remaining frame into a line-rate burst.
    // Permit at most two batches of catch-up credit, independent of lateness.
    auto flush = [&]() {
        size_t offset = 0, wireBytes = 0;
        uint64_t start = td::nowNs();
        size_t lane = c->nextLane++ % (1 + c->extraUDP.size());
        int videoSocket = lane ? c->extraUDP[lane - 1] : c->udp;
        if (start > paceDue) {
            uint64_t debt = start - paceDue;
            pipelineStats.paceDebtMax = std::max(pipelineStats.paceDebtMax, debt / 1e6);
            if (debt > catchupNs) {
                paceDue = start - catchupNs;
                pipelineStats.paceClamps++;
            }
        }
        while (offset < queued) {
            if (!running || !configured || td::nowNs() - began > budget) {
                aborted = true;
                break;
            }
            ssize_t n = -1;
            {
                std::lock_guard<std::mutex> sendLock(c->udpSendMutex);
                if (sendBatch) {
                    calls++;
                    n = sendBatch(videoSocket, messages.data() + offset, unsigned(queued - offset),
                                  0);
                }
                if (!sendBatch || (n < 0 && (errno == ENOSYS || errno == ENOTSUP ||
                                             errno == EINVAL || errno == EMSGSIZE))) {
                    msghdr message{};
                    message.msg_iov = vectors[offset].data();
                    message.msg_iovlen = 2;
                    calls++;
                    n = sendmsg(videoSocket, &message, MSG_DONTWAIT) >= 0 ? 1 : -1;
                }
            }
            if (n < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == EINTR)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            if (n <= 0) {
                int error = errno;
                static unsigned reports = 0;
                if (reports++ < 12)
                    fprintf(stderr, "[Transport] video send failed: errno=%d (%s), batch=%zu\n",
                            error, strerror(error), queued - offset);
                if (error == EPIPE || error == ENOTCONN || error == ECONNREFUSED ||
                    error == EADDRNOTAVAIL || error == ENETDOWN || error == ENETUNREACH ||
                    error == EHOSTUNREACH) {
                    std::lock_guard<std::mutex> lock(connectionMutex);
                    if (connection == c)
                        connection.reset();
                }
                aborted = true;
                break;
            }
            datagrams += uint64_t(n);
            for (ssize_t i = 0; i < n; i++) {
                size_t bytes = td::HeaderSize + vectors[offset + i][1].iov_len;
                sentBytes += bytes;
                wireBytes += bytes;
            }
            offset += size_t(n);
        }
        queued = 0;
        paceDue += uint64_t(wireBytes) * 8000 / uint64_t(frame.paceMbps);
        uint64_t now = td::nowNs();
        if (!aborted && paceDue > now) {
            static mach_timebase_info_data_t timebase = []() {
                mach_timebase_info_data_t t;
                mach_timebase_info(&t);
                return t;
            }();
            uint64_t ticks =
                uint64_t((__uint128_t(paceDue - now) * timebase.denom) / timebase.numer);
            mach_wait_until(mach_absolute_time() + ticks);
        }
    };
    auto queueUnit = [&](size_t i) {
        auto &p = packets[i];
        h.unit = i;
        h.unitSize = p.size;
        h.flags = (h.flags & ~uint16_t(1)) | (i < critical ? 1 : 0);
        h.fragments = (p.size + td::ChunkSize - 1) / td::ChunkSize;
        for (unsigned f = 0; f < h.fragments && !aborted; f++) {
            h.fragment = f;
            h.offset = f * td::ChunkSize;
            h.payload = std::min<size_t>(td::ChunkSize, p.size - h.offset);
            heads[queued] = td::encodeHeader(h);
            vectors[queued][0] = {heads[queued].data(), td::HeaderSize};
            vectors[queued][1] = {data.data() + p.offset + h.offset, h.payload};
            messages[queued] = {nullptr, 0, vectors[queued].data(), 2, nullptr, 0, 0, 0};
            if (++queued == 32)
                flush();
        }
    };
    for (size_t i = 0; i < out && !aborted; i++)
        queueUnit(i);
    // Separate the recovery copy from the initial burst: losing the sequence
    // header or low-frequency bands otherwise makes the whole frame undecodable.
    if (!(h.flags & td::FrameH264))
        for (size_t i = 0; i < std::min(out, std::max<size_t>(1, critical)) && !aborted; i++)
            queueUnit(i);
    if (queued && !aborted)
        flush();
    if (aborted) {
        abortedFrames++;
        if (useH264)
            h264Keyframe = true;
    } else
        sentFrames++;

    uint64_t now = td::nowNs();
    auto &stats = pipelineStats;
    auto &sendCPU = stats.sendCPU[sendBatch ? 1 : 0];
    sendCPU.frames++;
    sendCPU.cpuMs += (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - cpuBegan) / 1e6;
    sendCPU.calls += calls;
    sendCPU.packets += datagrams;
    sendCPU.bytes += sentBytes.load() - bytesBegan;
    if (frame.reused && frame.gpu >= 0) {
        if (frame.rdoCached) {
            stats.rdoHits++;
            stats.rdoHitGPU += frame.gpu;
        } else {
            stats.rdoMisses++;
            stats.rdoMissGPU += frame.gpu;
        }
    }
    stats.samples++;
    stats.dirty += frame.dirty;
    stats.bbox += frame.bbox;
    stats.dwt += frame.dwt;
    stats.quant += frame.quant;
    stats.reused += frame.reused;
    if (frame.gpu >= 0) {
        stats.gpu += frame.gpu;
        stats.gpuSamples++;
    }
    stats.conversion += frame.conversion;
    stats.encoding += frame.encoding;
    stats.packetization += frame.packetization;
    stats.sending += (now - began) / 1e6;
    stats.encodeWait += frame.encodeWait;
    stats.sendWait += (began - frame.readyNs) / 1e6;
    stats.maximum = std::max(stats.maximum, (now - frame.callbackNs) / 1e6);
    if (frame.delivery >= 0) {
        stats.delivery += frame.delivery;
        stats.deliverySamples++;
    }
    if (now - stats.began > 1000000000) {
        double seconds = (now - stats.began) / 1e9;
        auto sent = sentFrames.load(), bytes = sentBytes.load(), captured = capturedFrames.load(),
             idle = idleFrames.load(), busy = busyFrames.load(), aborted = abortedFrames.load();
        std::ostringstream text;
        text << "串流中 · " << int((sent - stats.frames) / seconds) << " FPS · 采集 "
             << int((captured - stats.captured) / seconds) << " FPS · "
             << int((bytes - stats.bytes) * 8 / seconds / 1e6) << " Mbps · 编码积压 " << busy
             << " · 发送积压 " << networkDropped.load() << " · 中断 " << aborted;
        status(text.str());
        fprintf(
            stderr,
            "[Pipeline] capture=%.1f/s idle=%.1f/s busy_drop=%llu network_drop=%llu send_abort=%llu avg_ms capture_delivery=%.2f encode_wait=%.2f convert_record=%.2f gpu_pipeline_wait=%.2f gpu_execute=%.2f packet=%.2f send_wait=%.2f send=%.2f callback_to_send_max=%.2f\n",
            (captured - stats.captured) / seconds, (idle - stats.idle) / seconds,
            (unsigned long long)(busy - stats.busy), (unsigned long long)networkDropped.load(),
            (unsigned long long)(aborted - stats.aborted),
            stats.deliverySamples ? stats.delivery / stats.deliverySamples : -1.0,
            stats.encodeWait / stats.samples, stats.conversion / stats.samples,
            stats.encoding / stats.samples, stats.gpuSamples ? stats.gpu / stats.gpuSamples : -1.0,
            stats.packetization / stats.samples, stats.sendWait / stats.samples,
            stats.sending / stats.samples, stats.maximum);
        for (size_t mode = 0; mode < stats.sendCPU.size(); mode++) {
            auto &cpu = stats.sendCPU[mode];
            if (cpu.frames)
                fprintf(
                    stderr,
                    "[SenderCPU] mode=%s frames=%llu wire_bytes=%llu cpu_ms=%.3f cpu_ms_per_frame=%.3f cpu_us_per_KiB=%.3f syscalls_per_s=%.1f datagrams_per_s=%.1f\n",
                    mode ? "serialized_batch" : "individual", (unsigned long long)cpu.frames,
                    (unsigned long long)cpu.bytes, cpu.cpuMs, cpu.cpuMs / cpu.frames,
                    cpu.bytes ? cpu.cpuMs * 1024000 / cpu.bytes : 0.0, cpu.calls / seconds,
                    cpu.packets / seconds);
        }
        fprintf(stderr,
                "[Dirty] reused=%llu/%llu avg_percent color=%.1f dwt=%.1f quant=%.1f bbox=%.1f\n",
                (unsigned long long)stats.reused, (unsigned long long)stats.samples,
                stats.dirty / stats.samples * 100, stats.dwt / stats.samples * 100,
                stats.quant / stats.samples * 100, stats.bbox / stats.samples * 100);
        fprintf(stderr, "[Pacing] target_mbps=%d catchup_bytes=%zu clamps=%llu debt_max_ms=%.3f\n",
                frame.paceMbps, size_t(64) * (td::HeaderSize + td::ChunkSize),
                (unsigned long long)stats.paceClamps, stats.paceDebtMax);
        if (stats.rdoHits && stats.rdoMisses)
            fprintf(stderr, "[RDOCache] gpu_ms cached=%.3f (%llu frames) full=%.3f (%llu frames)\n",
                    stats.rdoHitGPU / stats.rdoHits, (unsigned long long)stats.rdoHits,
                    stats.rdoMissGPU / stats.rdoMisses, (unsigned long long)stats.rdoMisses);
        stats = {};
        stats.began = now;
        stats.frames = sent;
        stats.bytes = bytes;
        stats.captured = captured;
        stats.idle = idle;
        stats.busy = busy;
        stats.aborted = aborted;
    }
}

// true asks the owning thread to create a fresh QoS worker for fallback.
static bool senderLoop(bool requestRealtime) {
    pthread_setname_np("td-send");
    td::SenderRealtime schedule;
    if (requestRealtime) {
        if (!schedule.enable(framesPerSecond))
            return true;
    } else {
        int result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        uint64_t tid = 0;
        pthread_threadid_np(nullptr, &tid);
        fprintf(stderr,
                "[SenderSchedule] request=user_interactive enabled=%d qos_result=%d tid=%llu\n",
                result == 0, result, (unsigned long long)tid);
    }
    uint64_t checked = td::nowNs(), frames = 0, cpuTotal = 0, cpuMax = 0, overruns = 0;
    unsigned consecutiveOverruns = 0;
    for (;;) {
        std::unique_ptr<EncodedFrame> next;
        {
            std::unique_lock<std::mutex> lock(sendMutex);
            senderWake.wait(lock, [] {
                return senderStopping || pendingEncoded;
            });
            if (senderStopping)
                return false;
            next = std::move(pendingEncoded);
        }
        if (!running || !configured)
            continue;
        uint64_t began = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        @autoreleasepool {
            try {
                sendEncoded(*next);
            } catch (const std::exception &e) {
                status(e.what());
            }
            next.reset();
        }
        uint64_t cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - began;
        frames++;
        cpuTotal += cpu;
        cpuMax = std::max(cpuMax, cpu);
        if (requestRealtime && cpu > schedule.budgetNs) {
            overruns++;
            consecutiveOverruns++;
        } else
            consecutiveOverruns = 0;
        uint64_t now = td::nowNs();
        if (requestRealtime && consecutiveOverruns >= 3) {
            fprintf(
                stderr,
                "[SenderSchedule] fallback=cpu_budget_exceeded consecutive=%u cpu_ms=%.3f budget_ms=%.3f\n",
                consecutiveOverruns, cpu / 1e6, schedule.budgetNs / 1e6);
            return true;
        }
        if (now - checked >= 1000000000ULL) {
            bool realtime = requestRealtime && schedule.stillRealtime();
            fprintf(
                stderr,
                "[SenderSchedule] realtime=%d frames=%llu cpu_avg_ms=%.3f cpu_max_ms=%.3f budget_overruns=%llu\n",
                realtime, (unsigned long long)frames, frames ? cpuTotal / 1e6 / frames : 0.0,
                cpuMax / 1e6, (unsigned long long)overruns);
            if (requestRealtime && !realtime) {
                fprintf(stderr, "[SenderSchedule] fallback=realtime_revoked\n");
                return true;
            }
            checked = now;
            frames = cpuTotal = cpuMax = 0;
        }
    }
}

static void startSender() {
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        senderStopping = false;
        senderAccepting = true;
        pendingEncoded.reset();
    }
    try {
        senderThread = std::thread([] {
            const char *mode = getenv("THUNDERDISPLAY_MAC_REALTIME");
            bool realtime = !mode || strcmp(mode, "0");
            if (senderLoop(realtime)) {
                pthread_setname_np("td-send-retired");
                fprintf(
                    stderr,
                    "[SenderSchedule] starting fresh QoS fallback worker; realtime will not be requested again for this display\n");
                try {
                    std::thread fallback([] {
                        senderLoop(false);
                    });
                    fallback.join();
                } catch (const std::exception &e) {
                    status(e.what());
                    std::lock_guard<std::mutex> lock(sendMutex);
                    senderAccepting = false;
                    pendingEncoded.reset();
                }
            }
        });
    } catch (...) {
        std::lock_guard<std::mutex> lock(sendMutex);
        senderStopping = true;
        senderAccepting = false;
        throw;
    }
}

static void stopSender() {
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        senderAccepting = false;
        senderStopping = true;
        pendingEncoded.reset();
    }
    senderWake.notify_all();
    if (senderThread.joinable())
        senderThread.join();
}

static void queueEncoded(std::unique_ptr<EncodedFrame> frame) {
    if (!running || !configured)
        return;
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        if (!senderAccepting)
            return;
        if (pendingEncoded) {
            networkDropped++;
            if (useH264)
                h264Keyframe = true;
        }
        pendingEncoded = std::move(frame);
    }
    senderWake.notify_one();
}

static void encodeFrame(CVPixelBufferRef pixelBuffer, uint64_t captureNs, uint64_t displayNs = 0,
                        Damage damage = {}) {
    @autoreleasepool {
        try {
            std::shared_ptr<Connection> c;
            {
                std::lock_guard<std::mutex> lock(connectionMutex);
                c = connection;
            }
            if (!c || !running || !configured || (!captureReady && !patternTimer)) {
                colorCacheValid = false;
                return;
            }
            const uint64_t frameBegan = td::nowNs();
            if (useH264) {
                auto encoded = h264Encoder->encode(pixelBuffer, h264Keyframe.exchange(false));
                auto frame = std::make_unique<EncodedFrame>();
                frame->connection = c;
                auto &h = frame->header;
                h.session = c->session;
                h.frame = ++frameID;
                h.captureNs = captureNs;
                h.width = outputWidth;
                h.height = outputHeight;
                h.chroma = 0;
                h.flags = td::FrameH264 | td::FrameTimeValid | td::FrameCallbackTime |
                          (encoded.key ? td::FrameKeyframe : 0);
                frame->data = std::move(encoded.bytes);
                const size_t unitSize = 256 * 1024;
                for (size_t offset = 0; offset < frame->data.size(); offset += unitSize) {
                    pyrowave_packet p{};
                    p.offset = offset;
                    p.size = std::min(unitSize, frame->data.size() - offset);
                    frame->packets.push_back(p);
                }
                h.units = frame->packets.size();
                frame->callbackNs = captureNs;
                frame->readyNs = td::nowNs();
                frame->budgetNs = encoded.key
                                      ? 100000000
                                      : std::max<uint64_t>(25000000, 1500000000 / framesPerSecond);
                frame->paceMbps =
                    std::min(std::max({100, bitrate * 2, int(frame->data.size() * 8000 / 4000000)}),
                             linkSpeedMbps * 9 / 10);
                frame->conversion = encoded.scaleMs;
                frame->encoding = encoded.encodeMs;
                frame->encodeWait = (frameBegan - captureNs) / 1e6;
                queueEncoded(std::move(frame));
                return;
            }
            pyrowave_gpu_input input{};
            IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixelBuffer);
            if (!surface) {
                colorCacheValid = false;
                return;
            }
            id<MTLCommandBuffer> unified = nil;
            pyrowave_dirty_stats dirtyStats{};
            double dirtyFraction = 1, boundingFraction = 1;
            pyrowave_rate_control rate{size_t(uint64_t(bitrate) * 1000000 / (8 * framesPerSecond)) &
                                       ~size_t(3)};
            uint64_t converted = frameBegan;
            pyrowave_result result = PYROWAVE_SUCCESS;
            if (use444) {
                const NSUInteger sourceWidth = CVPixelBufferGetWidth(pixelBuffer),
                                 sourceHeight = CVPixelBufferGetHeight(pixelBuffer);
                MTLTextureDescriptor *d = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                 width:sourceWidth
                                                height:sourceHeight
                                             mipmapped:NO];
                d.usage = MTLTextureUsageShaderRead;
                d.storageMode = MTLStorageModeShared;
                id<MTLTexture> source = [mtl newTextureWithDescriptor:d iosurface:surface plane:0];
                if (!source) {
                    colorCacheValid = false;
                    return;
                }
                const bool resizing = sourceWidth != NSUInteger(outputWidth) ||
                                      sourceHeight != NSUInteger(outputHeight);
                if (sourceWidth != resizeSourceWidth || sourceHeight != resizeSourceHeight) {
                    colorCacheValid = false;
                    resizeSourceWidth = sourceWidth;
                    resizeSourceHeight = sourceHeight;
                    const char *filter = splitScaling      ? "split: left=bilinear right=Lanczos"
                                         : bilinearScaling ? "Metal bilinear"
                                                           : "Metal Lanczos";
                    fprintf(stderr, "[Scaler] %lux%lu -> %dx%d filter=%s\n",
                            (unsigned long)sourceWidth, (unsigned long)sourceHeight, outputWidth,
                            outputHeight, resizing ? filter : "native (no resize)");
                }
                if (resizing && !resizeTexture) {
                    resizeKernel = [[MPSImageLanczosScale alloc] initWithDevice:mtl];
                    resizeKernel.edgeMode = MPSImageEdgeModeClamp;
                    if (splitScaling || bilinearScaling) {
                        bilinearKernel = [[MPSImageBilinearScale alloc] initWithDevice:mtl];
                        bilinearKernel.edgeMode = MPSImageEdgeModeClamp;
                        if (!bilinearKernel)
                            throw std::runtime_error("无法创建 Metal 双线性缩放滤镜");
                    }
                    MTLTextureDescriptor *rd = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                     width:outputWidth
                                                    height:outputHeight
                                                 mipmapped:NO];
                    rd.storageMode = MTLStorageModePrivate;
                    rd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    resizeTexture = [mtl newTextureWithDescriptor:rd];
                    resizeTexture.label = @"ThunderDisplay persistent scaled BGRA";
                    if (!resizeKernel || !resizeTexture)
                        throw std::runtime_error("无法创建 Metal 缩放纹理");
                }
                unified = [conversionQueue commandBuffer];
                unified.label = @"ThunderDisplay resize + color + PyroWave";
                CGRect extent = CGRectMake(0, 0, outputWidth, outputHeight);
                bool regional = dirtyEnabled && colorCacheValid && !damage.full &&
                                !damage.regions.empty() &&
                                frameBegan - lastFullRefresh < 2000000000;

                struct ColorTile {
                    uint32_t x, y;
                };

                std::vector<ColorTile> tiles;
                std::vector<pyrowave_dirty_region> regions;
                const int tw = (outputWidth + 15) / 16, th = (outputHeight + 15) / 16;
                if (regional) {
                    std::vector<uint8_t> mask(size_t(tw) * th);
                    for (auto &r : damage.regions) {
                        CGRect roi =
                            CGRectIntegral(CGRectIntersection(extent, CGRectInset(r, -2, -2)));
                        if (CGRectIsNull(roi) || CGRectIsEmpty(roi))
                            continue;
                        regions.push_back({uint32_t(roi.origin.x), uint32_t(roi.origin.y),
                                           uint32_t(roi.size.width), uint32_t(roi.size.height)});
                        int left = int(roi.origin.x) / 16, top = int(roi.origin.y) / 16,
                            right = (int(CGRectGetMaxX(roi)) + 15) / 16,
                            bottom = (int(CGRectGetMaxY(roi)) + 15) / 16;
                        for (int y = top; y < bottom; y++)
                            for (int x = left; x < right; x++)
                                mask[size_t(y) * tw + x] = 1;
                    }
                    for (int y = 0; y < th; y++)
                        for (int x = 0; x < tw; x++)
                            if (mask[size_t(y) * tw + x])
                                tiles.push_back({uint32_t(x), uint32_t(y)});
                    if (tiles.empty() || tiles.size() > size_t(tw) * th * 0.6)
                        regional = false;
                }
                if (!regional) {
                    regions.clear();
                    lastFullRefresh = frameBegan;
                }
                if (resizing) {
                    // Refresh whole color tiles, including their alignment apron. Other pixels
                    // in the persistent scaled image remain valid from the preceding frame.
                    CGRect roi = extent;
                    if (regional) {
                        roi = CGRectNull;
                        for (auto &t : tiles)
                            roi = CGRectUnion(roi, CGRectMake(t.x * 16, t.y * 16, 16, 16));
                        roi = CGRectIntersection(roi, extent);
                    }
                    auto scaleRegion = [&](MPSImageScale *kernel, CGRect part) {
                        if (CGRectIsNull(part) || CGRectIsEmpty(part))
                            return;
                        kernel.clipRect = MTLRegionMake2D(
                            NSUInteger(part.origin.x), NSUInteger(part.origin.y),
                            NSUInteger(part.size.width), NSUInteger(part.size.height));
                        // Both filters use the same global sampling coordinates. MPS positions
                        // are relative to clipRect, so compensate for each half's origin.
                        MPSScaleTransform transform{double(outputWidth) / sourceWidth,
                                                    double(outputHeight) / sourceHeight,
                                                    -part.origin.x, -part.origin.y};
                        kernel.scaleTransform = &transform;
                        [kernel encodeToCommandBuffer:unified
                                        sourceTexture:source
                                   destinationTexture:resizeTexture];
                    };
                    if (splitScaling) {
                        const int middle = outputWidth / 2;
                        scaleRegion(
                            bilinearKernel,
                            CGRectIntersection(roi, CGRectMake(0, 0, middle, outputHeight)));
                        scaleRegion(
                            resizeKernel,
                            CGRectIntersection(
                                roi, CGRectMake(middle, 0, outputWidth - middle, outputHeight)));
                    } else
                        scaleRegion(
                            bilinearScaling ? (MPSImageScale *)bilinearKernel : resizeKernel, roi);
                    source = resizeTexture;
                }
                id<MTLComputeCommandEncoder> ce = [unified computeCommandEncoder];
                if (!ce)
                    throw std::runtime_error("无法创建转色 command encoder");
                [ce setComputePipelineState:conversionPipeline];
                [ce setTexture:source atIndex:0];
                pyrowave_mtl_texture textures[3];
                for (int i = 0; i < 3; i++) {
                    [ce setTexture:planeTextures[i] atIndex:i + 1];
                    textures[i] = (__bridge void *)planeTextures[i];
                }
                uint32_t sparse = regional;
                [ce setBuffer:colorTileBuffer offset:0 atIndex:0];
                [ce setBytes:&sparse length:sizeof(sparse) atIndex:1];
                if (regional) {
                    memcpy(colorTileBuffer.contents, tiles.data(),
                           tiles.size() * sizeof(ColorTile));
                    [ce dispatchThreadgroups:MTLSizeMake(tiles.size(), 1, 1)
                        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                    dirtyFraction = std::min(1.0, double(tiles.size()) * 256 /
                                                      (double(outputWidth) * outputHeight));
                    CGRect bbox = CGRectIntersection(extent, CGRectInset(damage.bounds, -2, -2));
                    boundingFraction =
                        bbox.size.width * bbox.size.height / (double(outputWidth) * outputHeight);
                } else
                    [ce dispatchThreads:MTLSizeMake(outputWidth, outputHeight, 1)
                        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [ce endEncoding];
                converted = td::nowNs();
                result = pyrowave_encoder_encode_gpu_textures_regions(
                    encoder, textures, (__bridge void *)unified, &rate, regions.data(),
                    regions.size(), &dirtyStats);
                if (result != PYROWAVE_SUCCESS)
                    throw std::runtime_error(pyrowave_result_to_string(result));
                [unified commit];
            } else {
                input.planes[0] = surface;
                converted = td::nowNs();
                result = pyrowave_encoder_encode_gpu(encoder, &input, &rate);
            }
            if (result != PYROWAVE_SUCCESS)
                throw std::runtime_error(pyrowave_result_to_string(result));
            size_t count = 0;
            result = pyrowave_encoder_compute_num_packets(encoder, td::ChunkSize, &count);
            if (result != PYROWAVE_SUCCESS || count > 32768)
                throw std::runtime_error("编码分包失败");
            if (unified.status == MTLCommandBufferStatusError)
                throw std::runtime_error(unified.error.localizedDescription.UTF8String
                                             ?: "Metal 缩放或编码失败");
            const uint64_t encoded = td::nowNs();
            colorCacheValid = use444;
            std::vector<pyrowave_packet> packets(count);
            std::vector<uint8_t> data(rate.maximum_bitstream_size * 2 + count * 16 + 4096);
            size_t out = 0;
            result = pyrowave_encoder_packetize(encoder, packets.data(), td::ChunkSize, &out,
                                                data.data(), data.size());
            if (result != PYROWAVE_SUCCESS)
                throw std::runtime_error("PyroWave packetize failed");
            size_t critical = 0;
            pyrowave_encoder_compute_num_critical_packets(encoder, 2, td::ChunkSize, 0, &critical);
            const uint64_t packetized = td::nowNs();
            td::Header h;
            h.session = c->session;
            h.frame = ++frameID;
            h.captureNs = captureNs;
            h.width = outputWidth;
            h.height = outputHeight;
            h.chroma = use444 ? 1 : 0;
            h.units = out;
            // Native SCK delivery may precede the nominal display/PTS timestamp. Use
            // the actual callback instant consistently instead of a future presentation
            // time, and tell the receiver the measurement basis explicitly.
            if (captureNativePacing) {
                h.flags |= td::FrameTimeValid | td::FrameCallbackTime;
            } else if (displayNs && captureNs >= displayNs && packetized >= displayNs &&
                       packetized - displayNs < 500000000) {
                h.captureNs = displayNs;
                h.flags |= td::FrameTimeValid;
            }
            auto frame = std::make_unique<EncodedFrame>();
            frame->connection = c;
            frame->header = h;
            packets.resize(out);
            frame->packets = std::move(packets);
            frame->data = std::move(data);
            frame->critical = critical;
            frame->callbackNs = captureNs;
            frame->readyNs = packetized;
            frame->budgetNs = uint64_t(std::max(25000000, 1500000000 / framesPerSecond));
            frame->paceMbps = std::min(std::max(2000, bitrate * 2), linkSpeedMbps * 9 / 10);
            frame->dirty = dirtyFraction;
            frame->bbox = boundingFraction;
            frame->reused = dirtyStats.reused;
            frame->rdoCached = dirtyStats.rdo_cached;
            frame->dwt =
                dirtyStats.dwt_total ? double(dirtyStats.dwt_tiles) / dirtyStats.dwt_total : 1;
            frame->quant = dirtyStats.quant_total
                               ? double(dirtyStats.quant_tiles) / dirtyStats.quant_total
                               : 1;
            if (unified.GPUEndTime > unified.GPUStartTime)
                frame->gpu = (unified.GPUEndTime - unified.GPUStartTime) * 1000.0;
            frame->conversion = (converted - frameBegan) / 1e6;
            frame->encoding = (encoded - converted) / 1e6;
            frame->packetization = (packetized - encoded) / 1e6;
            frame->encodeWait = (frameBegan - captureNs) / 1e6;
            if (displayNs && captureNs >= displayNs)
                frame->delivery = (captureNs - displayNs) / 1e6;
            queueEncoded(std::move(frame));
        } catch (const std::exception &e) {
            colorCacheValid = false;
            if (useH264)
                h264Keyframe = true;
            status(e.what());
        }
    }
}

static void submitPixel(CVPixelBufferRef pixel, uint64_t timestamp, uint64_t displayNs,
                        Damage damage) {
    if (!running || !configured || !encodeQueue)
        return;
    std::lock_guard<std::mutex> lock(pendingMutex);
    CVPixelBufferRetain(pixel);
    if (pendingPixel) {
        damage.merge(pendingDamage);
        CVPixelBufferRelease(pendingPixel);
        busyFrames++;
    }
    pendingDamage = damage;
    pendingPixel = pixel;
    pendingPixelNs = timestamp;
    pendingDisplayNs = displayNs;
    if (busy.exchange(true))
        return;
    dispatch_async(encodeQueue, ^{
        for (;;) {
            CVPixelBufferRef next = nullptr;
            uint64_t timestamp = 0, displayNs = 0;
            Damage damage;
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                if (!running || !configured || !pendingPixel) {
                    if (pendingPixel) {
                        CVPixelBufferRelease(pendingPixel);
                        pendingPixel = nullptr;
                    }
                    busy = false;
                    return;
                }
                next = pendingPixel;
                damage = pendingDamage;
                timestamp = pendingPixelNs;
                displayNs = pendingDisplayNs;
                pendingPixel = nullptr;
            }
            encodeFrame(next, timestamp, displayNs, damage);
            CVPixelBufferRelease(next);
        }
    });
}

// Owned by one SCStream output on its serial capture queue. Measure source
// timestamps separately from delivery so scheduling jitter is not called loss.
struct CaptureCadence {
    uint64_t began = 0, lastCallback = 0, lastDisplay = 0, complete = 0, idle = 0, other = 0,
             callbackIntervals = 0, displayIntervals = 0, longDisplayGaps = 0;
    double callbackSum = 0, callbackMin = 1e9, callbackMax = 0, displaySum = 0, displayMin = 1e9,
           displayMax = 0;

    void record(NSDictionary *info, uint64_t now, CMSampleBufferRef sample) {
        if (!began)
            began = now;
        const NSInteger state = [info[SCStreamFrameInfoStatus] integerValue];
        if (state == SCFrameStatusComplete) {
            complete++;
            if (lastCallback && now > lastCallback) {
                double ms = (now - lastCallback) / 1e6;
                callbackIntervals++;
                callbackSum += ms;
                callbackMin = std::min(callbackMin, ms);
                callbackMax = std::max(callbackMax, ms);
            }
            lastCallback = now;
            uint64_t ticks = [info[SCStreamFrameInfoDisplayTime] unsignedLongLongValue];
            if (lastDisplay && ticks > lastDisplay) {
                static mach_timebase_info_data_t base = []() {
                    mach_timebase_info_data_t b;
                    mach_timebase_info(&b);
                    return b;
                }();
                double ms = double(ticks - lastDisplay) * base.numer / base.denom / 1e6;
                displayIntervals++;
                displaySum += ms;
                displayMin = std::min(displayMin, ms);
                displayMax = std::max(displayMax, ms);
                if (ms > 1500.0 / framesPerSecond)
                    longDisplayGaps++;
            }
            if (ticks)
                lastDisplay = ticks;
        } else if (state == SCFrameStatusIdle) {
            idle++;
            lastCallback = 0;
            lastDisplay = 0;
        } else
            other++;
        if (now - began >= 5000000000ULL) {
            double sec = (now - began) / 1e9;
            static mach_timebase_info_data_t base = []() {
                mach_timebase_info_data_t b;
                mach_timebase_info(&b);
                return b;
            }();
            uint64_t ticks = [info[SCStreamFrameInfoDisplayTime] unsignedLongLongValue],
                     host = mach_absolute_time();
            double lead = double(int64_t(ticks - host)) * base.numer / base.denom / 1e6;
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample),
                   hostTime = CMClockGetTime(CMClockGetHostTimeClock());
            fprintf(
                stderr,
                "[CaptureClock] display_minus_now_ms=%.3f pts_minus_now_ms=%.3f duration_ms=%.3f\n",
                lead, CMTimeGetSeconds(CMTimeSubtract(pts, hostTime)) * 1000,
                CMTimeGetSeconds(CMSampleBufferGetDuration(sample)) * 1000);
            fprintf(
                stderr,
                "[CaptureCadence] complete_hz=%.3f idle_hz=%.3f other=%llu callback_ms=%.3f/%.3f/%.3f source_ms=%.3f/%.3f/%.3f source_gaps_gt_1_5_frames=%llu\n",
                complete / sec, idle / sec, (unsigned long long)other,
                callbackIntervals ? callbackMin : 0,
                callbackIntervals ? callbackSum / callbackIntervals : 0, callbackMax,
                displayIntervals ? displayMin : 0,
                displayIntervals ? displaySum / displayIntervals : 0, displayMax,
                (unsigned long long)longDisplayGaps);
            began = now;
            complete = idle = other = callbackIntervals = displayIntervals = longDisplayGaps = 0;
            callbackSum = displaySum = callbackMax = displayMax = 0;
            callbackMin = displayMin = 1e9;
        }
    }
};

@interface CaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
    CaptureCadence cadence;
}
@end
@implementation CaptureOutput

- (void)stream:(SCStream *)s
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !running || !configured || s != stream)
        return;
    lastCaptureEventNs = td::nowNs();
    NSArray *attachments =
        (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments.count)
        return;
    cadence.record(attachments[0], lastCaptureEventNs.load(), sample);
    NSNumber *state = attachments[0][SCStreamFrameInfoStatus];
    if (state.integerValue == SCFrameStatusIdle) {
        idleFrames++;
        return;
    }
    if (state.integerValue == SCFrameStatusBlank || state.integerValue == SCFrameStatusSuspended ||
        state.integerValue == SCFrameStatusStopped) {
        if (captureReady.exchange(false))
            fprintf(stderr, "[CaptureLifecycle] frame updates unavailable status=%ld\n",
                    (long)state.integerValue);
        if (state.integerValue == SCFrameStatusStopped) {
            captureFailed = false;
            captureAttemptNs = td::nowNs();
        } // Allow the delegate to classify user vs system stop.
        return;
    }
    if (state.integerValue != SCFrameStatusComplete)
        return;
    capturedFrames++;
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    if (!pixel)
        return;
    uint64_t timestamp = td::nowNs(), displayNs = 0;
    const bool firstComplete = !captureReady.exchange(true);
    captureFailed = false;
    if (firstComplete)
        fprintf(stderr, "[CaptureLifecycle] first complete frame after start/resume\n");
    Damage damage;
    id rects = attachments[0][SCStreamFrameInfoDirtyRects];
    if ([rects isKindOfClass:[NSArray class]] && [rects count]) {
        damage.full = [rects count] > 128;
        for (id value in rects) {
            CGRect r = CGRectNull;
            if ([value isKindOfClass:[NSValue class]] &&
                strcmp([value objCType], @encode(CGRect)) == 0)
                r = [value rectValue];
            else if ([value isKindOfClass:[NSDictionary class]])
                CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)value, &r);
            if (CGRectIsNull(r) || CGRectIsInfinite(r) || !std::isfinite(r.origin.x) ||
                !std::isfinite(r.origin.y) || !std::isfinite(r.size.width) ||
                !std::isfinite(r.size.height)) {
                damage.full = true;
                break;
            }
            damage.bounds = CGRectUnion(damage.bounds, r);
            if (!damage.full && !CGRectIsEmpty(r))
                damage.regions.push_back(r);
        }
        const double sourceWidth = CVPixelBufferGetWidth(pixel),
                     sourceHeight = CVPixelBufferGetHeight(pixel);
        if (CGRectIsEmpty(damage.bounds) || CGRectIsNull(damage.bounds) ||
            !CGRectContainsRect(CGRectMake(0, 0, sourceWidth, sourceHeight), damage.bounds))
            damage.full = true;
        if (!damage.full && (sourceWidth != outputWidth || sourceHeight != outputHeight)) {
            // A changed source pixel affects nearby Lanczos samples. Expand before
            // transforming damage, so cached color/DWT tiles also erase old edges.
            const double sx = outputWidth / sourceWidth, sy = outputHeight / sourceHeight;
            damage.bounds = CGRectNull;
            CGRect extent = CGRectMake(0, 0, outputWidth, outputHeight);
            for (auto &r : damage.regions) {
                r = CGRectInset(r, -16, -16);
                r = CGRectIntegral(
                    CGRectIntersection(extent, CGRectMake(r.origin.x * sx, r.origin.y * sy,
                                                          r.size.width * sx, r.size.height * sy)));
                damage.bounds = CGRectUnion(damage.bounds, r);
            }
        }
    }
    if (firstComplete)
        damage.full = true;
    static uint64_t lastDamageLog = 0;
    if (timestamp - lastDamageLog > 2000000000) {
        lastDamageLog = timestamp;
        fprintf(
            stderr,
            "[CaptureDamage] rects=%lu bounds=%.0f,%.0f %.0fx%.0f output=%zux%zu contentScale=%.3f scaleFactor=%.3f full=%d\n",
            [rects isKindOfClass:[NSArray class]] ? (unsigned long)[rects count] : 0,
            damage.bounds.origin.x, damage.bounds.origin.y, damage.bounds.size.width,
            damage.bounds.size.height, CVPixelBufferGetWidth(pixel), CVPixelBufferGetHeight(pixel),
            [attachments[0][SCStreamFrameInfoContentScale] doubleValue],
            [attachments[0][SCStreamFrameInfoScaleFactor] doubleValue], damage.full);
    }
    NSNumber *displayTime = attachments[0][SCStreamFrameInfoDisplayTime];
    uint64_t hostSampleNs = td::nowNs(), hostNow = mach_absolute_time(),
             displayTicks = displayTime.unsignedLongLongValue;
    if (displayTicks && displayTicks <= hostNow) {
        static mach_timebase_info_data_t timebase = []() {
            mach_timebase_info_data_t info;
            mach_timebase_info(&info);
            return info;
        }();
        uint64_t delay =
            uint64_t((__uint128_t(hostNow - displayTicks) * timebase.numer) / timebase.denom);
        if (delay < 1000000000 && delay < hostSampleNs)
            displayNs = hostSampleNs - delay;
    }
    {
        std::lock_guard<std::mutex> lock(pixelMutex);
        CVPixelBufferRetain(pixel);
        if (latestPixel)
            CVPixelBufferRelease(latestPixel);
        latestPixel = pixel;
        latestPixelNs = timestamp;
        latestDisplayNs = displayNs;
    }
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        if (!connection)
            return;
    }
    submitPixel(pixel, timestamp, displayNs, damage);
}

- (void)stream:(SCStream *)s didStopWithError:(NSError *)error {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s != stream || !configured)
            return;
        if ([error.domain isEqualToString:SCStreamErrorDomain] &&
            error.code == SCStreamErrorUserStopped) {
            captureReady = false;
            disconnectRequested = true;
            fprintf(stderr,
                    "[CaptureLifecycle] system Stop Sharing requested by user; disconnecting\n");
            return;
        }
        captureFailure(error, "stream stopped");
    });
}

@end
static CaptureOutput *captureOutput;

extern "C" const char *td_status() {
    thread_local std::string result;
    std::lock_guard<std::mutex> lock(statusMutex);
    result = statusText;
    return result.c_str();
}

extern "C" uint32_t td_display_id() {
    return displayID;
}

extern "C" int td_screen_permission(int request) {
    if (CGPreflightScreenCaptureAccess())
        captureAccess = 1;
    else if (request)
        captureAccess = CGRequestScreenCaptureAccess() ? 1 : -1;
    return captureAccess == 1;
}

extern "C" int td_input_permission(int request) {
    bool canPost = CGPreflightPostEventAccess();
    NSDictionary *opts = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt : @(request != 0)};
    bool trusted = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
    return canPost || trusted;
}

// WindowServer may restore an earlier resolution for this virtual display identity.
// Select both the requested logical size and Retina backing before starting capture.
static bool selectDisplayMode(CGDirectDisplayID display, int width, int height, int retina) {
    NSDictionary *options =
        @{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes : @YES};
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, (__bridge CFDictionaryRef)options);
    bool selected = false;
    if (modes) {
        for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
            CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            if (CGDisplayModeGetWidth(mode) != size_t(width) ||
                CGDisplayModeGetHeight(mode) != size_t(height) ||
                CGDisplayModeGetPixelWidth(mode) != size_t(width * (retina ? 2 : 1)) ||
                CGDisplayModeGetPixelHeight(mode) != size_t(height * (retina ? 2 : 1)))
                continue;
            CGError result = CGDisplaySetDisplayMode(display, mode, nullptr);
            fprintf(stderr, "[DisplayMode] request=%dx%d retina=%d result=%d\n", width, height,
                    retina, int(result));
            selected = result == kCGErrorSuccess;
            break;
        }
        CFRelease(modes);
    }
    return selected;
}

static void startConfiguredCapture(uint32_t wanted, int requestedWidth, int requestedHeight,
                                   int retina, uint64_t epoch, unsigned attempt = 0) {
    if (!running || !configured || displayID != wanted || captureEpoch != epoch)
        return;
    if (!selectDisplayMode(wanted, requestedWidth, requestedHeight, retina)) {
        if (attempt < 12) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250000000), dispatch_get_main_queue(),
                           ^{
                               startConfiguredCapture(wanted, requestedWidth, requestedHeight,
                                                      retina, epoch, attempt + 1);
                           });
            return;
        }
        captureReady = false;
        captureFailed = true;
        status("无法切换显示模式，等待显示会话恢复");
        return;
    }
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(wanted);
    int captureWidth = outputWidth, captureHeight = outputHeight;
    double captureRefresh = 0;
    if (mode) {
        captureRefresh = CGDisplayModeGetRefreshRate(mode);
        if (nativeScaling && CGDisplayModeGetPixelWidth(mode) >= size_t(outputWidth) &&
            CGDisplayModeGetPixelHeight(mode) >= size_t(outputHeight)) {
            captureWidth = int(CGDisplayModeGetPixelWidth(mode));
            captureHeight = int(CGDisplayModeGetPixelHeight(mode));
        }
        fprintf(
            stderr,
            "[DisplayMode] active logical=%zux%zu backing=%zux%zu output=%dx%d capture=%dx%d refresh=%.3f requested_fps=%d\n",
            CGDisplayModeGetWidth(mode), CGDisplayModeGetHeight(mode),
            CGDisplayModeGetPixelWidth(mode), CGDisplayModeGetPixelHeight(mode), outputWidth,
            outputHeight, captureWidth, captureHeight, CGDisplayModeGetRefreshRate(mode),
            framesPerSecond);
        CGDisplayModeRelease(mode);
    }
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:NO
                                 completionHandler:^(SCShareableContent *content, NSError *error) {
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                         if (!running || !configured || displayID != wanted ||
                                             captureEpoch != epoch)
                                             return;
                                         if (error) {
                                             captureFailure(error, "enumerate displays");
                                             return;
                                         }
                                         SCDisplay *chosen = nil;
                                         for (SCDisplay *d in content.displays)
                                             if (d.displayID == wanted) {
                                                 chosen = d;
                                                 break;
                                             }
                                         if (!chosen) {
                                             captureFailed = true;
                                             status("等待系统恢复虚拟屏幕");
                                             return;
                                         }
                                         CGRect actual = CGDisplayBounds(wanted);
                                         logicalWidth = int(actual.size.width);
                                         logicalHeight = int(actual.size.height);
                                         SCContentFilter *filter =
                                             [[SCContentFilter alloc] initWithDisplay:chosen
                                                                     excludingWindows:@[]];
                                         SCStreamConfiguration *config =
                                             [SCStreamConfiguration new];
                                         config.width = captureWidth;
                                         config.height = captureHeight;
                                         config.captureResolution = SCCaptureResolutionBest;
                                         const char *capturePacing =
                                             getenv("THUNDERDISPLAY_CAPTURE_PACING");
                                         // The virtual display already supplies the requested cadence. A second
                                         // exact 1/fps gate can periodically reject a refresh near its boundary.
                                         // Follow that source clock directly; retain throttling if its actual rate
                                         // differs or cannot be determined. Environment override is diagnostic only.
                                         bool nativePacing =
                                             captureRefresh > 0 &&
                                             std::abs(captureRefresh - framesPerSecond) < 0.01;
                                         if (capturePacing) {
                                             if (!strcmp(capturePacing, "native"))
                                                 nativePacing = true;
                                             else if (!strcmp(capturePacing, "limited"))
                                                 nativePacing = false;
                                         }
                                         captureNativePacing = nativePacing;
                                         config.minimumFrameInterval =
                                             nativePacing ? kCMTimeZero
                                                          : CMTimeMake(1, framesPerSecond);
                                         config.queueDepth = 3;
                                         fprintf(
                                             stderr,
                                             "[CaptureConfig] pacing=%s minimum_interval_ms=%.6f queue_depth=%ld\n",
                                             nativePacing ? "native" : "limited",
                                             CMTimeGetSeconds(config.minimumFrameInterval) * 1000,
                                             (long)config.queueDepth);
                                         config.pixelFormat =
                                             (use444 || useH264)
                                                 ? kCVPixelFormatType_32BGRA
                                                 : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
                                         config.colorSpaceName = kCGColorSpaceSRGB;
                                         config.colorMatrix =
                                             kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
                                         config.showsCursor = !separateCursor;
                                         config.scalesToFit = YES;
                                         config.capturesAudio = NO;
                                         captureOutput = [CaptureOutput new];
                                         SCStream *newStream =
                                             [[SCStream alloc] initWithFilter:filter
                                                                configuration:config
                                                                     delegate:captureOutput];
                                         NSError *err = nil;
                                         if (![newStream addStreamOutput:captureOutput
                                                                    type:SCStreamOutputTypeScreen
                                                      sampleHandlerQueue:captureQueue
                                                                   error:&err]) {
                                             captureFailure(err, "add capture output");
                                             return;
                                         }
                                         stream = newStream;
                                         [newStream startCaptureWithCompletionHandler:^(
                                                        NSError *error) {
                                             dispatch_async(dispatch_get_main_queue(), ^{
                                                 if (!running || !configured ||
                                                     captureEpoch != epoch || stream != newStream)
                                                     return;
                                                 if (error) {
                                                     captureFailure(error, "start capture");
                                                     return;
                                                 }
                                                 fprintf(
                                                     stderr,
                                                     "[CaptureLifecycle] stream started, awaiting complete frame\n");
                                             });
                                         }];
                                     });
                                 }];
}

static void pauseConfiguredCapture() {
    if (!configured || patternTimer)
        return;
    captureReady = false;
    captureFailed = true;
    captureAttemptNs = 0;
    lastCaptureEventNs = 0;
    ++captureEpoch;
    SCStream *old = stream;
    stream = nil;
    if (old)
        [old stopCaptureWithCompletionHandler:^(NSError *error) {
            if (error)
                fprintf(stderr, "[CaptureLifecycle] pause stop code=%ld\n", (long)error.code);
        }];
    {
        std::lock_guard<std::mutex> lock(pixelMutex);
        if (latestPixel) {
            CVPixelBufferRelease(latestPixel);
            latestPixel = nullptr;
        }
    }
    fprintf(stderr, "[CaptureLifecycle] paused capture: no active client\n");
}

static void restartConfiguredCapture() {
    if (!running || !configured || patternTimer)
        return;
    if (!CGDisplayIsOnline(displayID)) {
        configured = false;
        captureReady = false;
        captureFailed = true;
        status("虚拟屏已离线，等待重新创建");
        return;
    }
    captureReady = false;
    captureFailed = false;
    captureAttemptNs = td::nowNs();
    lastCaptureEventNs = 0;
    const uint64_t epoch = ++captureEpoch;
    const uint32_t wanted = displayID.load();
    const int width = logicalWidth, height = logicalHeight, retina = captureRetina;
    SCStream *old = stream;
    stream = nil;
    {
        std::lock_guard<std::mutex> lock(pixelMutex);
        if (latestPixel) {
            CVPixelBufferRelease(latestPixel);
            latestPixel = nullptr;
        }
    }
    fprintf(stderr, "[CaptureLifecycle] restarting capture for display=%u epoch=%llu\n", wanted,
            (unsigned long long)epoch);
    auto begin = ^{
        dispatch_async(dispatch_get_main_queue(), ^{
            startConfiguredCapture(wanted, width, height, retina, epoch);
        });
    };
    if (old)
        [old stopCaptureWithCompletionHandler:^(NSError *) {
            begin();
        }];
    else
        begin();
}

static const char *configureDisplay(int width, int height, int retina, double scale, int fps,
                                    int mbps, int chroma444, int cursorSeparate, int requestedLink,
                                    int requestedCodec) {
    stopDisplay();
    separateCursor = cursorSeparate != 0;
    if (width < 128 || height < 128 || width > 8192 || height > 8192 ||
        int64_t(width) * height > 33554432 || !std::isfinite(scale) || scale < 0.5 || scale > 4 ||
        fps < 10 || fps > 144 || mbps < 20 || mbps > 3000)
        return "分辨率、缩放、帧率或码率超出支持范围";
    linkSpeedMbps = requestedLink;
    useH264 = requestedCodec == 1;
    bool pattern = getenv("THUNDERDISPLAY_PATTERN") != nullptr;
    if (!NSClassFromString(@"CGVirtualDisplay"))
        return "此 macOS 不支持 CGVirtualDisplay";
    outputWidth = width & ~1;
    outputHeight = height & ~1;
    logicalWidth = td::logicalDisplaySize(outputWidth, scale);
    logicalHeight = td::logicalDisplaySize(outputHeight, scale);
    framesPerSecond = fps;
    bitrate = mbps;
    use444 = !useH264 && (chroma444 || pattern);
    sentFrames = 0;
    sentBytes = 0;
    busyFrames = 0;
    abortedFrames = 0;
    capturedFrames = 0;
    idleFrames = 0;
    networkDropped = 0;
    pipelineStats = {};
    pipelineStats.began = td::nowNs();
    @try {
        CGVirtualDisplayDescriptor *d = [CGVirtualDisplayDescriptor new];
        d.queue = dispatch_get_main_queue();
        d.name = @"ThunderDisplay";
        d.vendorID = 0x5444;
        d.productID = 0x0001;
        d.serialNum = 1;
        d.maxPixelsWide = std::max(outputWidth, logicalWidth * (retina ? 2 : 1));
        d.maxPixelsHigh = std::max(outputHeight, logicalHeight * (retina ? 2 : 1));
        d.sizeInMillimeters = CGSizeMake(340, 212.5);
        d.redPrimary = CGPointMake(0.64, 0.33);
        d.greenPrimary = CGPointMake(0.30, 0.60);
        d.bluePrimary = CGPointMake(0.15, 0.06);
        d.whitePoint = CGPointMake(0.3127, 0.3290);
        virtualDisplay = [[CGVirtualDisplay alloc] initWithDescriptor:d];
        CGVirtualDisplaySettings *settings = [CGVirtualDisplaySettings new];
        settings.hiDPI = retina ? 1 : 0;
        settings.modes = @[ [[CGVirtualDisplayMode alloc] initWithWidth:logicalWidth
                                                                 height:logicalHeight
                                                            refreshRate:fps] ];
        if (!virtualDisplay || ![virtualDisplay applySettings:settings]) {
            virtualDisplay = nil;
            return "虚拟显示器创建失败";
        }
        displayID = virtualDisplay.displayID;
        CGDisplayModeRef actualMode = CGDisplayCopyDisplayMode(displayID);
        if (actualMode) {
            fprintf(
                stderr,
                "[ThunderDisplay] Virtual display %u: logical %zux%zu, backing %zux%zu, output %dx%d\n",
                displayID.load(), CGDisplayModeGetWidth(actualMode),
                CGDisplayModeGetHeight(actualMode), CGDisplayModeGetPixelWidth(actualMode),
                CGDisplayModeGetPixelHeight(actualMode), outputWidth, outputHeight);
            CGDisplayModeRelease(actualMode);
        }
    } @catch (NSException *e) {
        status(e.reason.UTF8String);
        virtualDisplay = nil;
        return td_status();
    }
    try {
        if (!prepareCodec()) {
            stopDisplay();
            return td_status();
        }
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        captureQueue = dispatch_queue_create("display.capture", attr);
        encodeQueue = dispatch_queue_create("display.encode", attr);
        configured = true;
        startSender();
        if (separateCursor) {
            cursorTimer =
                dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
            dispatch_source_set_timer(cursorTimer, DISPATCH_TIME_NOW, 33333333, 1000000);
            dispatch_source_set_event_handler(cursorTimer, ^{
                pollCursorShape();
            });
            dispatch_resume(cursorTimer);
        }
        status("已应用 Linux 客户端显示设置");
    } catch (const std::exception &e) {
        status(e.what());
        stopDisplay();
        return td_status();
    }
    if (pattern) {
        @autoreleasepool {
            NSError *error = nil;
            NSString *source =
                @"#include <metal_stdlib>\nusing namespace metal;kernel void pattern(texture2d<float,access::write>dst[[texture(0)]],constant float&t[[buffer(0)]],uint2 p[[thread_position_in_grid]]){if(p.x>=dst.get_width()||p.y>=dst.get_height())return;float2 uv=float2(p)/float2(dst.get_width(),dst.get_height());float3 bars[8]={float3(1),float3(1,1,0),float3(0,1,1),float3(0,1,0),float3(1,0,1),float3(1,0,0),float3(0,0,1),float3(0)};float3 c=uv.y<0.6?bars[min(uint(uv.x*8),7u)]:float3(0.08+0.08*float((p.x/32+p.y/32)%2));if(abs(uv.x-fract(t*0.15))<0.004)c=float3(1,0.4,0.1);if(uv.y>0.85)c=mix(float3(0.02,0.08,0.15),float3(0.1,0.8,1),uv.x);dst.write(float4(c,1),p);}";
            id<MTLLibrary> lib = [mtl newLibraryWithSource:source options:nil error:&error];
            patternPipeline =
                [mtl newComputePipelineStateWithFunction:[lib newFunctionWithName:@"pattern"]
                                                   error:&error];
            if (!patternPipeline) {
                status(error.localizedDescription.UTF8String);
                stopDisplay();
                return td_status();
            }
            NSDictionary *attributes = @{
                (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                (id)kCVPixelBufferMetalCompatibilityKey : @YES
            };
            if (CVPixelBufferCreate(kCFAllocatorDefault, outputWidth, outputHeight,
                                    kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attributes,
                                    &patternBuffer) != kCVReturnSuccess) {
                status("Cannot create demo buffer");
                stopDisplay();
                return td_status();
            }
            MTLTextureDescriptor *d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:outputWidth
                                                                  height:outputHeight
                                                               mipmapped:NO];
            d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            id<MTLTexture> texture =
                [mtl newTextureWithDescriptor:d
                                    iosurface:CVPixelBufferGetIOSurface(patternBuffer)
                                        plane:0];
            if (!texture) {
                status("Cannot create demo texture");
                stopDisplay();
                return td_status();
            }
            patternTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, encodeQueue);
            dispatch_source_set_timer(patternTimer, DISPATCH_TIME_NOW, 1000000000 / framesPerSecond,
                                      50000);
            uint64_t origin = td::nowNs();
            dispatch_source_set_event_handler(patternTimer, ^{
                if (!running)
                    return;
                {
                    std::lock_guard<std::mutex> lock(connectionMutex);
                    if (!connection)
                        return;
                }
                float t = float((td::nowNs() - origin) / 1e9);
                id<MTLCommandBuffer> cmd = [conversionQueue commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
                [ce setComputePipelineState:patternPipeline];
                [ce setTexture:texture atIndex:0];
                [ce setBytes:&t length:4 atIndex:0];
                [ce dispatchThreads:MTLSizeMake(outputWidth, outputHeight, 1)
                    threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [ce endEncoding];
                [cmd commit];
                [cmd waitUntilCompleted];
                encodeFrame(patternBuffer, td::nowNs());
            });
            dispatch_resume(patternTimer);
            status("GPU 图案演示已启动，等待 UDP 接收端");
            return "";
        }
    }
    captureRetina = retina;
    captureAttemptNs = td::nowNs();
    captureFailed = false;
    const uint32_t wanted = displayID.load();
    const int requestedWidth = logicalWidth, requestedHeight = logicalHeight;
    const uint64_t epoch = captureEpoch;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250000000), dispatch_get_main_queue(), ^{
        startConfiguredCapture(wanted, requestedWidth, requestedHeight, retina, epoch);
    });
    return "";
}

static void stopDisplay() {
    configured = false;
    captureReady = false;
    captureFailed = false;
    captureAttemptNs = 0;
    lastCaptureEventNs = 0;
    ++captureEpoch;
    if (cursorTimer) {
        dispatch_source_cancel(cursorTimer);
        cursorTimer = nil;
    }
    {
        std::lock_guard<std::mutex> lock(cursorMutex);
        cursorShape.reset();
    }
    if (patternTimer) {
        dispatch_source_cancel(patternTimer);
        patternTimer = nil;
    }
    SCStream *old = stream;
    stream = nil;
    if (old)
        [old stopCaptureWithCompletionHandler:^(NSError *){
        }];
    if (captureQueue)
        dispatch_sync(captureQueue, ^{
                      });
    if (encodeQueue)
        dispatch_sync(encodeQueue, ^{
                      });
    stopSender();
    {
        std::lock_guard<std::mutex> lock(pixelMutex);
        if (latestPixel) {
            CVPixelBufferRelease(latestPixel);
            latestPixel = nullptr;
        }
    }
    if (patternBuffer) {
        CVPixelBufferRelease(patternBuffer);
        patternBuffer = nullptr;
    }
    patternPipeline = nil;
    h264Encoder.reset();
    if (encoder) {
        pyrowave_encoder_destroy(encoder);
        encoder = nullptr;
    }
    if (codecDevice) {
        pyrowave_device_destroy(codecDevice);
        codecDevice = nullptr;
    }
    for (int i = 0; i < 3; i++)
        planeTextures[i] = nil;
    conversionPipeline = nil;
    colorTileBuffer = nil;
    resizeKernel = nil;
    bilinearKernel = nil;
    resizeTexture = nil;
    resizeSourceWidth = resizeSourceHeight = 0;
    conversionQueue = nil;
    mtl = nil;
    virtualDisplay = nil;
    displayID = 0;
    busy = false;
}

extern "C" const char *td_start() {
    if (running)
        return "";
    try {
        listener = td::bindUdp(td::ControlPort);
        td::setSocketTimeout(listener.load(), 8);
        running = true;
        controlThread = std::thread(controlLoop, listener.load());
        status("UDP 42420 监听中，等待 Linux 客户端提交显示设置");
        return "";
    } catch (const std::exception &e) {
        status(e.what());
        return td_status();
    }
}

extern "C" void td_stop() {
    running = false;
    int fd = listener.exchange(-1);
    td::closeSocket(fd);
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        connection.reset();
    }
    if (controlThread.joinable())
        controlThread.join();
    stopDisplay();
    status("已停止");
}

extern "C" void td_disconnect_client() {
    disconnectRequested = true;
}

extern "C" int td_client_connected() {
    std::lock_guard<std::mutex> lock(connectionMutex);
    return connection ? 1 : 0;
}
