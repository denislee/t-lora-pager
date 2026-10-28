import Foundation
import PagerKit
import SwiftUI

// MARK: - Shared helpers

/// A settings failure as the UI shows it: the pager's own message when there
/// is one, else a short rendering of the local error.
private func describeSettingsFailure(_ error: Error) -> String {
    if let rpc = error as? RPCError {
        return rpc.msg.isEmpty ? rpc.code : rpc.msg
    }
    if let settingsError = error as? SettingsError, case .jobFailed(let message) = settingsError {
        return message.isEmpty ? "The pager reported a failure" : message
    }
    return String(describing: error)
}

private extension SettingValue {
    /// A `.json` array's elements, in order.
    var jsonArray: [JSONValue]? {
        guard case .json(.array(let items)) = self else { return nil }
        return items
    }

    /// A `.json` object's fields.
    var jsonObject: [String: JSONValue]? {
        guard case .json(.object(let object)) = self else { return nil }
        return object
    }
}

// MARK: - Wi-Fi

/// Wi-Fi (`wifi.*`): the pager's saved networks, a scan of what is around,
/// joining one, and an internet check. The pager owns every credential;
/// passwords travel to it over the encrypted link and are never read back.
struct WiFiNetworksView: View {
    @EnvironmentObject private var settings: SettingsModel

    @State private var networks: [ScannedNetwork] = []
    @State private var scanning = false
    @State private var scanError: String?

    @State private var testResult: InternetResult?
    @State private var testing = false
    @State private var testError: String?

    @State private var forgetting = false
    @State private var actionError: String?
    @State private var showForgetAll = false

    @State private var joinTarget: NetworkJoinTarget?

    /// `wifi.saved` is a read-only list of SSIDs, most recent first.
    private var savedSSIDs: [String] {
        settings.store.value("wifi.saved")?.jsonArray?.compactMap(\.stringValue) ?? []
    }

    private var canEdit: Bool { settings.store.canEdit }

    var body: some View {
        List {
            savedSection
            nearbySection
            internetSection
        }
        .navigationTitle("Wi-Fi networks")
        .refreshable { await settings.store.reload() }
        .sheet(item: $joinTarget) { target in
            AddNetworkForm(ssid: target.ssid, secure: target.secure)
                .environmentObject(settings)
        }
        .confirmationDialog(
            "Forget all saved networks?",
            isPresented: $showForgetAll,
            titleVisibility: .visible
        ) {
            Button("Forget all", role: .destructive) { forgetAll() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The pager drops every saved Wi-Fi password. This cannot be undone.")
        }
    }

    // MARK: Saved

    private var savedSection: some View {
        Section("Saved") {
            if savedSSIDs.isEmpty {
                Text("No saved networks")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(savedSSIDs, id: \.self) { ssid in
                    Text(ssid)
                        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                            Button(role: .destructive) {
                                forget(ssid)
                            } label: {
                                Text("Forget")
                            }
                            .disabled(!canEdit || forgetting)
                        }
                        .contextMenu {
                            Button("Forget", role: .destructive) {
                                forget(ssid)
                            }
                            .disabled(!canEdit || forgetting)
                        }
                }
                Button("Forget all", role: .destructive) {
                    showForgetAll = true
                }
                .disabled(!canEdit || forgetting)
            }
            if forgetting {
                HStack(spacing: 8) {
                    ProgressView()
                    Text("Updating the pager…")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            if let actionError {
                Text(actionError)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
    }

    // MARK: Nearby

    private var nearbySection: some View {
        Section("Nearby") {
            HStack(spacing: 8) {
                Button("Scan") { scan() }
                    .disabled(scanning || !canEdit)
                if scanning {
                    ProgressView()
                }
            }
            ForEach(networks) { network in
                Button {
                    joinTarget = NetworkJoinTarget(ssid: network.ssid, secure: network.secure)
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: network.secure ? "lock.fill" : "lock.open")
                            .foregroundStyle(.secondary)
                        Text(network.ssid)
                        Spacer()
                        Text("\(network.rssi) dBm")
                            .font(.caption)
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!canEdit || scanning)
            }
            if let scanError {
                Text(scanError)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
            Button("Join other network…") {
                joinTarget = NetworkJoinTarget(ssid: nil, secure: true)
            }
            .disabled(!canEdit || scanning)
        }
    }

    // MARK: Internet

    private var internetSection: some View {
        Section {
            HStack(spacing: 8) {
                Button("Test") { testInternet() }
                    .disabled(testing || !canEdit)
                if testing {
                    ProgressView()
                }
                Spacer()
                if let testResult {
                    Text(testResult.label)
                        .font(.caption)
                        .foregroundStyle(testResult.ok ? Color.green : Color.secondary)
                }
            }
            if let testError {
                Text(testError)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        } header: {
            Text("Internet")
        } footer: {
            Text("Passwords go to the pager over the encrypted link and are never sent back.")
        }
    }

    // MARK: Actions

    private func scan() {
        guard canEdit, !scanning else { return }
        scanning = true
        scanError = nil
        networks = []
        Task {
            do {
                let result = try await settings.run("wifi.scan")
                if let found = Self.networks(from: result) {
                    networks = found
                } else {
                    scanError = "Unexpected scan result"
                }
            } catch {
                scanError = describeSettingsFailure(error)
            }
            scanning = false
        }
    }

    private func forget(_ ssid: String) {
        guard canEdit, !forgetting else { return }
        forgetting = true
        actionError = nil
        Task {
            do {
                _ = try await settings.run("wifi.forget", params: ["ssid": .string(ssid)])
                await settings.store.reload()
            } catch {
                actionError = describeSettingsFailure(error)
            }
            forgetting = false
        }
    }

    private func forgetAll() {
        guard canEdit, !forgetting else { return }
        forgetting = true
        actionError = nil
        Task {
            do {
                _ = try await settings.run("wifi.forget", params: ["all": .bool(true)])
                await settings.store.reload()
            } catch {
                actionError = describeSettingsFailure(error)
            }
            forgetting = false
        }
    }

    private func testInternet() {
        guard canEdit, !testing else { return }
        testing = true
        testError = nil
        testResult = nil
        Task {
            do {
                let result = try await settings.run("wifi.test")
                if case .object(let object) = result, let ok = object["ok"]?.boolValue {
                    testResult = InternetResult(ok: ok, ms: object["ms"]?.intValue ?? 0)
                } else {
                    testError = "Unexpected test result"
                }
            } catch {
                testError = describeSettingsFailure(error)
            }
            testing = false
        }
    }

    /// `wifi.scan` answers with `[{ssid, rssi, secure}]`; nil means the pager
    /// sent something else.
    private static func networks(from result: JSONValue) -> [ScannedNetwork]? {
        guard case .array(let items) = result else { return nil }
        return items.compactMap { item in
            guard let ssid = item["ssid"]?.stringValue else { return nil }
            return ScannedNetwork(
                ssid: ssid,
                rssi: item["rssi"]?.intValue ?? 0,
                secure: item["secure"]?.boolValue ?? false
            )
        }
    }
}

// MARK: - Joining one network

/// The password form for one network. A scanned network's SSID is fixed; the
/// "other network" form lets the user type it. An open network asks for no
/// password at all.
private struct AddNetworkForm: View {
    @EnvironmentObject private var settings: SettingsModel
    @Environment(\.dismiss) private var dismiss

    private let initialSSID: String?
    private let secure: Bool

    @State private var ssid: String
    @State private var password = ""
    @State private var joining = false
    @State private var error: String?

    init(ssid: String?, secure: Bool) {
        initialSSID = ssid
        self.secure = secure
        _ssid = State(initialValue: ssid ?? "")
    }

    private var editingSSID: Bool { initialSSID == nil }

    private var canJoin: Bool {
        settings.store.canEdit
            && !joining
            && !ssid.isEmpty
            && !(secure && password.isEmpty)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    if editingSSID {
                        TextField("Network name (SSID)", text: $ssid)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                    } else {
                        LabeledContent("Network", value: ssid)
                    }
                    if secure {
                        SecureField("Password", text: $password)
                    }
                } footer: {
                    Text("The password goes to the pager over the encrypted link and is never sent back.")
                }
                if joining {
                    Section {
                        HStack(spacing: 8) {
                            ProgressView()
                            Text("Connecting…")
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                if let error {
                    Section {
                        Text(error).foregroundStyle(.red)
                    }
                }
                Section {
                    Button("Join") { join() }
                        .disabled(!canJoin)
                }
            }
            .navigationTitle("Join Wi-Fi")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }

    private func join() {
        guard canJoin else { return }
        let name = ssid
        let secret = password
        joining = true
        error = nil
        Task {
            do {
                _ = try await settings.run("wifi.add", params: [
                    "ssid": .string(name),
                    "password": .string(secret),
                ])
                await settings.store.reload()
                joining = false
                dismiss()
            } catch {
                self.error = describeSettingsFailure(error)
                joining = false
            }
        }
    }
}

// MARK: - Time zone

/// The pager's `time.tz` enum, with the phone's own zone offered first when
/// the pager supports it. The `""` option is the pager's built-in default.
struct TimeZonePickerView: View {
    @EnvironmentObject private var settings: SettingsModel
    @Environment(\.dismiss) private var dismiss

    @State private var query = ""
    @State private var saving = false
    @State private var error: String?

    private var options: [TimeZoneOption] {
        guard let item = settings.store.schema?.item("time.tz") else { return [] }
        return item.options.compactMap { option in
            guard case .string(let value) = option.value else { return nil }
            return TimeZoneOption(value: value, label: option.label)
        }
    }

    private var currentZone: String {
        settings.store.value("time.tz")?.stringValue ?? ""
    }

    private var phoneZone: String { TimeZone.current.identifier }

    private var phoneZoneIsOption: Bool {
        options.contains { $0.value == phoneZone }
    }

    private var phoneLabel: String {
        "Use this phone's zone (\(phoneZone))"
    }

    /// The pager lists the default (`""`) first; keep it there even if a
    /// catalog ever sends it elsewhere.
    private var orderedOptions: [TimeZoneOption] {
        let all = options
        guard let index = all.firstIndex(where: { $0.value.isEmpty }), index > 0 else {
            return all
        }
        var ordered = all
        let fallback = ordered.remove(at: index)
        ordered.insert(fallback, at: 0)
        return ordered
    }

    private var visibleOptions: [TimeZoneOption] {
        guard !query.isEmpty else { return orderedOptions }
        return orderedOptions.filter { option in
            option.label.localizedCaseInsensitiveContains(query)
                || option.value.localizedCaseInsensitiveContains(query)
        }
    }

    private var showPhoneRow: Bool {
        guard phoneZoneIsOption else { return false }
        guard !query.isEmpty else { return true }
        return phoneLabel.localizedCaseInsensitiveContains(query)
            || phoneZone.localizedCaseInsensitiveContains(query)
    }

    var body: some View {
        List {
            if showPhoneRow {
                zoneRow(value: phoneZone, label: phoneLabel)
            }
            ForEach(visibleOptions) { option in
                zoneRow(value: option.value, label: option.label)
            }
            if visibleOptions.isEmpty, !showPhoneRow {
                Text(query.isEmpty ? "No time zones available" : "No matching time zones")
                    .foregroundStyle(.secondary)
            }
            if saving {
                Section {
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("Saving…")
                            .foregroundStyle(.secondary)
                    }
                }
            }
            if let error {
                Section {
                    Text(error).foregroundStyle(.red)
                }
            }
        }
        .navigationTitle("Time zone")
        .searchable(text: $query, prompt: "Search time zones")
    }

    private func zoneRow(value: String, label: String) -> some View {
        Button {
            select(value)
        } label: {
            HStack {
                Text(label)
                Spacer()
                if value == currentZone {
                    Image(systemName: "checkmark")
                        .foregroundStyle(Color.accentColor)
                }
            }
            .contentShape(Rectangle())
        }
        .disabled(!settings.store.canEdit || saving)
    }

    private func select(_ value: String) {
        guard settings.store.canEdit, !saving else { return }
        saving = true
        error = nil
        Task {
            if await settings.commit("time.tz", .string(value)) {
                dismiss()
            } else {
                error = settings.store.lastError ?? "Could not set the time zone"
            }
            saving = false
        }
    }
}

// MARK: - Weather location

/// The pager's `weather.location` object: either the automatic (IP/phone)
/// location or a city found through Open-Meteo's geocoder. The phone only
/// resolves the city; the pager fetches and stores the forecast.
struct WeatherLocationView: View {
    @EnvironmentObject private var settings: SettingsModel
    @Environment(\.dismiss) private var dismiss

    @State private var query = ""
    @State private var matches: [CityMatch] = []
    @State private var searching = false
    @State private var saving = false
    @State private var error: String?

    private var location: [String: JSONValue]? {
        settings.store.value("weather.location")?.jsonObject
    }

    private var isAuto: Bool {
        location?["auto"]?.boolValue == true
    }

    private var currentLabel: String {
        if location == nil || isAuto { return "Auto (IP location)" }
        return location?["name"]?.stringValue ?? "Custom location"
    }

    var body: some View {
        List {
            Section("Location") {
                LabeledContent("Current", value: currentLabel)
                Button("Use auto") { useAuto() }
                    .disabled(!settings.store.canEdit || saving)
                if isAuto {
                    Text("The pager asks this phone for its location and falls back to an IP lookup.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                if saving {
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("Saving…")
                            .foregroundStyle(.secondary)
                    }
                }
            }
            Section {
                HStack {
                    TextField("City", text: $query)
                        .textInputAutocapitalization(.words)
                        .autocorrectionDisabled()
                        .submitLabel(.search)
                        .onSubmit(search)
                    Button("Search") { search() }
                        .disabled(searching || query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
                if searching {
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("Searching…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                ForEach(matches) { match in
                    Button {
                        choose(match)
                    } label: {
                        Text(match.label.isEmpty ? match.name : match.label)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .disabled(!settings.store.canEdit || saving)
                }
                if let error {
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.red)
                }
            } header: {
                Text("City")
            } footer: {
                Text("The phone looks the city up; the pager fetches the forecast for it.")
            }
        }
        .navigationTitle("Weather location")
    }

    private func search() {
        let term = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !term.isEmpty, !searching else { return }
        searching = true
        error = nil
        Task {
            do {
                matches = try await WeatherService.searchCities(term)
                if matches.isEmpty {
                    error = "No cities found"
                }
            } catch {
                self.error = describeSettingsFailure(error)
            }
            searching = false
        }
    }

    private func useAuto() {
        guard settings.store.canEdit, !saving else { return }
        saving = true
        error = nil
        Task {
            let saved = await settings.commit(
                "weather.location", .json(.object(["auto": .bool(true)])))
            if !saved {
                error = settings.store.lastError ?? "Could not use the automatic location"
            }
            saving = false
        }
    }

    private func choose(_ match: CityMatch) {
        guard settings.store.canEdit, !saving else { return }
        saving = true
        error = nil
        let value = SettingValue.json(.object([
            "name": .string(match.name),
            "lat": .number(match.lat),
            "lon": .number(match.lon),
        ]))
        Task {
            if await settings.commit("weather.location", value) {
                saving = false
                dismiss()
            } else {
                error = settings.store.lastError ?? "Could not save the location"
                saving = false
            }
        }
    }
}

// MARK: - Telegram favorites

/// `tg.favorites`: the chats the pager's Telegram app pins. When the phone is
/// logged in, the list is picked from its own chat list; otherwise only the
/// favorites stored on the pager can be removed.
struct TelegramFavoritesView: View {
    @EnvironmentObject private var settings: SettingsModel
    @EnvironmentObject private var session: PagerSession

    @State private var loading = true
    @State private var phoneReady = false
    @State private var chats: [TelegramChat] = []
    @State private var error: String?

    private var canEdit: Bool { settings.store.canEdit }

    /// `tg.favorites` is `[{id, title}]`.
    private var favorites: [FavoriteChat] {
        guard let items = settings.store.value("tg.favorites")?.jsonArray else { return [] }
        return items.compactMap { item in
            guard let id = item["id"]?.intValue else { return nil }
            return FavoriteChat(id: Int64(id), title: item["title"]?.stringValue ?? "")
        }
    }

    var body: some View {
        List {
            if loading {
                Section {
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("Looking for your Telegram chats…")
                            .foregroundStyle(.secondary)
                    }
                }
            } else if phoneReady {
                chatsSection
            } else {
                favoritesSection
                Section {
                    Text("Log in to Telegram in the Telegram tab to pick from your chats.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            if let error {
                Section {
                    Text(error).foregroundStyle(.red)
                }
            }
            Section {
            } footer: {
                Text("When the phone is logged in, the pager's Telegram goes through the phone; "
                    + "the bridge URL and token only matter for its direct Wi-Fi path.")
            }
        }
        .navigationTitle("Favorite chats")
        .task { await loadChats() }
    }

    // MARK: Sections

    private var chatsSection: some View {
        Section("Your chats") {
            if chats.isEmpty {
                Text("No chats found")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(chats) { chat in
                    Toggle(isOn: Binding(
                        get: { isFavorite(chat.id) },
                        set: { setFavorite(chat, on: $0) }
                    )) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(chat.title.isEmpty ? "Chat \(chat.id)" : chat.title)
                            if chat.unread > 0 {
                                Text("\(chat.unread) unread")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                    .disabled(!canEdit)
                }
            }
        }
    }

    private var favoritesSection: some View {
        Section("Favorites") {
            if favorites.isEmpty {
                Text("No favorite chats yet")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(favorites) { favorite in
                    HStack {
                        Text(favorite.title.isEmpty ? "Chat \(favorite.id)" : favorite.title)
                        Spacer()
                        Button("Remove", role: .destructive) {
                            remove(favorite)
                        }
                        .disabled(!canEdit)
                    }
                }
            }
        }
    }

    // MARK: Actions

    private func loadChats() async {
        loading = true
        error = nil
        defer { loading = false }
        guard let native = session.telegramNative else {
            phoneReady = false
            return
        }
        guard await native.isReady() else {
            phoneReady = false
            return
        }
        phoneReady = true
        do {
            let reply = try await native.chats(limit: 100)
            guard case .array(let items) = reply else {
                error = "Unexpected chat list from Telegram"
                return
            }
            chats = items.compactMap { item in
                guard let id = item["id"]?.intValue else { return nil }
                return TelegramChat(
                    id: Int64(id),
                    title: item["title"]?.stringValue ?? "",
                    unread: item["unread"]?.intValue ?? 0
                )
            }
        } catch {
            self.error = describeSettingsFailure(error)
        }
    }

    private func isFavorite(_ id: Int64) -> Bool {
        favorites.contains { $0.id == id }
    }

    /// Toggling adds the chat with its own title, or removes it by id; the
    /// titles of favorites that stay are left untouched.
    private func setFavorite(_ chat: TelegramChat, on: Bool) {
        guard canEdit else { return }
        var next = favorites
        if on {
            guard !next.contains(where: { $0.id == chat.id }) else { return }
            next.append(FavoriteChat(id: chat.id, title: chat.title))
        } else {
            next.removeAll { $0.id == chat.id }
        }
        commit(next)
    }

    private func remove(_ favorite: FavoriteChat) {
        guard canEdit else { return }
        commit(favorites.filter { $0.id != favorite.id })
    }

    private func commit(_ next: [FavoriteChat]) {
        let value = SettingValue.json(.array(next.map { favorite in
            JSONValue.object([
                "id": .number(Double(favorite.id)),
                "title": .string(favorite.title),
            ])
        }))
        settings.store.draft("tg.favorites", value)
        error = nil
        Task {
            let saved = await settings.commit("tg.favorites", value)
            if !saved {
                error = settings.store.lastError ?? "Could not save favorite chats"
                // `commit` leaves a draft behind when the pager refuses the
                // value; drop ours so the toggle shows what was really saved.
                settings.store.cancelEdit("tg.favorites")
            }
        }
    }
}

// MARK: - Small value types

private struct ScannedNetwork: Identifiable {
    let id: UUID
    let ssid: String
    let rssi: Int
    let secure: Bool

    init(ssid: String, rssi: Int, secure: Bool) {
        id = UUID()
        self.ssid = ssid
        self.rssi = rssi
        self.secure = secure
    }
}

private struct NetworkJoinTarget: Identifiable {
    let id: UUID
    /// nil opens the form with an editable SSID.
    let ssid: String?
    let secure: Bool

    init(ssid: String?, secure: Bool) {
        id = UUID()
        self.ssid = ssid
        self.secure = secure
    }
}

private struct InternetResult {
    let ok: Bool
    let ms: Int

    var label: String {
        ok ? "Online (\(ms) ms)" : "Offline"
    }
}

private struct TimeZoneOption: Identifiable {
    let value: String
    let label: String

    var id: String { value }
}

private struct TelegramChat: Identifiable {
    let id: Int64
    let title: String
    let unread: Int
}

private struct FavoriteChat: Identifiable {
    let id: Int64
    let title: String
}
