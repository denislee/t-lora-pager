/// Frozen PagerLink v1 GATT identifiers (PROTOCOL.md §1). These UUIDs must
/// never change; the firmware counterpart lives in `src/hal/link/link_uuids.h`.
public enum PagerLinkUUID {
    /// Primary service advertised in the scan response.
    public static let service = "5BFF3035-4B08-4B35-BEEB-6A426D451D47"
    /// Phone → pager, write / write without response.
    public static let rx = "02855CCC-F4A4-4AA7-AFDC-237158247D4D"
    /// Pager → phone, notify.
    public static let tx = "A975E68C-192B-4364-BE4E-6BB414A20346"
    /// Read-only JSON identity characteristic.
    public static let info = "D7B0CF7E-0B2B-4CAC-BB23-4C0E20464B42"
}

#if canImport(CoreBluetooth)
import CoreBluetooth

extension PagerLinkUUID {
    nonisolated(unsafe) public static let serviceCB = CBUUID(string: service)
    nonisolated(unsafe) public static let rxCB = CBUUID(string: rx)
    nonisolated(unsafe) public static let txCB = CBUUID(string: tx)
    nonisolated(unsafe) public static let infoCB = CBUUID(string: info)
}
#endif
