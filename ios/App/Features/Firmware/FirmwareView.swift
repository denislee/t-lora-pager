import PagerKit
import SwiftUI

/// Firmware update over Bluetooth (S12, PROTOCOL.md §6.7): pick a `.bin`
/// from a GitHub repo's releases, download it (checked against GitHub's
/// SHA-256 `digest` when the asset has one), see the pager's slots, and
/// install. The pager reboots into the new image; it rolls back by itself
/// after three boots that don't confirm. State lives in `FirmwareModel`.
struct FirmwareView: View {
    @EnvironmentObject private var firmware: FirmwareModel
    @EnvironmentObject private var session: PagerSession

    @State private var confirmInstall = false

    var body: some View {
        List {
            statusSection
            Section {
                TextField("owner/repo", text: $firmware.repo)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("List releases") { Task { await firmware.loadReleases() } }
                    .disabled(FirmwareReleases.parseRepo(firmware.repo) == nil || firmware.loading)
            } header: {
                Text("GitHub releases")
            } footer: {
                Text("Public repositories only; no token is used.")
            }
            if !firmware.assets.isEmpty {
                Section("Images") {
                    ForEach(firmware.assets) { asset in
                        Button {
                            Task { await firmware.download(asset) }
                        } label: {
                            AssetRow(asset: asset, downloading: firmware.downloading == asset,
                                     selected: firmware.image?.asset == asset)
                        }
                        .foregroundStyle(.primary)
                        .disabled(firmware.downloading != nil || firmware.installing)
                    }
                }
            }
            installSection
            if let error = firmware.errorText {
                Section { Text(error).foregroundStyle(.red) }
            }
        }
        .navigationTitle("Firmware")
        .refreshable { await firmware.refreshStatus() }
        .task { await firmware.refreshStatus() }
        .confirmationDialog("Install \(firmware.image?.asset.name ?? "")?", isPresented: $confirmInstall,
                            titleVisibility: .visible) {
            Button("Install and restart the pager", role: .destructive) { Task { await firmware.install() } }
        } message: {
            Text("The pager restarts into the new firmware. If it doesn't run for a minute, it goes back to "
                 + "the current firmware after 3 boots. Keep it charged and close to the phone.")
        }
    }

    @ViewBuilder
    private var statusSection: some View {
        Section("Pager") {
            if !session.has("ota") {
                Text(session.pagerCaps.isEmpty ? "Connect to the pager to update it."
                                               : "This pager's firmware can't be updated over Bluetooth yet.")
                    .foregroundStyle(.secondary)
            }
            if let status = firmware.status {
                LabeledContent("Firmware", value: session.lastHello?["fw"]?.stringValue ?? "—")
                LabeledContent("Running slot", value: status.running)
                LabeledContent("Next slot", value: status.next)
                LabeledContent("Slot size", value: ByteCountFormatter.string(fromByteCount: Int64(status.maxSize),
                                                                              countStyle: .file))
                if status.pending {
                    Label("Running image not confirmed yet (boot \(status.boots) of 3)", systemImage: "hourglass")
                        .foregroundStyle(.orange)
                }
            }
        }
    }

    @ViewBuilder
    private var installSection: some View {
        if let image = firmware.image {
            Section {
                LabeledContent("Image", value: image.asset.name)
                LabeledContent("Size", value: ByteCountFormatter.string(fromByteCount: Int64(image.bytes.count),
                                                                         countStyle: .file))
                Label(image.verified ? "SHA-256 matches GitHub's digest" : "GitHub has no digest for this asset",
                      systemImage: image.verified ? "checkmark.seal" : "exclamationmark.triangle")
                    .foregroundColor(image.verified ? .green : .orange)
                if let progress = firmware.progress {
                    VStack(alignment: .leading) {
                        Text(stageText(progress.stage))
                        ProgressView(value: progress.fraction)
                    }
                } else if firmware.installing {
                    ProgressView()
                }
                Button("Install") { confirmInstall = true }
                    .disabled(!firmware.canInstall)
                if let result = firmware.result {
                    Text(result).font(.footnote)
                }
            } header: {
                Text("Install")
            } footer: {
                Text("The image travels as a file upload (/ota.bin) and is checked again on the pager before it is written.")
            }
        }
    }

    private func stageText(_ stage: OTAStage) -> String {
        switch stage {
        case .upload: return "Sending to the pager…"
        case .verify: return "Pager is checking the image…"
        case .write: return "Pager is writing the update slot…"
        }
    }
}

private struct AssetRow: View {
    let asset: FirmwareAsset
    let downloading: Bool
    let selected: Bool

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(asset.name)
                Text("\(asset.tag)\(asset.prerelease ? " (pre-release)" : "") · "
                     + ByteCountFormatter.string(fromByteCount: Int64(asset.size), countStyle: .file))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if downloading {
                ProgressView()
            } else if selected {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            } else {
                Image(systemName: "arrow.down.circle")
            }
        }
    }
}
