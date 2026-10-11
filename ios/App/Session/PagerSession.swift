import Foundation
import Observation
import PagerKit

/// The observable state the UI sees for one companion link
/// (`IOS_COMPANION_PLAN.md` §6.2).
///
/// Phase 1 ships the emulator/simulator path only: `connectTCP` opens the
/// PagerLink TCP transport (`protocol/PROTOCOL.md` §5), runs `sys.hello`, sets
/// the pager clock/zone, and refreshes `sys.info`. The CoreBluetooth transport
/// is pending Spike S2; see the TODO below.
@Observable
@MainActor
final class PagerSession {
    /// Lifecycle of the single link.
    enum State {
        case disconnected
        case connecting
        case connected(hello: JSONValue)
        case failed(String)
    }

    private(set) var state: State = .disconnected
    private(set) var info: JSONValue?
    private(set) var lastRTT: Duration?

    /// Rolling session log, newest last, capped at 200 lines.
    private(set) var log: [String] = []

    @ObservationIgnored private var session: LinkSession?
    #if canImport(Network)
        @ObservationIgnored private var transport: TCPTransport?
    #endif

    // TODO(Phase 1, after S2): BLETransport
    // The CoreBluetooth central (PagerKit/Transport/BLETransport.swift), its
    // actor, and state restoration (`CBCentralManagerOptionRestoreIdentifierKey`)
    // land here once Spike S2 confirms iOS lets one peripheral serve both the
    // system keyboard and the PagerLink service. Until then `connectTCP` is the
    // only transport, matching the Simulator (no CoreBluetooth).

    /// Open the emulator TCP transport and bring the link up.
    func connectTCP(host: String = "127.0.0.1", port: UInt16 = 7600) async {
        #if canImport(Network)
            if case .connecting = state { return }
            await disconnect()
            state = .connecting
            appendLog("TCP connecting to \(host):\(port)…")

            let transport = TCPTransport(host: host, port: port)
            self.transport = transport
            await transport.start()

            let session = LinkSession(transport: transport)
            self.session = session
            await session.start()

            let sys = SysService(session: session)
            do {
                let hello = try await sys.hello(app: "0.1", caps: ["sys"])
                state = .connected(hello: hello)
                appendLog("sys.hello ok (fw=\(hello["fw"]?.stringValue ?? "?"))")

                // Auto time sync on connect (plan §4.5: sys.time.set).
                try await sys.setTime(
                    epoch: Int(Date().timeIntervalSince1970),
                    tz: TimeZone.current.identifier
                )
                appendLog("time synced (\(TimeZone.current.identifier))")

                await refresh()
            } catch {
                state = .failed(String(describing: error))
                appendLog("connect failed: \(error)")
            }
        #else
            state = .failed("TCP transport needs Network (Apple platforms only)")
            appendLog("connect unavailable: Network framework missing")
        #endif
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
        if let session {
            await session.stop()
        }
        #if canImport(Network)
            if let transport {
                await transport.stop()
            }
            transport = nil
        #endif
        session = nil
        info = nil
        lastRTT = nil
        state = .disconnected
        if wasConnected {
            appendLog("disconnected")
        }
    }

    /// Format a duration the way the Diagnostics screen shows it.
    nonisolated static func describe(_ duration: Duration) -> String {
        let seconds = Double(duration.components.seconds)
            + Double(duration.components.attoseconds) / 1e18
        return String(format: "%.1f ms", seconds * 1000)
    }

    private func appendLog(_ line: String) {
        let stamp = Date().formatted(date: .omitted, time: .standard)
        log.append("[\(stamp)] \(line)")
        if log.count > 200 {
            log.removeFirst(log.count - 200)
        }
    }
}
