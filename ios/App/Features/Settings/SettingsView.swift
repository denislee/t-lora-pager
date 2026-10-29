import PagerKit
import SwiftUI

/// The pager's Settings tab (`IOS_SETTINGS_PLAN.md` §4.4): every group the
/// pager advertises, rendered from its catalog. A setting whose UI needs more
/// than one row gets a whole screen (Wi-Fi, time zone, weather location,
/// Telegram favorites, the GitHub copy); everything else is a `SettingRow` or
/// an `ActionRow`. State lives in `SettingsStore`; this view only sequences it.
/// Offline the last values read stay visible, read-only.
struct SettingsView: View {
    @EnvironmentObject private var settings: SettingsModel
    @EnvironmentObject private var session: PagerSession
    @EnvironmentObject private var notes: NotesModel

    /// Settings a custom screen owns, so no generic row is drawn for them.
    private static let handledItems: Set<String> = [
        "wifi.saved", "time.tz", "weather.location", "tg.favorites",
        // The SSH screen (host, user, port, password, key).
        "ssh.host", "ssh.user", "ssh.port", "ssh.password", "ssh.key",
    ]
    /// Actions a custom screen owns, so no generic `ActionRow` is drawn.
    private static let handledActions: Set<String> = [
        "wifi.scan", "wifi.add", "wifi.forget", "wifi.test",
    ]

    var body: some View {
        NavigationStack {
            List {
                statusSection
                catalogSections
                thisPhoneSection
            }
            .navigationTitle("Settings")
            .refreshable { await settings.store.reload() }
        }
    }

    // MARK: - Sections

    private var hasStatus: Bool {
        !settings.store.connected || settings.store.rebootNeeded || settings.store.lastError != nil
    }

    @ViewBuilder
    private var statusSection: some View {
        if hasStatus {
            Section {
                if !settings.store.connected {
                    Label("Offline — last values, read-only", systemImage: "wifi.slash")
                        .foregroundStyle(.secondary)
                }
                if settings.store.rebootNeeded {
                    Label("Restart the pager to apply some changes", systemImage: "arrow.clockwise")
                        .foregroundStyle(.orange)
                }
                if let error = settings.store.lastError {
                    Text(error).foregroundStyle(.red)
                }
            }
        }
    }

    @ViewBuilder
    private var catalogSections: some View {
        if let schema = settings.store.schema {
            ForEach(settings.store.groups()) { group in
                Section(group.title) {
                    customContent(for: group.id)
                    ForEach(schema.items(in: group.id).filter { !Self.handledItems.contains($0.id) }) { item in
                        SettingRow(item: item)
                    }
                    ForEach(schema.actions(in: group.id).filter { !Self.handledActions.contains($0.id) }) { action in
                        ActionRow(action: action)
                    }
                }
            }
        }
    }

    private var thisPhoneSection: some View {
        Section("This phone") {
            NavigationLink("GitHub account") {
                GitHubSettingsView()
            }
            Text("The Telegram login lives in the Telegram tab.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }

    // MARK: - Custom screens

    @ViewBuilder
    private func customContent(for group: String) -> some View {
        switch group {
        case "wifi":
            NavigationLink("Wi-Fi networks") {
                WiFiNetworksView()
            }
        case "time":
            NavigationLink {
                TimeZonePickerView()
            } label: {
                LabeledContent("Time zone", value: tzValue)
            }
        case "weather":
            NavigationLink {
                WeatherLocationView()
            } label: {
                LabeledContent("Location", value: weatherLocation)
            }
        case "tg":
            NavigationLink("Favorite chats") {
                TelegramFavoritesView()
            }
        case "ssh":
            NavigationLink {
                SSHSettingsView()
            } label: {
                LabeledContent("SSH host & key", value: settings.store.value("ssh.host")?.stringValue ?? "")
            }
        case "nsync":
            Button {
                guard let config = notes.account.config() else { return }
                Task { _ = await settings.copyGitHub(config) }
            } label: {
                Text("Copy from this phone's GitHub settings")
            }
            .disabled(notes.account.config() == nil || !settings.store.canEdit)
        default:
            EmptyView()
        }
    }

    /// `time.tz` as shown in the link; `""` means the pager's built-in default.
    private var tzValue: String {
        guard case .string(let zone)? = settings.store.value("time.tz"), !zone.isEmpty else {
            return "Default"
        }
        return zone
    }

    /// `weather.location` is `{"auto":true}` or `{"name","lat","lon"}`.
    private var weatherLocation: String {
        guard case .json(let location)? = settings.store.value("weather.location") else {
            return "Auto"
        }
        if location["auto"]?.boolValue == true { return "Auto" }
        return location["name"]?.stringValue ?? "Custom"
    }
}

// MARK: - One setting

/// One catalog item as an editor. The control follows `item.kind`; every write
/// goes through `SettingsModel.commit` in a Task, and the row is read-only
/// while the link is down or the item says so.
struct SettingRow: View {
    let item: SettingItem

    @EnvironmentObject private var settings: SettingsModel
    @State private var text = ""
    @State private var secretText = ""
    @State private var confirmDangerOff = false

    private var disabled: Bool {
        !settings.store.canEdit || item.readOnly
    }

    private var currentValue: SettingValue? {
        settings.store.value(item.id)
    }

    private var currentString: String {
        currentValue?.stringValue ?? ""
    }

    private var intValue: Int {
        currentValue?.intValue ?? item.min ?? 0
    }

    private var secretIsSet: Bool {
        if case .secret(let set)? = currentValue { return set }
        return false
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            control
            footnote
        }
        .disabled(disabled)
        .onAppear(perform: syncText)
        // iOS 16 single-parameter form: a value changed under us (a pager
        // write or a reload) replaces the field only for plain strings.
        .onChange(of: currentString) { newValue in
            if item.kind == .string { text = newValue }
        }
        .confirmationDialog(item.title, isPresented: $confirmDangerOff, titleVisibility: .visible) {
            Button("Turn off", role: .destructive) {
                Task { _ = await settings.commit(item.id, .bool(false)) }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(item.hint ?? "Turn off \(item.title)?")
        }
    }

    // MARK: - Controls

    @ViewBuilder
    private var control: some View {
        switch item.kind {
        case .bool:
            Toggle(isOn: boolBinding) {
                Text(item.title)
            }
        case .int:
            intControl
        case .enumeration:
            if item.readOnly {
                LabeledContent {
                    Text(optionLabel(currentValue))
                } label: {
                    Text(item.title)
                }
            } else {
                Picker(selection: enumBinding) {
                    ForEach(item.options, id: \.self) { option in
                        Text(option.label).tag(option.value)
                    }
                } label: {
                    Text(item.title)
                }
                .pickerStyle(.menu)
            }
        case .string:
            TextField(item.title, text: $text)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .submitLabel(.done)
                .onSubmit(commitText)
        case .secret:
            secretControl
        case .list, .object:
            LabeledContent {
                Text(jsonSummary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.trailing)
                    .lineLimit(3)
            } label: {
                Text(item.title)
            }
        }
    }

    @ViewBuilder
    private var intControl: some View {
        if let low = item.min, let high = item.max, high >= low {
            let step = max(1, item.step ?? 1)
            if (high - low) / step <= 40 {
                Stepper(value: intBinding, in: low...high, step: step) {
                    Text(item.title)
                }
            } else {
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text(item.title)
                        Spacer()
                        Text("\(intValue)")
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    }
                    Slider(
                        value: sliderBinding,
                        in: Double(low)...Double(high),
                        step: Double(step),
                        onEditingChanged: sliderEditingChanged
                    )
                }
            }
        } else {
            LabeledContent {
                Text("—")
            } label: {
                Text(item.title)
            }
        }
    }

    private var secretControl: some View {
        VStack(alignment: .leading, spacing: 6) {
            LabeledContent {
                Text(secretIsSet ? "Set" : "Not set")
            } label: {
                Text(item.title)
            }
            HStack {
                SecureField("New value", text: $secretText)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .onSubmit { commitSecret(secretText) }
                Button {
                    commitSecret(secretText)
                } label: {
                    Text("Save")
                }
                .disabled(secretText.isEmpty)
            }
            Button("Clear", role: .destructive) {
                commitSecret("")
            }
            Text("The pager never sends the secret back; it needs notes encryption on and unlocked on the pager.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private var footnote: some View {
        if item.hint != nil || item.apply == .reboot {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                if let hint = item.hint {
                    Text(hint)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                if item.apply == .reboot {
                    restartTag
                }
            }
        }
    }

    private var restartTag: some View {
        Text("Restart needed")
            .font(.caption2.weight(.semibold))
            .foregroundStyle(.orange)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Color.orange.opacity(0.15), in: Capsule())
    }

    // MARK: - Bindings

    private var boolBinding: Binding<Bool> {
        Binding(
            get: { currentValue?.boolValue ?? false },
            set: { newValue in
                // Turning a dangerous switch off asks first.
                if item.danger && !newValue {
                    confirmDangerOff = true
                    return
                }
                settings.store.draft(item.id, .bool(newValue))
                Task { _ = await settings.commit(item.id, .bool(newValue)) }
            }
        )
    }

    private var intBinding: Binding<Int> {
        Binding(
            get: { intValue },
            set: { newValue in
                settings.store.draft(item.id, .int(newValue))
                Task { _ = await settings.commit(item.id, .int(newValue)) }
            }
        )
    }

    /// The slider stages its value while the thumb moves; the commit happens
    /// once, when the drag ends.
    private var sliderBinding: Binding<Double> {
        Binding(
            get: { Double(intValue) },
            set: { newValue in
                settings.store.draft(item.id, .int(Int(newValue.rounded())))
            }
        )
    }

    private var enumBinding: Binding<SettingValue> {
        Binding(
            get: { currentValue ?? item.options.first?.value ?? .string("") },
            set: { newValue in
                settings.store.draft(item.id, newValue)
                Task { _ = await settings.commit(item.id, newValue) }
            }
        )
    }

    // MARK: - Actions

    private func sliderEditingChanged(_ editing: Bool) {
        guard !editing else { return }
        let value = intValue
        Task { _ = await settings.commit(item.id, .int(value)) }
    }

    private func commitText() {
        let value = text
        Task { _ = await settings.commit(item.id, .string(value)) }
    }

    private func commitSecret(_ value: String) {
        Task {
            let saved = await settings.commit(item.id, .string(value))
            if saved { secretText = "" }
        }
    }

    // MARK: - Helpers

    private func syncText() {
        if item.kind == .string { text = currentString }
    }

    private func optionLabel(_ value: SettingValue?) -> String {
        guard let value else { return "—" }
        return item.options.first { $0.value == value }?.label
            ?? value.stringValue
            ?? value.intValue.map(String.init)
            ?? "—"
    }

    /// A short JSON rendering of a `list` / `object` setting, for the rows the
    /// custom screens do not own.
    private var jsonSummary: String {
        guard case .json(let json)? = currentValue, let bytes = try? json.encoded() else {
            return "—"
        }
        let rendered = String(decoding: bytes, as: UTF8.self)
        return rendered.count > 140 ? String(rendered.prefix(137)) + "…" : rendered
    }
}

// MARK: - One action

/// One `settings.action` as a button. A long action reports its result under
/// the button; a dangerous one asks first.
struct ActionRow: View {
    let action: SettingAction

    @EnvironmentObject private var settings: SettingsModel
    @State private var running = false
    @State private var showConfirm = false
    @State private var message: String?
    @State private var failed = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 8) {
                Button {
                    if action.danger {
                        showConfirm = true
                    } else {
                        run()
                    }
                } label: {
                    Text(action.title)
                }
                .disabled(running || !settings.store.canEdit)
                if running {
                    ProgressView()
                }
            }
            if let hint = action.hint, !hint.isEmpty {
                Text(hint)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if let message {
                Text(message)
                    .font(.caption)
                    .foregroundStyle(failed ? Color.red : Color.secondary)
            }
        }
        .confirmationDialog(action.title, isPresented: $showConfirm, titleVisibility: .visible) {
            Button("Continue", role: .destructive) { run() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(confirmMessage)
        }
    }

    private var confirmMessage: String {
        // The pager owns this confirmation entirely.
        if action.id == "sys.power_off" {
            return "The pager asks for confirmation on its own screen."
        }
        return action.hint ?? action.title
    }

    private func run() {
        running = true
        message = nil
        Task {
            do {
                _ = try await settings.run(action.id)
                failed = false
                message = "Done"
            } catch {
                failed = true
                message = Self.describe(error)
            }
            running = false
        }
    }

    private static func describe(_ error: Error) -> String {
        if let rpc = error as? RPCError {
            return rpc.msg.isEmpty ? rpc.code : rpc.msg
        }
        if let settingsError = error as? SettingsError, case .jobFailed(let msg) = settingsError {
            return msg.isEmpty ? "The pager reported a failure" : msg
        }
        return String(describing: error)
    }
}

#Preview {
    SettingsView()
        .environmentObject(PagerSession())
        .environmentObject(PagerSession().settings)
        .environmentObject(PagerSession().notes)
}
