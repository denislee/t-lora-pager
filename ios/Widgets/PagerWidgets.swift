import ActivityKit
import PagerKit
import SwiftUI
import WidgetKit

// The PagerWidgets extension (S16): a Home / Lock Screen widget with the
// pager's last-known battery and connection state, read from the App Group
// the app writes (`PagerStatusPublisher`), and the notes-sync Live Activity
// the app starts from `evt.notes.sync` (`NotesSyncLiveActivity`). The
// extension never talks to the pager itself. iOS 16.1+ (project.yml).

@main
struct PagerWidgetBundle: WidgetBundle {
    var body: some Widget {
        StatusWidget()
        NotesSyncLiveActivityWidget()
    }
}

// MARK: - Status widget

/// One timeline entry: the snapshot as last written (nil: never connected
/// or no App Group).
struct StatusEntry: TimelineEntry {
    let date: Date
    let snapshot: PagerStatusSnapshot?
}

struct StatusProvider: TimelineProvider {
    private func load() -> PagerStatusSnapshot? {
        let group = PagerStatusStore.appGroupID(bundleID: Bundle.main.bundleIdentifier ?? "")
        guard let defaults = UserDefaults(suiteName: group) else { return nil }
        return PagerStatusStore(defaults: defaults).load()
    }

    func placeholder(in context: Context) -> StatusEntry {
        StatusEntry(date: Date(), snapshot: PagerStatusSnapshot(link: .connected, name: "lilygo-pager",
                                                                batteryPct: 80, charging: false,
                                                                batteryAt: Date(), updatedAt: Date()))
    }

    func getSnapshot(in context: Context, completion: @escaping (StatusEntry) -> Void) {
        completion(context.isPreview ? placeholder(in: context) : StatusEntry(date: Date(), snapshot: load()))
    }

    /// The app reloads the timeline on every change; the half-hourly refresh
    /// only keeps "updated … ago" honest.
    func getTimeline(in context: Context, completion: @escaping (Timeline<StatusEntry>) -> Void) {
        let entry = StatusEntry(date: Date(), snapshot: load())
        completion(Timeline(entries: [entry], policy: .after(Date().addingTimeInterval(30 * 60))))
    }
}

struct StatusWidget: Widget {
    /// `PagerStatusPublisher.widgetKind` in the app.
    static let kind = "PagerStatus"

    var body: some WidgetConfiguration {
        StaticConfiguration(kind: Self.kind, provider: StatusProvider()) { entry in
            StatusWidgetView(entry: entry)
                .widgetBackground()
        }
        .configurationDisplayName("Pager")
        .description("The pager's battery and whether the phone is connected to it.")
        .supportedFamilies([.systemSmall, .accessoryCircular, .accessoryRectangular, .accessoryInline])
    }
}

struct StatusWidgetView: View {
    let entry: StatusEntry
    @Environment(\.widgetFamily) private var family

    var body: some View {
        switch family {
        case .accessoryCircular:
            Gauge(value: Double(entry.snapshot?.batteryPct ?? 0), in: 0...100) {
                Image(systemName: linkSymbol)
            } currentValueLabel: {
                Text(batteryText)
            }
            .gaugeStyle(.accessoryCircular)
        case .accessoryInline:
            Label("Pager \(batteryText)", systemImage: linkSymbol)
        case .accessoryRectangular:
            VStack(alignment: .leading) {
                Label(linkText, systemImage: linkSymbol)
                    .font(.headline)
                Text("Battery \(batteryText)\(charging)")
                Text(updatedText)
                    .font(.caption2)
            }
        default:
            VStack(alignment: .leading, spacing: 6) {
                Label(linkText, systemImage: linkSymbol)
                    .font(.caption.weight(.semibold))
                    .foregroundColor(entry.snapshot?.link == .connected ? .green : .secondary)
                Spacer()
                Image(systemName: batterySymbol)
                    .font(.title2)
                Text(batteryText)
                    .font(.title.weight(.bold).monospacedDigit())
                Text(updatedText)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var batteryText: String {
        entry.snapshot?.batteryPct.map { "\($0)%" } ?? "—"
    }

    private var charging: String { entry.snapshot?.charging == true ? ", charging" : "" }

    private var linkText: String {
        switch entry.snapshot?.link {
        case .connected?: return "Connected"
        case .connecting?: return "Connecting"
        case .disconnected?: return "Not connected"
        case nil: return "No pager yet"
        }
    }

    private var linkSymbol: String {
        entry.snapshot?.link == .connected ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash"
    }

    private var batterySymbol: String {
        guard let pct = entry.snapshot?.batteryPct else { return "battery.0" }
        if entry.snapshot?.charging == true { return "battery.100.bolt" }
        switch pct {
        case 88...: return "battery.100"
        case 63..<88: return "battery.75"
        case 38..<63: return "battery.50"
        case 13..<38: return "battery.25"
        default: return "battery.0"
        }
    }

    private var updatedText: String {
        guard let at = entry.snapshot?.batteryAt ?? entry.snapshot?.updatedAt else { return "" }
        return "Updated \(at.formatted(.relative(presentation: .named)))"
    }
}

// MARK: - Notes-sync Live Activity

struct NotesSyncLiveActivityWidget: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: NotesSyncActivityAttributes.self) { context in
            // Lock Screen / banner.
            HStack(spacing: 12) {
                Image(systemName: context.state.done ? "checkmark.icloud" : "arrow.up.doc")
                    .font(.title2)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Notes sync: \(context.state.phase)")
                        .font(.headline)
                    Text(context.state.summary)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if !context.state.done {
                    ProgressView()
                }
            }
            .padding()
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Image(systemName: "arrow.up.doc")
                }
                DynamicIslandExpandedRegion(.trailing) {
                    Text("\(context.state.uploaded + context.state.already)")
                        .monospacedDigit()
                }
                DynamicIslandExpandedRegion(.bottom) {
                    VStack(alignment: .leading) {
                        Text("Notes sync: \(context.state.phase)")
                            .font(.headline)
                        Text(context.state.summary)
                            .font(.caption)
                    }
                }
            } compactLeading: {
                Image(systemName: "arrow.up.doc")
            } compactTrailing: {
                Text("\(context.state.uploaded + context.state.already)")
                    .monospacedDigit()
            } minimal: {
                Image(systemName: context.state.done ? "checkmark" : "arrow.up.doc")
            }
        }
    }
}

// MARK: - Helpers

private extension View {
    /// iOS 17 requires a container background for widgets; iOS 16 has none.
    @ViewBuilder
    func widgetBackground() -> some View {
        if #available(iOS 17.0, *) {
            containerBackground(.fill.tertiary, for: .widget)
        } else {
            padding()
        }
    }
}
