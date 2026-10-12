import Foundation
import PagerKit
import SwiftUI

/// Connection state, live `sys.info` (battery, storage, Wi-Fi) and the
/// Phase-1 actions. Bluetooth is the primary connect path; the emulator TCP
/// endpoint lives under a disclosure group.
struct DashboardView: View {
    @EnvironmentObject private var session: PagerSession

    @State private var host = "127.0.0.1"
    @State private var port = "7600"

    var body: some View {
        NavigationStack {
            List {
                connectionSection
                if let info = session.info {
                    powerSection(info)
                    storageSection(info)
                    networkSection(info)
                }
                actionsSection
            }
            .navigationTitle("Dashboard")
        }
    }

    // MARK: - Sections

    @ViewBuilder
    private var connectionSection: some View {
        Section("Connection") {
            LabeledContent("State", value: stateText)
            if case .connected(let hello) = session.state {
                LabeledContent("Firmware", value: hello["fw"]?.stringValue ?? "—")
                LabeledContent("Board", value: hello["board"]?.stringValue ?? "—")
                LabeledContent("Name", value: hello["name"]?.stringValue ?? "—")
            }
        }
    }

    @ViewBuilder
    private func powerSection(_ info: JSONValue) -> some View {
        Section("Power") {
            LabeledContent("Battery", value: percent(info["batt_pct"]?.intValue))
            LabeledContent("Charging", value: yesNo(info["charging"]?.boolValue))
            LabeledContent("VBUS", value: yesNo(info["vbus"]?.boolValue))
            LabeledContent("Uptime", value: uptime(info["uptime_s"]?.intValue))
        }
    }

    @ViewBuilder
    private func storageSection(_ info: JSONValue) -> some View {
        let storage = info["storage"]
        Section("Storage") {
            LabeledContent("Internal", value: volume(storage?["int"]))
            LabeledContent(
                "SD",
                value: volume(storage?["sd"], mounted: storage?["sd"]?["mounted"]?.boolValue)
            )
        }
    }

    @ViewBuilder
    private func networkSection(_ info: JSONValue) -> some View {
        Section("Network") {
            LabeledContent("Wi-Fi", value: wifiText(info["wifi"]))
        }
    }

    @ViewBuilder
    private var actionsSection: some View {
        Section {
            Toggle("Auto-connect via Bluetooth", isOn: $session.autoConnect)
            Button("Connect via Bluetooth") {
                Task { await session.connectBLE() }
            }
            .disabled(isConnecting)
            DisclosureGroup("Emulator") {
                TextField("Host", text: $host)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                TextField("Port", text: $port)
                    .keyboardType(.numberPad)
            }
            Button("Connect to emulator") {
                Task { await session.connectTCP(host: host, port: parsedPort) }
            }
            .disabled(isConnecting)
            Button("Refresh") {
                Task { await session.refresh() }
            }
            .disabled(!isConnected)
            Button("Sync time") {
                Task { await session.syncTime() }
            }
            .disabled(!isConnected)
            Button("Push weather") {
                Task { await session.pushWeather() }
            }
            .disabled(!isConnected)
            Button("Disconnect", role: .destructive) {
                Task { await session.userDisconnect() }
            }
            .disabled(!isConnected && !isConnecting)
        }
    }

    // MARK: - Helpers

    private var parsedPort: UInt16 {
        UInt16(port) ?? TCPTransport.defaultPort
    }

    private var isConnected: Bool {
        if case .connected = session.state { return true }
        return false
    }

    private var isConnecting: Bool {
        if case .connecting = session.state { return true }
        return false
    }

    private var stateText: String {
        switch session.state {
        case .disconnected: return "Disconnected"
        case .connecting: return "Connecting…"
        case .connected: return "Connected"
        case .failed(let message): return "Failed: \(message)"
        }
    }

    private func percent(_ value: Int?) -> String {
        guard let value else { return "—" }
        return "\(value) %"
    }

    private func yesNo(_ value: Bool?) -> String {
        guard let value else { return "—" }
        return value ? "Yes" : "No"
    }

    private func uptime(_ seconds: Int?) -> String {
        guard let seconds else { return "—" }
        let formatter = DateComponentsFormatter()
        formatter.allowedUnits = [.day, .hour, .minute]
        formatter.unitsStyle = .abbreviated
        return formatter.string(from: TimeInterval(seconds)) ?? "\(seconds) s"
    }

    private func volume(_ volume: JSONValue?, mounted: Bool? = nil) -> String {
        if mounted == false { return "Not mounted" }
        guard let volume,
              let used = volume["u"]?.intValue,
              let total = volume["t"]?.intValue else {
            return "—"
        }
        return "\(bytes(used)) / \(bytes(total))"
    }

    private func bytes(_ value: Int) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(value), countStyle: .file)
    }

    private func wifiText(_ wifi: JSONValue?) -> String {
        guard let wifi else { return "—" }
        guard wifi["on"]?.boolValue == true else { return "Off" }
        let ssid = wifi["ssid"]?.stringValue ?? "—"
        if let rssi = wifi["rssi"]?.intValue {
            return "\(ssid) (\(rssi) dBm)"
        }
        return ssid
    }
}

#Preview {
    DashboardView()
        .environmentObject(PagerSession())
}
