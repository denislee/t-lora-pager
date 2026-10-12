import SwiftUI

/// Log the phone into Telegram so it can answer the pager's Telegram app
/// directly (no tg-bridge server).
struct TelegramView: View {
    @EnvironmentObject private var account: TelegramAccount

    @State private var apiID = ""
    @State private var apiHash = ""
    @State private var phone = ""
    @State private var code = ""
    @State private var password = ""

    var body: some View {
        NavigationStack {
            Form {
                content
                if let error = account.lastError {
                    Section {
                        Text(error).foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle("Telegram")
            .disabled(account.busy)
            .onAppear {
                apiID = account.apiID
                apiHash = account.apiHash
            }
        }
    }

    @ViewBuilder
    private var content: some View {
        switch account.phase {
        case .needsKeys:
            Section {
                TextField("api_id", text: $apiID)
                    .keyboardType(.numberPad)
                TextField("api_hash", text: $apiHash)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Continue") { account.saveKeys(apiID: apiID, apiHash: apiHash) }
                    .disabled(apiID.isEmpty || apiHash.isEmpty)
            } header: {
                Text("App credentials")
            } footer: {
                Text("Create them once at my.telegram.org → API development tools. "
                    + "They stay on this phone.")
            }
        case .starting:
            Section {
                HStack {
                    ProgressView()
                    Text("Connecting to Telegram…")
                }
            }
        case .waitPhone:
            Section {
                TextField("+55 11 91234 5678", text: $phone)
                    .keyboardType(.phonePad)
                Button("Send code") { account.submitPhone(phone) }
                    .disabled(phone.isEmpty)
            } header: {
                Text("Phone number")
            }
        case .waitCode:
            Section {
                TextField("Code", text: $code)
                    .keyboardType(.numberPad)
                    .textContentType(.oneTimeCode)
                Button("Log in") { account.submitCode(code) }
                    .disabled(code.isEmpty)
            } header: {
                Text("Login code")
            } footer: {
                Text("Telegram sent it to your other Telegram apps, or by SMS.")
            }
        case .waitPassword(let hint):
            Section {
                SecureField(hint.isEmpty ? "Password" : "Password (hint: \(hint))", text: $password)
                Button("Log in") { account.submitPassword(password) }
                    .disabled(password.isEmpty)
            } header: {
                Text("Two-step verification")
            }
        case .ready(let name):
            Section {
                Label(name.isEmpty ? "Logged in" : "Logged in as \(name)", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
            } footer: {
                Text("The pager's Telegram app now goes through this phone while it is connected. "
                    + "No bridge server is needed.")
            }
            Section {
                Button("Log out", role: .destructive) { account.logOut() }
            }
        case .unsupported(let message):
            Section {
                Text(message)
            }
        }
    }
}
