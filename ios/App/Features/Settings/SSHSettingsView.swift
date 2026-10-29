import PagerKit
import SwiftUI
import UniformTypeIdentifiers

/// The pager's SSH client settings (S10): host, user, port and password
/// through the settings registry (`settings.set`), the saved-hosts list
/// (`ssh.hosts`) the pager's Connect offers first, and the private key —
/// imported from Files or pasted, kept in this phone's Keychain, and pushed
/// to the pager's write-only `ssh.key` secret (tried before the password).
/// The key never comes back from the pager. Keys over 3000 bytes or
/// passphrase-protected are refused here (`SSHKeyCheck`); the pager also
/// refuses any secret while notes encryption is off or locked.
struct SSHSettingsView: View {
    @EnvironmentObject private var settings: SettingsModel

    @State private var host = ""
    @State private var user = ""
    @State private var port = ""
    @State private var hosts: [SSHHostList.Host] = []
    @State private var hostDraft = SSHHostList.Host(name: "", user: "", host: "", port: 22)
    @State private var hostDraftPort = "22"
    @State private var password = ""
    @State private var keyDraft = ""
    @State private var storedKey: String?
    @State private var showImporter = false
    @State private var message: String?
    @State private var problem: String?

    private static let keychain = KeychainSecretStore(service: "PagerCompanion.ssh")
    private static let keyName = "ssh.key"

    var body: some View {
        Form {
            Section("Host") {
                TextField("Host", text: $host)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                TextField("User", text: $user)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                TextField("Port", text: $port)
                    .keyboardType(.numberPad)
                Button("Save host, user and port") { saveHost() }
                    .disabled(!settings.store.canEdit)
            }
            if settings.store.schema?.item("ssh.hosts") != nil {
                savedHostsSection
            }
            if settings.store.schema?.item("ssh.password") != nil {
                Section {
                    LabeledContent("On the pager", value: isSet("ssh.password") ? "Set" : "Not set")
                    SecureField("New password", text: $password)
                    Button("Send password") { send("ssh.password", password, done: "Password sent.") }
                        .disabled(password.isEmpty || !settings.store.canEdit)
                } header: {
                    Text("Password")
                }
            }
            if settings.store.schema?.item("ssh.key") != nil {
                keySection
            } else {
                Section {
                    Text("This pager's firmware has no SSH key setting; only the password is used.")
                        .foregroundStyle(.secondary)
                }
            }
            if let message {
                Section { Text(message).foregroundStyle(.green) }
            }
            if let problem = problem ?? settings.store.lastError {
                Section { Text(problem).foregroundStyle(.red) }
            }
        }
        .navigationTitle("SSH")
        .onAppear(perform: load)
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.item], allowsMultipleSelection: false) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            guard let data = try? Data(contentsOf: url), data.count <= 64 * 1024,
                  let text = String(data: data, encoding: .utf8) else {
                problem = "Could not read \(url.lastPathComponent) as a text key file."
                return
            }
            keyDraft = text
            checkDraft()
        }
    }

    private var savedHostsSection: some View {
        Section {
            ForEach(hosts, id: \.self) { h in
                VStack(alignment: .leading) {
                    Text(h.name)
                    Text(SSHHostList.line(h))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .onDelete { offsets in hosts.remove(atOffsets: offsets) }
            .onMove { from, to in hosts.move(fromOffsets: from, toOffset: to) }
            TextField("Name (optional)", text: $hostDraft.name)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            TextField("user", text: $hostDraft.user)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            TextField("host", text: $hostDraft.host)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            TextField("Port", text: $hostDraftPort)
                .keyboardType(.numberPad)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Add host") { addHost() }
                .disabled(hostDraft.user.isEmpty || hostDraft.host.isEmpty
                          || hosts.count >= SSHHostList.maxHosts)
            Button("Save list to pager") { saveHosts() }
                .disabled(!settings.store.canEdit)
        } header: {
            HStack {
                Text("Saved hosts")
                Spacer()
                EditButton()
            }
        } footer: {
            Text("Connect on the pager lists these first. Every host uses the same key and password; "
                 + "up to \(SSHHostList.maxHosts).")
        }
    }

    private var keySection: some View {
        Section {
            LabeledContent("On the pager", value: isSet("ssh.key") ? "Set" : "Not set")
            LabeledContent("On this phone", value: storedKey == nil ? "None" : "\(storedKey!.utf8.count) bytes")
            Button("Import key file…") { showImporter = true }
            TextEditor(text: $keyDraft)
                .font(.system(.caption2, design: .monospaced))
                .frame(minHeight: 120)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .onChange(of: keyDraft) { _ in problem = nil }
            Button("Keep this key on the phone") { keepDraft() }
                .disabled(keyDraft.isEmpty)
            Button("Push key to pager") { pushKey() }
                .disabled(storedKey == nil || !settings.store.canEdit)
            Button("Remove key from pager", role: .destructive) {
                send("ssh.key", "", done: "Key removed from the pager.")
            }
            .disabled(!isSet("ssh.key") || !settings.store.canEdit)
            if storedKey != nil {
                Button("Forget key on this phone", role: .destructive) {
                    Self.keychain.delete(Self.keyName)
                    storedKey = nil
                }
            }
        } header: {
            Text("Private key")
        } footer: {
            Text("An unencrypted OpenSSH or PEM key of at most \(SSHKeyCheck.maxBytes) bytes (ed25519 fits easily). "
                 + "It stays in this phone's Keychain and is sent only when you push it; the pager never sends it back. "
                 + "The pager takes it only while its notes encryption is on and unlocked.")
        }
    }

    // MARK: - Actions

    private func load() {
        host = settings.store.value("ssh.host")?.stringValue ?? ""
        user = settings.store.value("ssh.user")?.stringValue ?? ""
        port = settings.store.value("ssh.port")?.intValue.map(String.init) ?? "22"
        if case .success(let list) = SSHHostList.parse(settings.store.value("ssh.hosts")?.stringValue ?? "") {
            hosts = list
        }
        storedKey = Self.keychain.read(Self.keyName)
    }

    private func isSet(_ id: String) -> Bool {
        if case .secret(let set)? = settings.store.value(id) { return set }
        return false
    }

    @discardableResult
    private func checkDraft() -> String? {
        let key = SSHKeyCheck.normalize(keyDraft)
        problem = SSHKeyCheck.problem(key)
        return problem == nil ? key : nil
    }

    private func keepDraft() {
        guard let key = checkDraft() else { return }
        do {
            try Self.keychain.write(Self.keyName, key)
            storedKey = key
            keyDraft = ""
            message = "Key kept on this phone. Push it to use it on the pager."
        } catch {
            problem = "Could not store the key: \(error)"
        }
    }

    private func pushKey() {
        guard let key = storedKey else { return }
        if let issue = SSHKeyCheck.problem(key) {
            problem = issue
            return
        }
        send("ssh.key", key, done: "Key sent to the pager.")
    }

    private func saveHost() {
        guard let portNumber = Int(port), (1...65535).contains(portNumber) else {
            problem = "The port must be 1–65535."
            return
        }
        message = nil
        problem = nil
        Task {
            let ok = await settings.commit("ssh.host", .string(host.trimmingCharacters(in: .whitespaces)))
            let ok2 = ok ? await settings.commit("ssh.user", .string(user.trimmingCharacters(in: .whitespaces))) : false
            let ok3 = ok2 ? await settings.commit("ssh.port", .int(portNumber)) : false
            if ok3 { message = "Saved on the pager." }
        }
    }

    private func addHost() {
        let trimmedName = hostDraft.name.trimmingCharacters(in: .whitespaces)
        let trimmedUser = hostDraft.user.trimmingCharacters(in: .whitespaces)
        let trimmedHost = hostDraft.host.trimmingCharacters(in: .whitespaces)
        let candidate = hosts + [SSHHostList.Host(name: trimmedName.isEmpty ? trimmedHost : trimmedName,
                                                  user: trimmedUser,
                                                  host: trimmedHost,
                                                  port: Int(hostDraftPort) ?? -1)]
        // The pager's own parser decides: it names the line and the reason.
        switch SSHHostList.parse(SSHHostList.text(candidate)) {
        case .failure(let error):
            problem = error.message
        case .success(let list):
            hosts = list
            hostDraft = SSHHostList.Host(name: "", user: "", host: "", port: 22)
            hostDraftPort = "22"
            problem = nil
        }
    }

    private func saveHosts() {
        message = nil
        problem = nil
        Task {
            if await settings.commit("ssh.hosts", .string(SSHHostList.text(hosts))) {
                message = "Host list saved on the pager."
            }
        }
    }

    private func send(_ id: String, _ value: String, done: String) {
        message = nil
        problem = nil
        Task {
            if await settings.commit(id, .string(value)) {
                message = done
                if id == "ssh.password" { password = "" }
            }
        }
    }
}
