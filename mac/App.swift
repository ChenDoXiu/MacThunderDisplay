import SwiftUI
import AppKit
import Darwin
import ServiceManagement
import Combine

final class SenderModel: ObservableObject {
    @Published var active = false
    @Published var status = "准备就绪"
    @Published var screenPermission = false
    @Published var inputPermission = false
    @Published var error = ""
    @Published var displayID: UInt32 = 0
    @Published var clientConnected = false
    @Published var launchAtLogin = false
    @Published var loginApprovalRequired = false
    @Published var loginError = ""
    private var timer: Timer?
    private var lastPermissions = ""
    init() {
        freopen("/tmp/thunder-display-sender.log", "a", stderr)
        freopen("/tmp/thunder-display-sender.log", "a", stdout)
        setbuf(stderr, nil)
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.start() }
    }
    func refresh() {
        status = String(cString: td_status())
        displayID = td_display_id()
        clientConnected = td_client_connected() != 0
        let loginStatus = SMAppService.mainApp.status
        launchAtLogin = loginStatus == .enabled || loginStatus == .requiresApproval
        loginApprovalRequired = loginStatus == .requiresApproval
        screenPermission = td_screen_permission(0) != 0
        inputPermission = td_input_permission(0) != 0
        let permissions = "screen=\(screenPermission) input=\(inputPermission)"
        if permissions != lastPermissions {
            FileHandle.standardError.write(
                Data(("[ThunderDisplay] Permissions: " + permissions + "\n").utf8))
            lastPermissions = permissions
        }
    }
    func start() {
        error = String(cString: td_start())
        if !error.isEmpty {
            FileHandle.standardError.write(Data(("[ThunderDisplay] " + error + "\n").utf8))
        }
        active = error.isEmpty
        refresh()
    }
    func disconnectClient() {
        td_disconnect_client()
        clientConnected = false
    }
    func setLaunchAtLogin(_ enabled: Bool) {
        loginError = ""
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            loginError = "自动启动设置失败：\(error.localizedDescription)"
            FileHandle.standardError.write(Data(("[LoginItem] " + loginError + "\n").utf8))
        }
        refresh()
        FileHandle.standardError.write(
            Data(("[LoginItem] status=\(SMAppService.mainApp.status.rawValue)\n").utf8))
    }
}

struct ContentView: View {
    @ObservedObject var model: SenderModel

    private var connectionStatus: String {
        model.clientConnected ? "已连接" : model.active ? "等待连接" : "未就绪"
    }
    private var streamMetrics: [String] {
        guard model.clientConnected, model.status.hasPrefix("串流中") else { return [] }
        return model.status.components(separatedBy: " · ").filter {
            ($0.hasSuffix(" FPS") && !$0.hasPrefix("采集 ")) || $0.hasSuffix(" Mbps")
        }
    }
    private var errorMessage: String? {
        if !model.error.isEmpty { return model.error }
        let routine = ["准备就绪", "串流中", "UDP 42420 监听中", "虚拟屏采集已启动", "已应用 Linux 客户端显示设置", "已停止"]
        return routine.contains(where: { model.status.hasPrefix($0) }) ? nil : model.status
    }
    private func permissionRow(_ title: String, granted: Bool, authorize: @escaping () -> Void)
        -> some View
    {
        HStack(spacing: 8) {
            Image(systemName: granted ? "checkmark.circle.fill" : "exclamationmark.circle")
                .foregroundStyle(granted ? Color.green : Color.orange)
            Text(title)
            Spacer()
            if granted {
                Text("已授权").font(.caption).foregroundStyle(.secondary)
            } else {
                Button("授权", action: authorize).controlSize(.small)
            }
        }
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Label("ThunderDisplay", systemImage: "bolt.horizontal.circle.fill")
                    .font(.headline).foregroundStyle(.cyan)
                Spacer(minLength: 8)
                Circle().fill(model.active ? Color.green : Color.orange).frame(width: 6, height: 6)
                Text(connectionStatus).font(.caption).foregroundStyle(.secondary)
            }
            if !streamMetrics.isEmpty {
                HStack {
                    ForEach(streamMetrics, id: \.self) { metric in
                        Text(metric).monospacedDigit()
                        if metric != streamMetrics.last { Spacer() }
                    }
                }.font(.system(size: 14, weight: .medium))
            }
            if let message = errorMessage {
                HStack(alignment: .top) {
                    Text(message).font(.caption).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                    if !model.active { Button("重试") { model.start() }.controlSize(.small) }
                }
            }
            Divider()
            VStack(spacing: 10) {
                permissionRow("屏幕录制", granted: model.screenPermission) {
                    _ = td_screen_permission(1)
                    model.refresh()
                }
                permissionRow("键鼠控制", granted: model.inputPermission) {
                    _ = td_input_permission(1)
                    model.refresh()
                }
            }
            Divider()
            HStack {
                Toggle(
                    "登录时启动",
                    isOn: Binding(get: { model.launchAtLogin }, set: { model.setLaunchAtLogin($0) })
                )
                .toggleStyle(.switch).controlSize(.small)
                .fixedSize()
                Spacer()
                Button("断开客户端") { model.disconnectClient() }
                    .controlSize(.small).disabled(!model.clientConnected)
            }
            if model.loginApprovalRequired {
                Button("允许自动启动…") { SMAppService.openSystemSettingsLoginItems() }
                    .controlSize(.small)
            }
            if !model.loginError.isEmpty {
                Text(model.loginError).font(.caption).foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }.padding(16).frame(width: 320)
            .onAppear { model.refresh() }
            .onReceive(
                NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)
            ) { _ in model.refresh() }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var model: SenderModel!
    private var statusItem: NSStatusItem!
    private let popover = NSPopover()
    private let contextMenu = NSMenu()
    private var modelSubscription: AnyCancellable?

    func applicationDidFinishLaunching(_ notification: Notification) {
        model = SenderModel()
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem.button {
            button.target = self
            button.action = #selector(statusItemClicked(_:))
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
            button.setAccessibilityLabel("ThunderDisplay")
        }
        let content = NSHostingController(rootView: ContentView(model: model))
        content.sizingOptions = [.preferredContentSize]
        popover.contentViewController = content
        popover.behavior = .transient
        popover.animates = false
        let quit = NSMenuItem(
            title: "退出 ThunderDisplay", action: #selector(quitApplication(_:)), keyEquivalent: "")
        quit.target = self
        contextMenu.addItem(quit)
        modelSubscription = model.$active.combineLatest(model.$clientConnected)
            .sink { [weak self] active, connected in
                guard let self else { return }
                self.statusItem.button?.image = NSImage(
                    systemSymbolName: active
                        ? "bolt.horizontal.circle.fill" : "bolt.horizontal.circle",
                    accessibilityDescription: "ThunderDisplay")
                self.statusItem.button?.image?.isTemplate = true
                self.statusItem.button?.toolTip =
                    connected
                    ? "ThunderDisplay · 客户端已连接"
                    : active ? "ThunderDisplay · 监听中" : "ThunderDisplay · 未就绪"
            }
    }
    @objc private func statusItemClicked(_ sender: NSStatusBarButton) {
        let event = NSApplication.shared.currentEvent
        if event?.type == .rightMouseUp || event?.modifierFlags.contains(.control) == true {
            popover.performClose(nil)
            contextMenu.popUp(
                positioning: nil, at: NSPoint(x: 0, y: sender.bounds.maxY + 4), in: sender)
        } else if popover.isShown {
            popover.performClose(nil)
        } else {
            model.refresh()
            popover.show(relativeTo: sender.bounds, of: sender, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }
    @objc private func quitApplication(_ sender: Any?) {
        NSApplication.shared.terminate(nil)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationWillTerminate(_ notification: Notification) { td_stop() }
}

@main struct ThunderDisplayApp {
    @MainActor static func main() {
        let application = NSApplication.shared
        let delegate = AppDelegate()
        application.delegate = delegate
        application.setActivationPolicy(.accessory)
        application.run()
        withExtendedLifetime(delegate) {}
    }
}
