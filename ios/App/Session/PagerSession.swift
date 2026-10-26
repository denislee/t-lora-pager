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

    /// Connect over Bluetooth on launch / foreground and keep retrying (with
    /// backoff) after a failed connect or a dropped link. Persisted; on by default.
    @Published var autoConnect: Bool = UserDefaults.standard.object(forKey: PagerSession.autoConnectDefault) as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(autoConnect, forKey: Self.autoConnectDefault)
            if autoConnect {
                userDisconnected = false
                autoConnectIfNeeded()
            } else {
                cancelReconnect()
            }
        }
    }

    /// Rolling session log, newest last, capped at 200 lines. Its own
    /// observable, so an appended line re-renders only the log view rather
    /// than every view watching the session (P7.63).
    let log = SessionLog()

    private var session: LinkSession?
    private var eventsTask: Task<Void, Never>?
    /// Tears the active transport down (TCP or BLE).
    private var stopTransport: (() async -> Void)?
    private var weather: WeatherService?
    private let location = LocationProvider()
    /// Pending auto-connect retry, if any.
    private var reconnectTask: Task<Void, Never>?
    /// Next retry delay in seconds; doubles per failure up to 60, reset on connect.
    private var reconnectDelay: Double = 2
    /// The user pressed Disconnect: no auto-connect until they connect again
    /// (or re-enable the toggle, or relaunch the app).
    private var userDisconnected = false
    /// Whether the current/last link is BLE (only BLE links auto-reconnect).
    private var linkIsBLE = false
    /// The phone's own Telegram login; answers tg.* while logged in.
    private let telegram: TelegramNative?
    /// The Notes tab: every note on the pager, and the move to GitHub
    /// (`IOS_NOTES_VIEWER_PLAN.md`). Outlives links; attached in `bringUp`.
    let notes: NotesModel

    init(telegram: TelegramNative? = nil) {
        self.telegram = telegram
        notes = NotesModel(
            account: GitHubAccountStore(secrets: KeychainSecretStore(service: "PagerCompanion.github")),
            cacheRoot: NotesModel.defaultCacheRoot()
        )
        notes.log = { [weak self] line in self?.appendLog(line) }
    }

    /// The weather key the pager last asked for (`"auto"` or a city), pushed
    /// again on every connect so the pager's cache is fresh before it asks.
    nonisolated private static let weatherKeyDefault = "PagerLink.weatherKey"
    nonisolated private static let autoConnectDefault = "PagerLink.autoConnect"

    /// Called on launch and whenever the app becomes active: connect over
    /// Bluetooth right away if auto-connect is on and no link is up.
    func autoConnectIfNeeded() {
        guard autoConnect, !userDisconnected else { return }
        switch state {
        case .connected, .connecting: return
        case .disconnected, .failed: break
        }
        cancelReconnect()
        reconnectDelay = 2
        Task { await connectBLE() }
    }

    /// Retry the BLE connect after a backoff delay, if auto-connect wants it.
    private func scheduleReconnect() {
        guard autoConnect, !userDisconnected, linkIsBLE, reconnectTask == nil else { return }
        let delay = reconnectDelay
        reconnectDelay = min(reconnectDelay * 2, 60)
        appendLog("auto-connect: retrying in \(Int(delay)) s")
        reconnectTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard let self, !Task.isCancelled else { return }
            self.reconnectTask = nil
            await self.connectBLE()
        }
    }

    private func cancelReconnect() {
        reconnectTask?.cancel()
        reconnectTask = nil
    }

    /// The Disconnect button: drop the link and stop auto-connect from
    /// immediately bringing it back.
    func userDisconnect() async {
        userDisconnected = true
        cancelReconnect()
        await disconnect()
    }

    /// Open the emulator TCP transport and bring the link up.
    func connectTCP(host: String, port: UInt16) async {
        cancelReconnect()
        userDisconnected = false
        guard await beginConnect("TCP connecting to \(host):\(port)…") else { return }
        linkIsBLE = false
        let transport = TCPTransport(host: host, port: port)
        stopTransport = { await transport.stop() }
        do {
            try await transport.start()
        } catch {
            await connectFailed(error)
            return
        }
        await bringUp(transport)
    }

    /// Find the pager over Bluetooth (sharing the system keyboard link when
    /// iOS already holds it) and bring the link up.
    func connectBLE() async {
        cancelReconnect()
        userDisconnected = false
        guard await beginConnect("BLE: looking for the pager…") else { return }
        linkIsBLE = true
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
    }

    /// Common pre-connect: drop any old link, enter `.connecting`.
    private func beginConnect(_ line: String) async -> Bool {
        if case .connecting = state { return false }
        // Claim the slot before suspending so a concurrent caller (launch +
        // foreground auto-connect) backs off instead of racing this one.
        state = .connecting
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
        scheduleReconnect()
    }

    /// Start the RPC session on an open transport: `sys.hello`, time sync,
    /// `sys.info`.
    private func bringUp(_ transport: PacketTransport) async {
        let session = LinkSession(transport: transport)
        self.session = session
        await registerWeather(on: session)
        await TelegramService(session: session, native: telegram).register()
        await session.start()
        watchEvents(of: session)

        let sys = SysService(session: session)
        do {
            let hello = try await sys.hello(app: "0.1", caps: ["sys", "weather", "tg"])
            state = .connected(hello: hello)
            reconnectDelay = 2
            appendLog("sys.hello ok (fw=\(hello["fw"]?.stringValue ?? "?"))")

            // Auto time sync on connect (plan §4.5: sys.time.set).
            try await setClock(sys)

            await refresh()
            location.requestAuthorizationIfNeeded()
            if let key = UserDefaults.standard.string(forKey: Self.weatherKeyDefault) {
                await pushWeather(key: key)
            }
            await startNotes(on: session, pagerCaps: Self.caps(of: hello))
        } catch {
            await disconnect()
            state = .failed(String(describing: error))
            appendLog("connect failed: \(error)")
            scheduleReconnect()
        }
    }

    /// One `NotesSyncService` per link: answers the pager's `notes.sync`
    /// (its Notes Sync button) and runs the Notes tab's "Sync to GitHub",
    /// both as a move (upload, verify, then conditional delete). The pager's
    /// caps say whether it honours the delete precondition.
    private func startNotes(on session: LinkSession, pagerCaps: [String]) async {
        let account = notes.account
        let backend = GitHubNotesBackend(config: { account.config() })
        let service = NotesSyncService(session: session, github: backend, phoneDirectory: nil,
                                       pagerCaps: pagerCaps)
        await service.onPagerMove { [weak self] result in
            await MainActor.run { self?.notes.pagerMoveFinished(result) }
        }
        await service.register()
        if !pagerCaps.contains(NotesSyncService.conditionalDeleteCap) {
            appendLog("notes: pager firmware has no fs.delete.if; sync uploads but keeps the notes")
        }
        await notes.attach(session, sync: service, pagerCaps: pagerCaps)
    }

    /// The `caps` strings of a `sys.hello` result.
    nonisolated static func caps(of hello: JSONValue) -> [String] {
        guard case .array(let caps)? = hello["caps"] else { return [] }
        return caps.compactMap(\.stringValue)
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
        guard let sys else {
            appendLog("refresh: not connected")
            return
        }
        do {
            info = try await sys.info()
            appendLog("sys.info updated")
        } catch {
            appendLog("sys.info failed: \(error)")
        }
    }

    /// `sys.ping` — round-trip time, measured with `ContinuousClock`.
    func ping() async {
        guard let sys else {
            appendLog("ping: not connected")
            return
        }
        let start = ContinuousClock.now
        do {
            let echoed = try await sys.ping(t: Int(Date().timeIntervalSince1970))
            let elapsed = ContinuousClock.now - start
            lastRTT = elapsed
            appendLog("sys.ping ok (echo \(echoed), \(Self.describe(elapsed)))")
        } catch {
            appendLog("sys.ping failed: \(error)")
        }
    }

    /// `sys.time.set` — re-send the current epoch + IANA zone.
    func syncTime() async {
        guard let sys else {
            appendLog("sync time: not connected")
            return
        }
        do {
            try await setClock(sys)
        } catch {
            appendLog("time sync failed: \(error)")
        }
    }

    /// Set the pager clock to now in the phone's IANA zone.
    private func setClock(_ sys: SysService) async throws {
        let zone = TimeZone.current.identifier
        try await sys.setTime(epoch: Int(Date().timeIntervalSince1970), tz: zone)
        appendLog("time synced (\(zone))")
    }

    /// `sys.*` on the current link, if any.
    private var sys: SysService? {
        session.map { SysService(session: $0) }
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
        await notes.detach()
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
                if event.name == "evt.fs.changed" { self?.notes.fsChanged() }
            }
            guard let self, self.session === session else { return }
            await self.disconnect()
            self.state = .failed("link dropped")
            self.appendLog("link dropped")
            self.scheduleReconnect()
        }
    }

    /// Format a duration the way the Diagnostics screen shows it.
    nonisolated static func describe(_ duration: Duration) -> String {
        String(format: "%.1f ms", duration / .milliseconds(1))
    }

    private static let logger = Logger(subsystem: "PagerCompanion", category: "session")

    private func appendLog(_ line: String) {
        let stamp = Date().formatted(date: .omitted, time: .standard)
        log.append("[\(stamp)] \(line)")
        // Mirror to the unified log so the Mac can read it: Console.app,
        // `make device-console` (iOS 17+) or `make device-log` (any iOS).
        Self.logger.notice("[pagerlink] \(line, privacy: .public)")
    }
}

/// The session log's lines, each with an id that stays put when the cap
/// drops the oldest, so SwiftUI rows don't all shift on rotation.
@MainActor
final class SessionLog: ObservableObject {
    struct Line: Identifiable {
        let id: Int
        let text: String
    }

    static let capacity = 200

    @Published private(set) var lines: [Line] = []
    private var nextID = 0

    func append(_ text: String) {
        lines.append(Line(id: nextID, text: text))
        nextID += 1
        if lines.count > Self.capacity {
            lines.removeFirst(lines.count - Self.capacity)
        }
    }
}
