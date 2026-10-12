import Foundation
import OSLog
import PagerKit

/// The observable state the UI sees for one companion link
/// (`IOS_COMPANION_PLAN.md` §6.2).
///
/// `connectBLE` talks to the pager over the PagerLink GATT service
/// (`protocol/PROTOCOL.md` §1); `connectTCP` talks to the emulator
/// (§5). Either way the session runs `sys.hello`, sets the pager clock/zone,
/// and refreshes `sys.info`. State restoration is not wired yet.
@MainActor
final class PagerSession: ObservableObject {
    /// Lifecycle of the single link.
    enum State {
        case disconnected
        case connecting
        case connected(hello: JSONValue)
        case failed(String)
    }

    @Published private(set) var state: State = .disconnected
    @Published private(set) var info: JSONValue?
    @Published private(set) var lastRTT: Duration?

    /// Rolling session log, newest last, capped at 200 lines.
    @Published private(set) var log: [String] = []

    private var session: LinkSession?
    private var eventsTask: Task<Void, Never>?
    /// Tears the active transport down (TCP or BLE).
    private var stopTransport: (() async -> Void)?
    private var weather: WeatherService?
    private let location = LocationProvider()

    /// The weather key the pager last asked for (`"auto"` or a city), pushed
    /// again on every connect so the pager's cache is fresh before it asks.
    nonisolated private static let weatherKeyDefault = "PagerLink.weatherKey"

    /// Open the emulator TCP transport and bring the link up.
    func connectTCP(host: String = "127.0.0.1", port: UInt16 = 7600) async {
        #if canImport(Network)
            guard await beginConnect("TCP connecting to \(host):\(port)…") else { return }
            let transport = TCPTransport(host: host, port: port)
            stopTransport = { await transport.stop() }
            do {
                try await transport.start()
            } catch {
                await connectFailed(error)
                return
            }
            await bringUp(transport)
        #else
            state = .failed("TCP transport needs Network (Apple platforms only)")
            appendLog("connect unavailable: Network framework missing")
        #endif
    }

    /// Find the pager over Bluetooth (sharing the system keyboard link when
    /// iOS already holds it) and bring the link up.
    func connectBLE() async {
        #if canImport(CoreBluetooth)
            guard await beginConnect("BLE: looking for the pager…") else { return }
            let transport = BLETransport { [weak self] line in
                Task { @MainActor in self?.appendLog("BLE: \(line)") }
            }
            stopTransport = { transport.stop() }
            do {
                try await transport.start()
            } catch {
                await connectFailed(error)
                return
            }
            await bringUp(transport)
        #else
            state = .failed("Bluetooth transport needs CoreBluetooth")
            appendLog("connect unavailable: CoreBluetooth missing")
        #endif
    }

    /// Common pre-connect: drop any old link, enter `.connecting`.
    private func beginConnect(_ line: String) async -> Bool {
        if case .connecting = state { return false }
        await disconnect()
        state = .connecting
        appendLog(line)
        return true
    }

    private func connectFailed(_ error: Error) async {
        await stopTransport?()
        stopTransport = nil
        state = .failed(String(describing: error))
        appendLog("connect failed: \(error)")
    }

    /// Start the RPC session on an open transport: `sys.hello`, time sync,
    /// `sys.info`.
    private func bringUp(_ transport: PacketTransport) async {
        let session = LinkSession(transport: transport)
        self.session = session
        await registerWeather(on: session)
        await TelegramService(session: session).register()
        await session.start()
        watchEvents(of: session)

        let sys = SysService(session: session)
        do {
            let hello = try await sys.hello(app: "0.1", caps: ["sys", "weather", "tg"])
            state = .connected(hello: hello)
            appendLog("sys.hello ok (fw=\(hello["fw"]?.stringValue ?? "?"))")

            // Auto time sync on connect (plan §4.5: sys.time.set).
            try await sys.setTime(
                epoch: Int(Date().timeIntervalSince1970),
                tz: TimeZone.current.identifier
            )
            appendLog("time synced (\(TimeZone.current.identifier))")

            await refresh()
            location.requestAuthorizationIfNeeded()
            if let key = UserDefaults.standard.string(forKey: Self.weatherKeyDefault) {
                await pushWeather(key: key)
            }
        } catch {
            await disconnect()
            state = .failed(String(describing: error))
            appendLog("connect failed: \(error)")
        }
    }

    /// Answer the pager's `weather.get` (PROTOCOL.md §6.3).
    private func registerWeather(on session: LinkSession) async {
        let location = self.location
        let service = WeatherService(session: session, locator: { try await location.place() })
        weather = service
        await service.register { [weak self] key in
            UserDefaults.standard.set(key, forKey: PagerSession.weatherKeyDefault)
            Task { @MainActor in self?.appendLog("weather.get for \"\(key)\"") }
        }
    }

    /// `weather.push` — fetch the forecast for `key` and send it to the pager.
    func pushWeather(key: String? = nil) async {
        guard let weather else {
            appendLog("weather push: not connected")
            return
        }
        // The pager's key ("auto", a city, or the "Sao Paulo" first-boot
        // default) is only known once it has asked.
        guard let key = key ?? UserDefaults.standard.string(forKey: Self.weatherKeyDefault) else {
            appendLog("weather push: open Weather on the pager once so the phone learns its location")
            return
        }
        do {
            try await weather.push(key: key)
            appendLog("weather pushed for \"\(key)\"")
        } catch {
            appendLog("weather push failed: \(error)")
        }
    }

    /// `sys.info` — battery, storage and Wi-Fi status.
    func refresh() async {
        guard let session else {
            appendLog("refresh: not connected")
            return
        }
        do {
            info = try await SysService(session: session).info()
            appendLog("sys.info updated")
        } catch {
            appendLog("sys.info failed: \(error)")
        }
    }

    /// `sys.ping` — round-trip time, measured with `ContinuousClock`.
    func ping() async {
        guard let session else {
            appendLog("ping: not connected")
            return
        }
        let start = ContinuousClock.now
        do {
            let echoed = try await SysService(session: session).ping(t: Int(Date().timeIntervalSince1970))
            let elapsed = ContinuousClock.now - start
            lastRTT = elapsed
            appendLog("sys.ping ok (echo \(echoed), \(Self.describe(elapsed)))")
        } catch {
            appendLog("sys.ping failed: \(error)")
        }
    }

    /// `sys.time.set` — re-send the current epoch + IANA zone.
    func syncTime() async {
        guard let session else {
            appendLog("sync time: not connected")
            return
        }
        do {
            try await SysService(session: session).setTime(
                epoch: Int(Date().timeIntervalSince1970),
                tz: TimeZone.current.identifier
            )
            appendLog("time synced (\(TimeZone.current.identifier))")
        } catch {
            appendLog("time sync failed: \(error)")
        }
    }

    /// Tear the link down and clear the transient device state.
    func disconnect() async {
        let wasConnected = session != nil
        // Detach first so the events watcher doesn't report this as a drop.
        let session = self.session
        self.session = nil
        weather = nil
        eventsTask?.cancel()
        eventsTask = nil
        if let session {
            await session.stop()
        }
        let stopTransport = self.stopTransport
        self.stopTransport = nil
        await stopTransport?()
        info = nil
        lastRTT = nil
        state = .disconnected
        if wasConnected {
            appendLog("disconnected")
        }
    }

    /// Log pager events and notice when the link drops (the session's event
    /// stream finishes when the transport closes).
    private func watchEvents(of session: LinkSession) {
        eventsTask = Task { [weak self] in
            for await event in session.events {
                self?.appendLog("event \(event.name)")
            }
            guard let self, self.session === session else { return }
            await self.disconnect()
            self.state = .failed("link dropped")
            self.appendLog("link dropped")
        }
    }

    /// Format a duration the way the Diagnostics screen shows it.
    nonisolated static func describe(_ duration: Duration) -> String {
        let seconds = Double(duration.components.seconds)
            + Double(duration.components.attoseconds) / 1e18
        return String(format: "%.1f ms", seconds * 1000)
    }

    private static let logger = Logger(subsystem: "PagerCompanion", category: "session")

    private func appendLog(_ line: String) {
        let stamp = Date().formatted(date: .omitted, time: .standard)
        log.append("[\(stamp)] \(line)")
        // Mirror to the unified log so the Mac can read it: Console.app,
        // `make device-console` (iOS 17+) or `make device-log` (any iOS).
        Self.logger.notice("[pagerlink] \(line, privacy: .public)")
        if log.count > 200 {
            log.removeFirst(log.count - 200)
        }
    }
}
