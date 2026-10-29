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
    /// The last `sys.hello` result, kept after a disconnect (Diagnostics export).
    @Published private(set) var lastHello: JSONValue?
    /// What the weather service last fetched / pushed / was asked (Weather tab).
    @Published private(set) var weatherActivity = WeatherActivity()

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
    /// Periodic sys.time.set while a link is up; cancelled in disconnect().
    private var clockTask: Task<Void, Never>?
    /// Observers for the phone's clock / time-zone changes (registered in init).
    private var clockObservers: [NSObjectProtocol] = []
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
    /// The Tasks tab: the pager's `/tasks.txt`, editable. Attached in
    /// `bringUp` like `notes`.
    let tasks = TasksModel()
    /// The Settings tab: the pager's catalog and values
    /// (`IOS_SETTINGS_PLAN.md` §4.4). Attached in `bringUp` like `tasks`.
    let settings = SettingsModel()
    /// The Files tab: browse, download and upload on both volumes. Attached
    /// in `bringUp` like `tasks`.
    let files = FilesModel()
    /// The Chat tab: every `chat.ask` the phone answered, and the Groq key
    /// `ChatService` uses.
    let chat: ChatHistoryModel
    /// The Voice memos tab: pull + transcribe the pager's recordings.
    let voiceMemos: VoiceMemosModel
    /// The battery chart: `evt.battery` and `sys.info` readings, 30 days.
    let battery = BatteryHistoryModel()
    /// Last-known state for the Home / Lock Screen widget.
    private let status = PagerStatusPublisher()
    /// The notes-sync Live Activity.
    private let syncActivity = NotesSyncLiveActivity()
    /// The last `sys.info`, kept after a disconnect (Diagnostics export).
    private var lastInfo: JSONValue?
    /// The Inbox tab: messages to the pager, and the clips / find requests
    /// the pager sent (pasteboard, local notifications).
    let inbox = InboxModel()
    /// The Firmware screen: GitHub release images and the OTA install.
    let firmware = FirmwareModel()
    /// The connected pager's `sys.hello` caps (empty while disconnected):
    /// every Phase 4/6 feature checks its cap here so older firmware hides it.
    @Published private(set) var pagerCaps: [String] = []
    /// The latest pager screen capture (Diagnostics; attached to the export).
    @Published private(set) var lastScreenshot: CapturedScreen?
    /// Messaging, screen capture and OTA on the current link.
    private var messaging: MessagingService?
    private var screen: ScreenCaptureService?
    private var ota: OTAService?

    init(telegram: TelegramNative? = nil) {
        self.telegram = telegram
        notes = NotesModel(
            account: GitHubAccountStore(secrets: KeychainSecretStore(service: "PagerCompanion.github")),
            cacheRoot: NotesModel.defaultCacheRoot()
        )
        let chat = ChatHistoryModel()
        self.chat = chat
        voiceMemos = VoiceMemosModel(notes: notes, groqKey: chat.groqKey)
        notes.log = { [weak self] line in self?.appendLog(line) }
        tasks.log = { [weak self] line in self?.appendLog(line) }
        settings.log = { [weak self] line in self?.appendLog(line) }
        files.log = { [weak self] line in self?.appendLog(line) }
        voiceMemos.log = { [weak self] line in self?.appendLog(line) }
        inbox.log = { [weak self] line in self?.appendLog(line) }
        firmware.log = { [weak self] line in self?.appendLog(line) }
        // The phone's clock or zone moved: re-send it now rather than at the next tick.
        for (name, reason) in [
            (Notification.Name.NSSystemClockDidChange, "phone clock changed"),
            (Notification.Name.NSSystemTimeZoneDidChange, "time zone changed"),
        ] {
            clockObservers.append(NotificationCenter.default.addObserver(
                forName: name, object: nil, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.resyncClockIfConnected(reason: reason) }
            })
        }
        // App Intents reach the running app's session through this.
        AppBridge.session = self
    }

    /// The phone's Telegram login, for screens that talk to Telegram through
    /// the session's own client (the Telegram tab's favorites).
    var telegramNative: TelegramNative? { telegram }

    /// The weather key the pager last asked for (`"auto"` or a city), pushed
    /// again on every connect so the pager's cache is fresh before it asks.
    nonisolated private static let weatherKeyDefault = "PagerLink.weatherKey"
    nonisolated private static let autoConnectDefault = "PagerLink.autoConnect"
    /// How often a live link re-sends the clock, so pager drift never builds up.
    nonisolated private static let clockResyncInterval: Duration = .seconds(30 * 60)

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
        #if targetEnvironment(simulator)
        // The Simulator has no Bluetooth radio; its pager is the emulator.
        Task { await connectTCP(host: TCPTransport.defaultHost, port: TCPTransport.defaultPort) }
        #else
        Task { await connectBLE() }
        #endif
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
        status.link(.connecting)
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
        await registerChat(on: session)
        await registerMessaging(on: session)
        await session.start()
        watchEvents(of: session)

        let sys = SysService(session: session)
        do {
            let hello = try await sys.hello(app: "0.1", caps: ["sys", "weather", "tg", "chat", "clip", "find", "screen"])
            state = .connected(hello: hello)
            lastHello = hello
            pagerCaps = Self.caps(of: hello)
            status.link(.connected, name: hello["name"]?.stringValue)
            reconnectDelay = 2
            appendLog("sys.hello ok (fw=\(hello["fw"]?.stringValue ?? "?"))")

            // Auto time sync on connect (plan §4.5: sys.time.set).
            try await setClock(sys)
            startClockResync()

            await refresh()
            location.requestAuthorizationIfNeeded()
            if let key = UserDefaults.standard.string(forKey: Self.weatherKeyDefault) {
                await pushWeather(key: key)
            }
            await startNotes(on: session, pagerCaps: Self.caps(of: hello))
            await tasks.attach(session, pagerCaps: Self.caps(of: hello))
            await settings.attach(session, pagerCaps: Self.caps(of: hello))
            await files.attach(session, pagerCaps: Self.caps(of: hello))
            await voiceMemos.attach(session, pagerCaps: Self.caps(of: hello))
            if pagerCaps.contains("ota") {
                let ota = OTAService(session: session)
                self.ota = ota
                // An image left by an install that rebooted the pager.
                if await ota.removeLeftover() { appendLog("ota: removed the previous /ota.bin") }
            }
            firmware.attach(self)
            await flushOutbox()
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
        await service.onSyncEvent { [weak self] params in
            guard let event = NotesSyncProgressEvent(params: params) else { return }
            await MainActor.run { self?.syncActivity.handle(event) }
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
            Task { @MainActor in
                self?.appendLog("weather.get for \"\(key)\"")
                // The reply is built after this callback; pick the fetch up shortly.
                try? await Task.sleep(for: .seconds(2))
                self?.weatherActivity = service.activity()
            }
        }
    }

    /// One `ChatService` per link: answers the pager's `chat.audio.begin` /
    /// `chat.ask` with Groq (the key from the Chat tab; without one the
    /// pager gets `unauthorized`) and records every turn in the history.
    private func registerChat(on session: LinkSession) async {
        let service = ChatService(session: session, backend: GroqChatBackend(apiKey: chat.groqKey))
        await service.onRecord { [weak self] record in
            await MainActor.run { self?.chat.append(record) }
        }
        await service.register()
    }

    /// The weather key the pager last asked for (`"auto"`, a city), if any.
    var weatherKey: String? {
        UserDefaults.standard.string(forKey: Self.weatherKeyDefault)
    }

    /// Fetch the forecast for the pager's key without pushing it (the
    /// Weather tab's preview refresh).
    func fetchWeatherPreview() async {
        guard let weather else {
            appendLog("weather: not connected")
            return
        }
        guard let key = weatherKey else {
            appendLog("weather: open Weather on the pager once so the phone learns its location")
            return
        }
        do {
            _ = try await weather.payload(key: key)
        } catch {
            appendLog("weather fetch failed: \(error)")
        }
        weatherActivity = weather.activity()
    }

    /// `weather.push` — fetch the forecast for `key` and send it to the
    /// pager. Returns nil on success, else why it failed (App Intents).
    @discardableResult
    func pushWeather(key: String? = nil) async -> String? {
        guard let weather else {
            appendLog("weather push: not connected")
            return "No pager is connected."
        }
        // The pager's key ("auto", a city, or the "Sao Paulo" first-boot
        // default) is only known once it has asked.
        guard let key = key ?? UserDefaults.standard.string(forKey: Self.weatherKeyDefault) else {
            appendLog("weather push: open Weather on the pager once so the phone learns its location")
            return "Open Weather on the pager once so the phone learns its location."
        }
        defer { weatherActivity = weather.activity() }
        do {
            try await weather.push(key: key)
            appendLog("weather pushed for \"\(key)\"")
            return nil
        } catch {
            appendLog("weather push failed: \(error)")
            return NotesModel.describe(error)
        }
    }

    /// `sys.info` — battery, storage and Wi-Fi status.
    func refresh() async {
        guard let sys else {
            appendLog("refresh: not connected")
            return
        }
        do {
            let info = try await sys.info()
            self.info = info
            lastInfo = info
            if let sample = BatterySample.fromInfo(info, at: Date()) { recordBattery(sample) }
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

    /// Automatic resync: re-send the clock if a link is up (foreground,
    /// phone clock/zone change, periodic timer). A no-op when disconnected.
    func resyncClockIfConnected(reason: String) {
        guard case .connected = state, let sys else { return }
        Task {
            do {
                try await setClock(sys)
            } catch {
                appendLog("auto time sync (\(reason)) failed: \(error)")
            }
        }
    }

    /// Re-send the clock every clockResyncInterval for as long as this link lives.
    private func startClockResync() {
        clockTask?.cancel()
        clockTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: PagerSession.clockResyncInterval)
                guard !Task.isCancelled, let self else { return }
                self.resyncClockIfConnected(reason: "periodic")
            }
        }
    }

    /// Set the pager clock to now in the phone's IANA zone.
    private func setClock(_ sys: SysService) async throws {
        // autoupdatingCurrent: TimeZone.current can stay cached after a zone change.
        let zone = TimeZone.autoupdatingCurrent.identifier
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
        clockTask?.cancel()
        clockTask = nil
        if let session {
            await session.stop()
        }
        let stopTransport = self.stopTransport
        self.stopTransport = nil
        await stopTransport?()
        info = nil
        lastRTT = nil
        await notes.detach()
        tasks.detach()
        settings.detach()
        await files.detach()
        voiceMemos.detach()
        status.link(.disconnected)
        await ota?.linkClosed()
        ota = nil
        messaging = nil
        screen = nil
        pagerCaps = []
        firmware.detach()
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
                if event.name == "evt.fs.changed" {
                    self?.notes.fsChanged()
                    self?.tasks.fsChanged()
                    self?.files.fsChanged()
                    self?.voiceMemos.fsChanged()
                }
                if event.name == "evt.battery" {
                    self?.batteryEvent(event.params)
                }
                await self?.settings.handleEvent(name: event.name, params: event.params)
                await self?.ota?.handleEvent(name: event.name, params: event.params)
            }
            guard let self, self.session === session else { return }
            await self.disconnect()
            self.state = .failed("link dropped")
            self.appendLog("link dropped")
            self.scheduleReconnect()
        }
    }

    // MARK: - Messaging, screen capture, OTA (PROTOCOL.md §6.5–6.7)

    /// Whether the link is up and the pager advertises `cap`.
    func has(_ cap: String) -> Bool {
        guard case .connected = state else { return false }
        return pagerCaps.contains(cap)
    }

    /// Install the pager → phone `clip.push` / `phone.find` handlers and the
    /// `screen.data` receiver; keep the phone → pager calls for this link.
    private func registerMessaging(on session: LinkSession) async {
        let messaging = MessagingService(session: session)
        self.messaging = messaging
        await messaging.register(
            onClip: { [weak self] text in await MainActor.run { self?.inbox.pagerClip(text) } },
            onFind: { [weak self] in await MainActor.run { self?.inbox.pagerFind() } })
        let screen = ScreenCaptureService(session: session)
        self.screen = screen
        await screen.register()
    }

    private static let notConnected = RPCError(code: "disconnected", msg: "not connected")

    private func messaging(for cap: String) throws -> MessagingService {
        guard let messaging, has(cap) else {
            throw pagerCaps.isEmpty ? Self.notConnected
                : RPCError(code: "unsupported", msg: "this pager firmware has no \"\(cap)\"")
        }
        return messaging
    }

    func notifyPost(_ message: NotifyMessage) async throws -> NotifyResult {
        let result = try await messaging(for: "notify").notifyPost(message)
        appendLog("notify.post \(result.dup ? "dup" : "stored")")
        return result
    }

    func clipPush(_ text: String) async throws -> Bool {
        let pasted = try await messaging(for: "clip").clipPush(text)
        appendLog("clip.push \(pasted ? "pasted" : "kept in inbox")")
        return pasted
    }

    func textSend(_ text: String, target: TextTarget) async throws -> Int {
        let typed = try await messaging(for: "text").textSend(text, target: target)
        appendLog("text.send \(target.rawValue): \(typed) bytes")
        return typed
    }

    func findPager(stop: Bool = false) async throws {
        try await messaging(for: "find").findPager(stop: stop)
        appendLog(stop ? "pager.find stop" : "pager.find")
    }

    /// Send what the share extension queued (App Group outbox) with
    /// `notify.post`; called on connect and on every return to the foreground.
    func flushOutbox() async {
        guard let store = inbox.outbox, has("notify"), let messaging else { return }
        guard !store.pending().isEmpty else { return }
        let report = await PagerOutbox.flush(store) { message in try await messaging.notifyPost(message) }
        appendLog("outbox: sent \(report.sent), dup \(report.duplicates), dropped \(report.dropped.count)"
                  + (report.stoppedBy.map { ", stopped: \($0.code)" } ?? ""))
        inbox.outboxChanged()
    }

    /// `screen.capture` → PNG, kept as `lastScreenshot`.
    @discardableResult
    func captureScreen() async throws -> CapturedScreen {
        guard let screen, has("screen") else { throw Self.notConnected }
        let image = try await screen.capture()
        guard let captured = CapturedScreen(image) else {
            throw RPCError(code: "internal", msg: "could not make a PNG")
        }
        lastScreenshot = captured
        appendLog("screen.capture \(image.width)x\(image.height)")
        return captured
    }

    func otaStatus() async throws -> OTAStatus {
        guard let ota, has("ota") else { throw Self.notConnected }
        return try await ota.status()
    }

    func installFirmware(_ image: [UInt8], progress: @escaping @Sendable (OTAProgress) -> Void) async throws -> OTAOutcome {
        guard let ota, has("ota") else { throw Self.notConnected }
        appendLog("ota: installing \(image.count) bytes")
        let outcome = try await ota.install(image, progress: progress)
        appendLog("ota: \(outcome == .reboot ? "pager rebooting into the new image" : "installed (emulator)")")
        return outcome
    }

    /// `evt.battery`: log it, show it on the Dashboard, tell the widget.
    private func batteryEvent(_ params: JSONValue) {
        guard let sample = BatterySample.fromEvent(params, at: Date()) else { return }
        recordBattery(sample)
        if case .object(var object)? = info {
            object["batt_pct"] = .number(Double(sample.pct))
            object["charging"] = .bool(sample.charging)
            info = .object(object)
            lastInfo = info
        }
    }

    private func recordBattery(_ sample: BatterySample) {
        battery.record(sample)
        status.battery(sample)
    }

    /// The Diagnostics export: app version, last `sys.hello` / `sys.info`,
    /// battery summary and the session log (`DiagnosticsReport`).
    func diagnosticsReport() -> DiagnosticsReport {
        let bundle = Bundle.main
        let stateText: String
        switch state {
        case .disconnected: stateText = "disconnected"
        case .connecting: stateText = "connecting"
        case .connected: stateText = "connected"
        case .failed(let message): stateText = "failed: \(message)"
        }
        let os = ProcessInfo.processInfo.operatingSystemVersion
        return DiagnosticsReport(
            generatedAt: Date(),
            appVersion: bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?",
            appBuild: bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?",
            system: "iOS \(os.majorVersion).\(os.minorVersion).\(os.patchVersion)",
            linkState: stateText,
            hello: lastHello,
            info: info ?? lastInfo,
            battery: battery.summary,
            log: log.lines.map(\.text),
            screenshot: lastScreenshot.map {
                DiagnosticsReport.Screenshot(png: [UInt8]($0.png), width: $0.width, height: $0.height,
                                             capturedAt: $0.capturedAt)
            }
        )
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
