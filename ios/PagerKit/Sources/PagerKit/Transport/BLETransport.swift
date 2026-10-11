#if canImport(CoreBluetooth)
import CoreBluetooth
import Foundation

/// PagerLink over BLE GATT (PROTOCOL.md §1–§2): packets go out as writes
/// without response to `rx` and arrive as notifications on `tx`. The packet
/// size is the peripheral's max write-without-response length (ATT_MTU − 3).
///
/// Discovery, in order:
/// 1. `retrieveConnectedPeripherals` for the PagerLink service. This is the
///    normal case: iOS already holds the pager as a BLE keyboard, so the pager
///    is connected and not advertising; the app shares that link.
/// 2. `retrievePeripherals(withIdentifiers:)` for the last pager this app used.
/// 3. A service-filtered scan (the UUID rides in the scan response).
///
/// All CoreBluetooth state lives on one serial queue; the class is
/// `@unchecked Sendable` on that basis. Only built where CoreBluetooth exists
/// (not Linux, not useful in the Simulator).
public final class BLETransport: NSObject, PacketTransport, @unchecked Sendable {
    /// UserDefaults key for the last connected pager's peripheral identifier.
    public static let lastPeripheralKey = "PagerLink.lastPeripheral"
    /// GAP name the firmware advertises (`hal/ble_stack.h` `DEVICE_NAME`).
    public static let deviceNamePrefix = "lilygo"

    /// Shown when the pager is reachable but iOS doesn't list PagerLink: the
    /// phone bonded before the service existed and is using a cached layout.
    static let staleCacheHint = """
        iOS has an old copy of the pager's Bluetooth services. Reboot the pager \
        (or toggle its Bluetooth) and try again; if that doesn't help, open \
        Settings → Bluetooth, tap ⓘ next to the pager, Forget This Device, \
        then pair it again.
        """

    public nonisolated let packets: AsyncStream<[UInt8]>
    private let continuation: AsyncStream<[UInt8]>.Continuation

    private let queue = DispatchQueue(label: "pagerlink.ble")
    private let scanTimeout: TimeInterval
    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var tx: CBCharacteristic?
    private var infoChar: CBCharacteristic?

    private var startContinuation: CheckedContinuation<Void, Error>?
    private var scanTimer: DispatchWorkItem?
    private var ready = false
    private var closed = false
    private var pendingWrites: [(packet: [UInt8], continuation: CheckedContinuation<Void, Error>)] = []

    /// Human-readable progress lines (discovery path, MTU, info JSON),
    /// called on the transport's private queue.
    private let onLog: (@Sendable (String) -> Void)?
    private var info: String?

    public init(scanTimeout: TimeInterval = 10, log: (@Sendable (String) -> Void)? = nil) {
        self.scanTimeout = scanTimeout
        self.onLog = log
        var continuation: AsyncStream<[UInt8]>.Continuation!
        self.packets = AsyncStream { continuation = $0 }
        self.continuation = continuation
        super.init()
    }

    /// The `info` characteristic JSON, once read.
    public var infoJSON: String? {
        queue.sync { info }
    }

    public var packetSize: Int {
        queue.sync {
            guard let peripheral else { return 20 }
            return max(20, peripheral.maximumWriteValueLength(for: .withoutResponse))
        }
    }

    /// Find the pager, connect, discover the service, subscribe to `tx`.
    /// Returns once the link can carry packets. Throws `RPCError.disconnected`
    /// on Bluetooth off/unauthorized, scan timeout, or any GATT failure.
    public func start() async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            queue.async {
                guard self.central == nil, !self.closed else {
                    cont.resume(throwing: RPCError.disconnected("BLE transport already started"))
                    return
                }
                self.startContinuation = cont
                // Creating the manager triggers the permission prompt the first
                // time; discovery begins in centralManagerDidUpdateState.
                self.central = CBCentralManager(delegate: self, queue: self.queue)
            }
        }
    }

    /// Unsubscribe (so the pager drops the companion even though the system
    /// keeps the keyboard link up), release the app's claim on the
    /// peripheral, and finish the packet stream.
    public func stop() {
        queue.async { self.close(reason: "BLE transport stopped") }
    }

    public func send(_ packet: [UInt8]) async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            queue.async {
                guard self.ready, !self.closed else {
                    cont.resume(throwing: RPCError.disconnected("BLE link is not ready"))
                    return
                }
                self.pendingWrites.append((packet, cont))
                self.drainWrites()
            }
        }
    }

    // MARK: - Queue-confined helpers

    private func log(_ line: String) {
        onLog?(line)
    }

    /// Write queued packets while CoreBluetooth has buffer space; the rest go
    /// out from `peripheralIsReady(toSendWriteWithoutResponse:)`.
    private func drainWrites() {
        guard let peripheral, let rx else { return }
        while !pendingWrites.isEmpty, peripheral.canSendWriteWithoutResponse {
            let (packet, cont) = pendingWrites.removeFirst()
            peripheral.writeValue(Data(packet), for: rx, type: .withoutResponse)
            cont.resume()
        }
    }

    private func discover() {
        guard let central else { return }
        let service = PagerLinkUUID.serviceCB

        if let p = central.retrieveConnectedPeripherals(withServices: [service]).first {
            log("found connected pager \(p.name ?? p.identifier.uuidString)")
            connect(p)
            return
        }
        // The pager is connected as a keyboard but iOS doesn't list PagerLink
        // for it (stale GATT cache, see staleCacheHint). Find it by its
        // standard services + name so the error below can say so.
        let standard = [CBUUID(string: "180A"), CBUUID(string: "180F")]
        if let p = central.retrieveConnectedPeripherals(withServices: standard)
            .first(where: { ($0.name ?? "").lowercased().hasPrefix(Self.deviceNamePrefix) }) {
            log("found connected \(p.name ?? "pager") without PagerLink in iOS's service list")
            connect(p)
            return
        }
        if let saved = UserDefaults.standard.string(forKey: Self.lastPeripheralKey),
           let id = UUID(uuidString: saved),
           let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            log("reconnecting to known pager \(p.name ?? saved)")
            connect(p)
            return
        }
        log("scanning for PagerLink…")
        central.scanForPeripherals(withServices: [service], options: nil)
        let timer = DispatchWorkItem { [weak self] in
            guard let self, self.peripheral == nil else { return }
            self.close(reason: """
                no pager found. Check Bluetooth is on on the pager. If iOS shows it \
                connected as a keyboard, \(Self.staleCacheHint)
                """)
        }
        scanTimer = timer
        queue.asyncAfter(deadline: .now() + scanTimeout, execute: timer)
    }

    private func connect(_ p: CBPeripheral) {
        scanTimer?.cancel()
        scanTimer = nil
        central?.stopScan()
        peripheral = p
        p.delegate = self
        central?.connect(p, options: nil)
    }

    private func fail(_ reason: String) {
        close(reason: reason)
    }

    private func close(reason: String) {
        guard !closed else { return }
        closed = true
        ready = false
        scanTimer?.cancel()
        scanTimer = nil
        if let central, central.state == .poweredOn {
            central.stopScan()
            if let peripheral {
                if let tx, peripheral.state == .connected, tx.isNotifying {
                    peripheral.setNotifyValue(false, for: tx)
                }
                central.cancelPeripheralConnection(peripheral)
            }
        }
        log(reason)
        let error = RPCError.disconnected(reason)
        startContinuation?.resume(throwing: error)
        startContinuation = nil
        let writes = pendingWrites
        pendingWrites.removeAll()
        for w in writes {
            w.continuation.resume(throwing: error)
        }
        continuation.finish()
    }
}

// MARK: - CBCentralManagerDelegate

extension BLETransport: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if peripheral == nil, !closed {
                discover()
            }
        case .poweredOff:
            fail("Bluetooth is off")
        case .unauthorized:
            fail("Bluetooth permission denied (Settings → Pager Companion)")
        case .unsupported:
            fail("Bluetooth LE unsupported on this device")
        default:
            break  // .unknown / .resetting: wait for the next update
        }
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        guard self.peripheral == nil else { return }
        log("discovered \(peripheral.name ?? "pager") rssi=\(RSSI)")
        connect(peripheral)
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("connected; discovering PagerLink service")
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.lastPeripheralKey)
        peripheral.discoverServices([PagerLinkUUID.serviceCB])
    }

    public func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        fail("BLE connect failed: \(error?.localizedDescription ?? "unknown")")
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        fail("BLE disconnected\(error.map { ": \($0.localizedDescription)" } ?? "")")
    }
}

// MARK: - CBPeripheralDelegate

extension BLETransport: CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            fail("service discovery failed: \(error.localizedDescription)")
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == PagerLinkUUID.serviceCB }) else {
            fail("pager doesn't expose PagerLink. Is the firmware updated? \(Self.staleCacheHint)")
            return
        }
        peripheral.discoverCharacteristics(
            [PagerLinkUUID.rxCB, PagerLinkUUID.txCB, PagerLinkUUID.infoCB],
            for: service
        )
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error {
            fail("characteristic discovery failed: \(error.localizedDescription)")
            return
        }
        for c in service.characteristics ?? [] {
            switch c.uuid {
            case PagerLinkUUID.rxCB: rx = c
            case PagerLinkUUID.txCB: tx = c
            case PagerLinkUUID.infoCB: infoChar = c
            default: break
            }
        }
        guard let tx, rx != nil else {
            fail("PagerLink service is missing rx/tx")
            return
        }
        if let infoChar {
            peripheral.readValue(for: infoChar)
        }
        // A fresh subscribe makes the pager reset its link state (seq = 0).
        // rx/tx are encrypted; iOS pairs here if the pager isn't bonded yet.
        peripheral.setNotifyValue(true, for: tx)
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard characteristic.uuid == PagerLinkUUID.txCB else { return }
        if let error {
            fail("subscribe to tx failed: \(error.localizedDescription)")
            return
        }
        guard characteristic.isNotifying else { return }
        guard !ready, !closed else { return }
        ready = true
        log("PagerLink up, packet size \(max(20, peripheral.maximumWriteValueLength(for: .withoutResponse)))")
        startContinuation?.resume()
        startContinuation = nil
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard error == nil, let value = characteristic.value else { return }
        switch characteristic.uuid {
        case PagerLinkUUID.txCB:
            guard ready, !value.isEmpty else { return }
            continuation.yield([UInt8](value))
        case PagerLinkUUID.infoCB:
            let json = String(decoding: value, as: UTF8.self)
            info = json
            log("info \(json)")
        default:
            break
        }
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        drainWrites()
    }

    public func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        if invalidatedServices.contains(where: { $0.uuid == PagerLinkUUID.serviceCB }) {
            fail("pager's PagerLink service went away")
        }
    }
}
#endif
