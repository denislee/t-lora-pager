import PagerKit
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// "Share → Pager" (S4): takes shared text and/or a web URL and queues it as
/// a pager message. An extension can't use the app's Bluetooth link, so the
/// message goes into the App Group outbox (`PagerOutboxStore`) with a stable
/// id, and the app sends it with `notify.post` the next time it is connected
/// (on connect and on every return to the foreground); a retry is deduped by
/// the pager. The sheet says so.
final class ShareViewController: UIViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        let model = ShareModel()
        model.finish = { [weak self] in self?.extensionContext?.completeRequest(returningItems: nil) }
        model.cancel = { [weak self] in
            self?.extensionContext?.cancelRequest(withError: NSError(domain: "PagerShare", code: 0))
        }
        let host = UIHostingController(rootView: ShareView(model: model))
        addChild(host)
        host.view.frame = view.bounds
        host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(host.view)
        host.didMove(toParent: self)

        let providers = (extensionContext?.inputItems as? [NSExtensionItem] ?? []).flatMap { $0.attachments ?? [] }
        Task { await model.load(providers) }
    }
}

/// The share sheet's state: what was shared, turned into a pager message.
@MainActor
final class ShareModel: ObservableObject {
    @Published var title = ""
    @Published var body = ""
    @Published var prio: NotifyPriority = .normal
    @Published private(set) var loaded = false
    @Published private(set) var problem: String?

    var finish: () -> Void = {}
    var cancel: () -> Void = {}

    private let outbox: PagerOutboxStore? = {
        let group = PagerStatusStore.appGroupID(bundleID: Bundle.main.bundleIdentifier ?? "")
        return UserDefaults(suiteName: group).map(PagerOutboxStore.init(defaults:))
    }()

    func load(_ providers: [NSItemProvider]) async {
        var text: String?
        var url: URL?
        for provider in providers {
            if url == nil, provider.hasItemConformingToTypeIdentifier(UTType.url.identifier) {
                url = await Self.loadURL(provider)
            } else if text == nil, provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier) {
                text = await Self.loadText(provider)
            }
        }
        let message = NotifyMessage.shared(text: text, url: url, src: "Share")
        title = message.title
        body = message.body ?? ""
        loaded = true
    }

    /// The message as it will be queued (cut to the pager's limits).
    var message: NotifyMessage {
        var m = NotifyMessage.shared(text: title + "\n" + body, url: nil, src: "Share", id: NotifyMessage.newID(prefix: "share"))
        m.prio = prio
        return m
    }

    func send() {
        guard let outbox else {
            problem = "Pager Companion's shared storage is unavailable (App Group not set up)."
            return
        }
        let m = message
        if let issue = MessagingRules.check("notify.post", .object(m.params)) {
            problem = issue
            return
        }
        outbox.enqueue(OutboxMessage(m))
        finish()
    }

    private static func loadURL(_ provider: NSItemProvider) async -> URL? {
        await withCheckedContinuation { continuation in
            _ = provider.loadObject(ofClass: URL.self) { url, _ in continuation.resume(returning: url) }
        }
    }

    private static func loadText(_ provider: NSItemProvider) async -> String? {
        await withCheckedContinuation { continuation in
            _ = provider.loadObject(ofClass: String.self) { text, _ in continuation.resume(returning: text) }
        }
    }
}

private struct ShareView: View {
    @ObservedObject var model: ShareModel

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Title", text: $model.title)
                    TextField("Message", text: $model.body, axis: .vertical)
                        .lineLimit(3...10)
                    Picker("Priority", selection: $model.prio) {
                        Text("Low").tag(NotifyPriority.low)
                        Text("Normal").tag(NotifyPriority.normal)
                        Text("High").tag(NotifyPriority.high)
                    }
                } footer: {
                    Text("Sharing can't reach the pager directly: the message waits in Pager Companion and is sent "
                         + "the next time the app is open and connected to the pager.")
                }
                if let problem = model.problem {
                    Section { Text(problem).foregroundStyle(.red) }
                }
            }
            .navigationTitle("Send to Pager")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { model.cancel() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Queue") { model.send() }
                        .disabled(!model.loaded || model.title.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }
}
