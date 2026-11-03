import PagerKit
import SwiftUI

/// The phone's GitHub notes connection (`IOS_NOTES_VIEWER_PLAN.md` §3.7):
/// repo, branch and a fine-grained PAT. The repo and branch live in
/// UserDefaults and the token in the Keychain, so the token is never shown
/// back.
///
/// Set up in two steps: a token (GitHub's token page opens already filled in
/// with the one permission notes need), then a repository — picked from the
/// ones the token reaches, or pasted in any form (`owner/name`, a github.com
/// link, a clone URL). Connect checks both against GitHub, takes the repo's
/// default branch when none is given, and can copy the result to the pager's
/// own Notes Sync settings so it also syncs on its own Wi-Fi.
struct GitHubSettingsView: View {
    @EnvironmentObject private var notes: NotesModel
    @EnvironmentObject private var settings: SettingsModel
    @Environment(\.dismiss) private var dismiss

    @State private var repo = ""
    @State private var branch = ""
    @State private var token = ""
    @State private var status = ""
    @State private var statusIsError = false
    @State private var working = false
    @State private var copyToPager = true
    @State private var repoChoices: [GitHubRepoInfo] = []
    @State private var showingRepoChoices = false

    var body: some View {
        NavigationStack {
            Form {
                if notes.account.isConnected {
                    Section {
                        Label("\(notes.account.repo ?? "")  ·  \(notes.account.branch)",
                              systemImage: "checkmark.seal.fill")
                            .foregroundStyle(.green)
                    } header: {
                        Text("Connected")
                    }
                }
                tokenSection
                repoSection
                connectSection
                cacheSection
            }
            .navigationTitle("GitHub")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .onAppear(perform: prefill)
            .sheet(isPresented: $showingRepoChoices) { repoPicker }
        }
    }

    // MARK: - Sections

    private var tokenSection: some View {
        Section {
            Link(destination: GitHubSetup.tokenCreationURL(owner: owner)) {
                Label("Create a token on GitHub", systemImage: "key.viewfinder")
            }
            HStack {
                SecureField(tokenPlaceholder, text: $token)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                PasteButton(payloadType: String.self) { strings in
                    if let pasted = strings.first { token = pasted.trimmingCharacters(in: .whitespacesAndNewlines) }
                }
                .labelStyle(.iconOnly)
                .buttonBorderShape(.capsule)
            }
        } header: {
            Text("1. Token")
        } footer: {
            Text(
                "The link opens GitHub with the name, expiry and \"Contents: Read and write\" filled in. "
                    + "Under Repository access choose \"Only select repositories\" and pick your notes repo, "
                    + "then Generate and copy the token. It stays in this phone's Keychain.")
        }
    }

    private var repoSection: some View {
        Section {
            TextField("owner/name or a GitHub link", text: $repo)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.URL)
                .onChange(of: repo) { value in normalizeRepo(value) }
            Button {
                loadRepos()
            } label: {
                Label("Choose from my repositories", systemImage: "list.bullet")
            }
            .disabled(effectiveToken == nil || working)
            TextField("Branch", text: $branch, prompt: Text("Repository's default branch"))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
        } header: {
            Text("2. Repository")
        } footer: {
            Text("Notes go into its notes/ folder. Leave the branch empty to use the default one.")
        }
    }

    private var connectSection: some View {
        Section {
            if pagerCanTakeGitHub {
                Toggle("Also set up the pager", isOn: $copyToPager)
            }
            Button {
                connect()
            } label: {
                HStack {
                    Text(notes.account.isConnected ? "Save & check" : "Connect")
                    if working { Spacer(); ProgressView() }
                }
            }
            .disabled(working || repo.isEmpty || effectiveToken == nil)
            Button("Test connection") { test() }
                .disabled(!notes.account.isConnected || working)
            if !status.isEmpty {
                Text(status)
                    .font(.footnote)
                    .foregroundStyle(statusIsError ? Color.red : Color.secondary)
            }
            if notes.account.isConnected {
                Button("Disconnect", role: .destructive) { disconnect() }
            }
        } footer: {
            if pagerCanTakeGitHub {
                Text(
                    "\"Also set up the pager\" copies the repo, branch and token to the pager's Notes Sync "
                        + "settings, so it can sync over its own Wi-Fi when the phone isn't around.")
            }
        }
    }

    private var cacheSection: some View {
        Section {
            Button("Clear cached notes") {
                notes.clearCache()
                status = "Cached notes cleared"
                statusIsError = false
            }
        } header: {
            Text("On this phone")
        } footer: {
            Text(
                "Cached copies are what offline reading shows. Encrypted notes stay "
                    + "ciphertext on disk.")
        }
    }

    private var repoPicker: some View {
        NavigationStack {
            List(repoChoices) { choice in
                Button {
                    repo = choice.fullName
                    branch = choice.defaultBranch
                    showingRepoChoices = false
                } label: {
                    HStack {
                        VStack(alignment: .leading) {
                            Text(choice.fullName)
                            Text(choice.defaultBranch).font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if choice.isPrivate { Image(systemName: "lock").foregroundStyle(.secondary) }
                    }
                }
                .foregroundStyle(.primary)
            }
            .navigationTitle("Repositories")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { showingRepoChoices = false }
                }
            }
        }
    }

    // MARK: - State

    private var tokenPlaceholder: String {
        notes.account.hasToken ? "Token saved — paste to replace" : "Paste the token"
    }

    /// The typed token, else the one in the Keychain.
    private var effectiveToken: String? {
        let typed = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if !typed.isEmpty { return typed }
        return notes.account.config()?.token
    }

    /// The owner part of the repo field, to preselect it on GitHub's token page.
    private var owner: String? {
        GitHubSetup.parseRepo(repo).map { String($0.repo.split(separator: "/")[0]) }
    }

    /// A live pager whose catalog has the Notes Sync settings.
    private var pagerCanTakeGitHub: Bool {
        settings.store.canEdit && settings.store.schema?.item("nsync.repo") != nil
    }

    /// The phone's saved repo, else what the pager's Notes Sync already uses.
    private func prefill() {
        if let saved = notes.account.repo {
            repo = saved
            branch = notes.account.branch
            return
        }
        if let pagerRepo = settings.store.value("nsync.repo")?.stringValue, !pagerRepo.isEmpty {
            repo = pagerRepo
            branch = settings.store.value("nsync.branch")?.stringValue ?? ""
        }
    }

    /// A pasted link becomes `owner/name` (and its `/tree/<branch>`).
    private func normalizeRepo(_ value: String) {
        guard let parsed = GitHubSetup.parseRepo(value), parsed.repo != value else { return }
        repo = parsed.repo
        if let linkBranch = parsed.branch { branch = linkBranch }
    }

    // MARK: - Actions

    private func loadRepos() {
        guard let token = effectiveToken else { return }
        working = true
        status = "Loading repositories…"
        statusIsError = false
        Task {
            do {
                repoChoices = try await GitHubSetup.repositories(token: token)
                if repoChoices.isEmpty {
                    status = "The token reaches no repositories. Grant it one on GitHub."
                    statusIsError = true
                } else {
                    status = ""
                    showingRepoChoices = true
                }
            } catch {
                status = describe(error)
                statusIsError = true
            }
            working = false
        }
    }

    /// Check repo + branch with the token, save, and optionally copy to the pager.
    private func connect() {
        guard let parsed = GitHubSetup.parseRepo(repo) else {
            status = "Enter the repository as owner/name or paste its GitHub link"
            statusIsError = true
            return
        }
        guard let token = effectiveToken else { return }
        let typedBranch = branch.trimmingCharacters(in: .whitespacesAndNewlines)
        let copy = copyToPager && pagerCanTakeGitHub
        working = true
        status = "Checking with GitHub…"
        statusIsError = false
        Task {
            defer { working = false }
            do {
                let info = try await GitHubSetup.inspect(
                    repo: parsed.repo, branch: typedBranch.isEmpty ? nil : typedBranch, token: token)
                let useBranch = typedBranch.isEmpty ? info.defaultBranch : typedBranch
                try notes.account.save(repo: info.fullName, branch: useBranch,
                                       token: self.token.isEmpty ? nil : token)
                self.token = ""
                repo = info.fullName
                branch = useBranch
                var done = "Connected to \(info.fullName) (\(useBranch))"
                if copy, let config = notes.account.config() {
                    if await settings.copyGitHub(config) {
                        done += " — pager set up too"
                    } else {
                        status = done + ". The pager refused: " + (settings.store.lastError ?? "unknown error")
                        statusIsError = true
                        return
                    }
                }
                status = done
                statusIsError = false
            } catch {
                status = describe(error)
                statusIsError = true
            }
        }
    }

    /// Lists `notes/` in the repo: a revoked or wrong token shows the GitHub
    /// error (e.g. `github 401: …`).
    private func test() {
        let account = notes.account
        working = true
        status = "Testing…"
        statusIsError = false
        Task {
            do {
                let backend = GitHubNotesBackend(config: { [account] in account.config() })
                let count = try await backend.existingBlobs().count
                status = "OK — \(count) notes in \(account.repo ?? "")@\(account.branch)"
                statusIsError = false
            } catch {
                status = describe(error)
                statusIsError = true
            }
            working = false
        }
    }

    private func disconnect() {
        notes.account.disconnect()
        repo = ""
        branch = ""
        token = ""
        status = "Disconnected"
        statusIsError = false
    }

    private func describe(_ error: Error) -> String {
        if let error = error as? GitHubAccountError { return error.msg }
        if let error = error as? NotesSyncError { return error.msg }
        return error.localizedDescription
    }
}

#Preview {
    let session = PagerSession()
    return GitHubSettingsView()
        .environmentObject(session.notes)
        .environmentObject(session.settings)
}
