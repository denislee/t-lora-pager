import Foundation
import PagerKit

/// The battery chart's state: PagerKit's `BatteryHistoryStore` (30 days,
/// persisted), fed by `PagerSession` from `evt.battery` and every `sys.info`.
@MainActor
final class BatteryHistoryModel: ObservableObject {
    /// The chart's time windows.
    enum Window: String, CaseIterable, Identifiable {
        case day = "24 h"
        case week = "7 days"
        case month = "30 days"

        var id: String { rawValue }
        var seconds: TimeInterval {
            switch self {
            case .day: return 86_400
            case .week: return 7 * 86_400
            case .month: return 30 * 86_400
            }
        }
    }

    @Published private(set) var samples: [BatterySample] = []
    @Published var window: Window = .week

    private let store: BatteryHistoryStore

    init(store: BatteryHistoryStore = BatteryHistoryStore()) {
        self.store = store
        samples = store.all
    }

    func record(_ sample: BatterySample) {
        if store.record(sample) { samples = store.all }
    }

    /// Samples inside the selected window.
    var visible: [BatterySample] {
        let since = Date().addingTimeInterval(-window.seconds)
        return samples.filter { $0.at >= since }
    }

    var summary: BatterySummary { BatteryHistoryStore.summary(samples) }

    func clear() {
        store.clear()
        samples = []
    }
}
