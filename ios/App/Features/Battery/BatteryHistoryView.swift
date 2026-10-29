import Charts
import PagerKit
import SwiftUI

/// The pager's battery over days (S18): every `evt.battery` and the
/// `sys.info` on each connect, kept 30 days on the phone, as a line chart
/// with charging stretches marked, plus the latest discharge rate — to catch
/// power regressions after a firmware change. State lives in
/// `BatteryHistoryModel`.
struct BatteryHistoryView: View {
    @EnvironmentObject private var battery: BatteryHistoryModel

    @State private var showClear = false

    var body: some View {
        List {
            Section {
                Picker("Window", selection: $battery.window) {
                    ForEach(BatteryHistoryModel.Window.allCases) { window in
                        Text(window.rawValue).tag(window)
                    }
                }
                .pickerStyle(.segmented)
                chart
                    .frame(height: 220)
            } footer: {
                Text("Green points: charging. Logged while the phone is connected.")
            }
            summarySection
            Section {
                Button("Clear history", role: .destructive) { showClear = true }
                    .disabled(battery.samples.isEmpty)
            }
        }
        .navigationTitle("Battery history")
        .confirmationDialog("Clear the battery history?", isPresented: $showClear, titleVisibility: .visible) {
            Button("Clear", role: .destructive) { battery.clear() }
        }
    }

    @ViewBuilder
    private var chart: some View {
        let samples = battery.visible
        if samples.isEmpty {
            Text("No readings in this window yet.")
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            Chart {
                ForEach(samples) { sample in
                    LineMark(x: .value("Time", sample.at), y: .value("Battery", sample.pct))
                        .interpolationMethod(.stepEnd)
                    PointMark(x: .value("Time", sample.at), y: .value("Battery", sample.pct))
                        .symbolSize(sample.charging ? 30 : 12)
                        .foregroundStyle(sample.charging ? Color.green : Color.accentColor)
                }
            }
            .chartYScale(domain: 0...100)
            .chartYAxis {
                AxisMarks(values: [0, 25, 50, 75, 100]) { value in
                    AxisGridLine()
                    AxisValueLabel {
                        if let pct = value.as(Int.self) { Text("\(pct)%") }
                    }
                }
            }
        }
    }

    @ViewBuilder
    private var summarySection: some View {
        let summary = battery.summary
        Section("Summary (30 days)") {
            LabeledContent("Readings", value: "\(summary.count)")
            LabeledContent("Latest", value: summary.latestPct.map { "\($0) %" } ?? "—")
            LabeledContent("Range", value: range(summary))
            LabeledContent("Discharge rate", value: summary.dischargePctPerHour.map {
                String(format: "%.1f %%/h", $0)
            } ?? "—")
            if let first = summary.first {
                LabeledContent("Since", value: first.formatted(date: .abbreviated, time: .shortened))
            }
        }
    }

    private func range(_ summary: BatterySummary) -> String {
        guard let low = summary.minPct, let high = summary.maxPct else { return "—" }
        return "\(low)–\(high) %"
    }
}

#Preview {
    NavigationStack {
        BatteryHistoryView()
            .environmentObject(BatteryHistoryModel())
    }
}
