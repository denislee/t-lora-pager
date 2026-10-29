import PagerKit
import SwiftUI

/// The phone's weather service (S2): where the forecast's location comes
/// from, when it was last fetched and pushed, "Push now", and a preview drawn
/// the way the pager draws it (`src/apps/ui_weather.cpp` `render_cache`: 8
/// three-hourly cells, 7 daily cells, the pager's WMO icon mapping and labels).
struct WeatherView: View {
    @EnvironmentObject private var session: PagerSession
    @EnvironmentObject private var settings: SettingsModel

    @State private var working = false

    var body: some View {
        NavigationStack {
            List {
                locationSection
                activitySection
                actionsSection
                if let forecast {
                    previewSection(forecast)
                }
            }
            .navigationTitle("Weather")
            .refreshable { await session.fetchWeatherPreview() }
        }
    }

    private var activity: WeatherActivity { session.weatherActivity }

    private var forecast: WeatherForecast? {
        activity.lastFetch.flatMap { WeatherForecast(payload: $0.payload) }
    }

    // MARK: - Sections

    @ViewBuilder
    private var locationSection: some View {
        Section("Location") {
            LabeledContent("Pager asks for", value: session.weatherKey.map(keyLabel) ?? "Not yet")
            if let setting = pagerLocationSetting {
                LabeledContent("Pager setting", value: setting)
            }
            if let fetch = activity.lastFetch {
                LabeledContent("Source", value: fetch.source.label)
                if !fetch.city.isEmpty {
                    LabeledContent("Place", value: fetch.city)
                }
            }
        }
    }

    @ViewBuilder
    private var activitySection: some View {
        Section("Activity") {
            LabeledContent("Last fetch", value: activity.lastFetch.map {
                stamp($0.at) + ($0.fromCache ? " (cached)" : "")
            } ?? "—")
            LabeledContent("Last push", value: activity.lastPush.map { push in
                stamp(push.at) + (push.error == nil ? "" : " (failed)")
            } ?? "—")
            if let error = activity.lastPush?.error {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(.red)
            }
            LabeledContent("Pager last asked", value: activity.lastRequest.map { stamp($0.at) } ?? "—")
        }
    }

    @ViewBuilder
    private var actionsSection: some View {
        Section {
            Button {
                run { await session.pushWeather() }
            } label: {
                Label("Push now", systemImage: "arrow.up.circle")
            }
            .disabled(!isConnected || working || session.weatherKey == nil)
            Button {
                run { await session.fetchWeatherPreview() }
            } label: {
                Label("Refresh preview", systemImage: "arrow.clockwise")
            }
            .disabled(!isConnected || working || session.weatherKey == nil)
        } footer: {
            if session.weatherKey == nil {
                Text("Open Weather on the pager once so the phone learns which location it shows.")
            } else if !isConnected {
                Text("Connect to the pager to fetch or push a forecast.")
            }
        }
    }

    private func previewSection(_ forecast: WeatherForecast) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 12) {
                if let city = forecast.city, !city.isEmpty {
                    Text(city)
                        .font(.headline)
                }
                HStack(spacing: 2) {
                    let hour = Calendar.current.component(.hour, from: Date())
                    ForEach(forecast.hourly) { slot in
                        WeatherCell(top: slot.hourLabel, icon: slot.icon, middle: slot.tempLabel,
                                    bottom: slot.popLabel, highlighted: slot.contains(hour: hour))
                    }
                }
                Divider()
                HStack(spacing: 2) {
                    ForEach(Array(forecast.daily.enumerated()), id: \.element.id) { index, day in
                        WeatherCell(top: WeatherForecast.dayLabel(index: index, date: day.date), icon: day.icon,
                                    middle: day.rangeLabel, bottom: day.popLabel, highlighted: index == 0)
                    }
                }
            }
            .padding(.vertical, 4)
        } header: {
            Text("Preview (as on the pager)")
        } footer: {
            if let ts = forecast.ts {
                Text("Forecast from \(stamp(ts)).")
            }
        }
    }

    // MARK: - Helpers

    private var isConnected: Bool {
        if case .connected = session.state { return true }
        return false
    }

    private func run(_ action: @escaping @MainActor () async -> Void) {
        working = true
        Task {
            await action()
            working = false
        }
    }

    private func keyLabel(_ key: String) -> String {
        key == "auto" ? "Automatic location" : key
    }

    /// The pager's `weather.location` setting: `{"auto":true}` or `{"name",…}`.
    private var pagerLocationSetting: String? {
        guard case .json(let location)? = settings.store.value("weather.location") else { return nil }
        if location["auto"]?.boolValue == true { return "Automatic" }
        return location["name"]?.stringValue
    }

    private func stamp(_ date: Date) -> String {
        date.formatted(date: .abbreviated, time: .shortened)
    }
}

/// One pager weather cell: top label, icon, middle label, bottom label,
/// the current slot / today highlighted.
private struct WeatherCell: View {
    let top: String
    let icon: WeatherIcon
    let middle: String
    let bottom: String
    let highlighted: Bool

    var body: some View {
        VStack(spacing: 4) {
            Text(top)
                .font(.caption2.weight(highlighted ? .bold : .regular))
            Image(systemName: icon.symbolName)
                .symbolRenderingMode(.multicolor)
                .font(.body)
                .frame(height: 22)
                .accessibilityLabel(icon.label)
            Text(middle)
                .font(.caption2.monospacedDigit())
            Text(bottom)
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .lineLimit(1)
        .minimumScaleFactor(0.6)
        .frame(maxWidth: .infinity)
        .padding(.vertical, 4)
        .background(highlighted ? Color.accentColor.opacity(0.15) : Color.clear,
                    in: RoundedRectangle(cornerRadius: 6))
    }
}

#Preview {
    let session = PagerSession()
    return WeatherView()
        .environmentObject(session)
        .environmentObject(session.settings)
}
