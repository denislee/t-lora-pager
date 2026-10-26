import PagerKit
import SwiftUI

/// The phone's GitHub notes connection (`IOS_NOTES_VIEWER_PLAN.md` §3.7):
/// repo, branch and a fine-grained PAT. The repo and branch live in
/// UserDefaults and the token in the Keychain, so the token is never shown
/// back and never sent to the pager.
struct GitHubSettingsView: View {
    @EnvironmentObject private var notes: NotesModel
    @Environment(\.dismiss) private var dismiss

    @State private var repo = ""
    @State private var branch = ""
    @State private var token = ""
    @State private var status = ""
    @State private var statusIsError = false
    @State private var testing = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("owner/name", text: $repo)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    TextField("Branch", text: $branch, prompt: Text("main"))
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField(tokenPlaceholder, text: $token)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    if !status.isEmpty {
                        Text(status)
                            .font(.footnote)
                            .foregroundStyle(statusIsError ? Color.red : Color.secondary)
                    }
                } header: {
                    Text("Repository")
                } footer: {
                    Text(
                        "The token stays in this phone's Keychain and is never sent to the pager. "
                            + "If you set the repo up on the pager before, enter it again here once.")
                }

                Section {
                    Button("Save") { save() }
                    Button("Test connection") { test() }
                        .disabled(!notes.account.isConnected || testing)
                    if notes.account.isConnected {
                        Button("Disconnect", role: .destructive) { disconnect() }
                    }
                }

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
            .navigationTitle("GitHub")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .onAppear {
                repo = notes.account.repo ?? ""
                branch = notes.account.branch
            }
        }
    }

    private var tokenPlaceholder: String {
        notes.account.hasToken ? "Replace token (optional)" : "Fine-grained token (contents: write)"
    }

    // MARK: - Actions

    private func save() {
        do {
            try notes.account.save(
                repo: repo, branch: branch,
                token: token.isEmpty ? nil : token)
            token = ""
            status = "Saved"
            statusIsError = false
        } catch let error as GitHubAccountError {
            status = error.msg
            statusIsError = true
        } catch {
            status = "\(error)"
            statusIsError = true
        }
    }

    /// Lists `notes/` in the repo: a revoked or wrong token shows the GitHub
    /// error (e.g. `github 401: …`).
    private func test() {
        let account = notes.account
        testing = true
        status = "Testing…"
        statusIsError = false
        Task {
            do {
                let backend = GitHubNotesBackend(config: { [account] in account.config() })
                let count = try await backend.existingBlobs().count
                status = "OK — \(count) notes in \(account.repo ?? "")@\(account.branch)"
                statusIsError = false
            } catch {
                status = (error as? NotesSyncError)?.msg ?? "\(error)"
                statusIsError = true
            }
            testing = false
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
}

#Preview {
    GitHubSettingsView()
        .environmentObject(PagerSession().notes)
}
