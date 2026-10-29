import PagerKit
import PhotosUI
import QuickLook
import SwiftUI
import UniformTypeIdentifiers

/// The pager's files (`IOS_COMPANION_PLAN.md` §6.1 "Files"): Internal / SD,
/// breadcrumbs, folders first, pull to refresh, swipe to delete or rename,
/// New folder, tap a file to download it (progress, then a text preview or
/// Quick Look, with Share), and upload from the Files app or Photos. Files
/// the pager marks `protected` show a lock and are never decrypted here.
/// State lives in `FilesModel`; this view only sequences it.
struct FilesView: View {
    @EnvironmentObject private var files: FilesModel

    @State private var showNewFolder = false
    @State private var newFolderName = ""
    @State private var renaming: FilesModel.Row?
    @State private var renameText = ""
    @State private var pendingDelete: FilesModel.Row?
    @State private var showImporter = false
    @State private var photo: PhotosPickerItem?

    /// Largest file the app uploads in one go (it is held in memory).
    private static let maxUploadBytes = 64 * 1024 * 1024

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Picker("Volume", selection: $files.volume) {
                        ForEach(FilesModel.Volume.allCases) { volume in
                            Text(volume.title).tag(volume)
                        }
                    }
                    .pickerStyle(.segmented)
                    breadcrumbs
                }
                statusSection
                entriesSection
            }
            .navigationTitle("Files")
            .refreshable { await files.reload(force: true) }
            .toolbar { toolbar }
            .alert("New folder", isPresented: $showNewFolder) {
                TextField("Name", text: $newFolderName)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Create") {
                    let name = newFolderName.trimmingCharacters(in: .whitespaces)
                    Task { await files.makeFolder(named: name) }
                }
                .disabled(!FilePath.isValidName(newFolderName.trimmingCharacters(in: .whitespaces)))
                Button("Cancel", role: .cancel) {}
            }
            .alert("Rename", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
                TextField("Name", text: $renameText)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Rename") {
                    guard let row = renaming else { return }
                    let name = renameText.trimmingCharacters(in: .whitespaces)
                    Task { await files.rename(row, to: name) }
                }
                .disabled(!FilePath.isValidName(renameText.trimmingCharacters(in: .whitespaces)))
                Button("Cancel", role: .cancel) {}
            }
            .confirmationDialog(
                "Delete \(pendingDelete?.entry.name ?? "this item") from the pager?",
                isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }),
                titleVisibility: .visible
            ) {
                Button("Delete", role: .destructive) {
                    guard let row = pendingDelete else { return }
                    Task { await files.delete(row) }
                }
            } message: {
                Text(pendingDelete?.entry.dir == true
                     ? "The folder and everything in it are removed. This cannot be undone."
                     : "This cannot be undone.")
            }
            .fileImporter(isPresented: $showImporter, allowedContentTypes: [.item], allowsMultipleSelection: false) { result in
                guard case .success(let urls) = result, let url = urls.first else { return }
                importFile(url)
            }
            .onChange(of: photo) { item in
                guard let item else { return }
                photo = nil
                importPhoto(item)
            }
            .sheet(item: $files.downloaded) { file in
                FilePreviewView(file: file)
            }
        }
    }

    // MARK: - Sections

    private var breadcrumbs: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 4) {
                ForEach(Array(files.crumbs.enumerated()), id: \.element.path) { index, crumb in
                    if index > 0 {
                        Image(systemName: "chevron.right")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                    Button(crumb.name) { files.open(crumb.path) }
                        .buttonStyle(.borderless)
                        .fontWeight(crumb.path == files.path ? .semibold : .regular)
                        .disabled(crumb.path == files.path)
                }
            }
        }
    }

    @ViewBuilder
    private var statusSection: some View {
        if !files.connected {
            Section {
                Text("Connect to the pager to browse its files.")
                    .foregroundStyle(.secondary)
            }
        }
        if let transfer = files.transfer {
            Section {
                VStack(alignment: .leading, spacing: 6) {
                    Text("\(transfer.direction == .download ? "Downloading" : "Uploading") \(transfer.name)")
                        .font(.subheadline)
                    ProgressView(value: transfer.fraction)
                    Text("\(bytes(Int(transfer.done))) of \(bytes(Int(transfer.total)))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        if files.interruptedCount > 0 {
            Section {
                Label("\(files.interruptedCount) upload(s) wait for the pager to reconnect; they start again from the beginning.",
                      systemImage: "arrow.clockwise.circle")
                    .font(.footnote)
                Button("Cancel waiting uploads", role: .destructive) {
                    Task { await files.discardInterrupted() }
                }
            }
        }
        if let error = files.errorText {
            Section {
                Text(error)
                    .foregroundStyle(.red)
                    .font(.footnote)
            }
        }
    }

    @ViewBuilder
    private var entriesSection: some View {
        Section {
            if files.loading && files.rows.isEmpty {
                ProgressView()
            } else if files.rows.isEmpty && files.connected && files.errorText == nil {
                Text("Empty folder")
                    .foregroundStyle(.secondary)
            }
            ForEach(files.rows) { row in
                Button {
                    tapped(row)
                } label: {
                    FileRow(row: row)
                }
                .foregroundStyle(.primary)
                .swipeActions(edge: .trailing) {
                    Button(role: .destructive) {
                        pendingDelete = row
                    } label: {
                        Label("Delete", systemImage: "trash")
                    }
                    Button {
                        renameText = row.entry.name
                        renaming = row
                    } label: {
                        Label("Rename", systemImage: "pencil")
                    }
                    .tint(.orange)
                }
                .disabled(!files.canEdit)
            }
        }
    }

    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .navigationBarTrailing) {
            Button {
                newFolderName = ""
                showNewFolder = true
            } label: {
                Label("New folder", systemImage: "folder.badge.plus")
            }
            .disabled(!files.canEdit)
            PhotosPicker(selection: $photo, matching: .any(of: [.images, .videos])) {
                Label("Upload from Photos", systemImage: "photo.badge.plus")
            }
            .disabled(!files.canEdit)
            Button {
                showImporter = true
            } label: {
                Label("Upload from Files", systemImage: "square.and.arrow.up")
            }
            .disabled(!files.canEdit)
        }
    }

    // MARK: - Actions

    private func tapped(_ row: FilesModel.Row) {
        if row.entry.dir {
            files.open(FilePath.join(files.path, row.entry.name))
        } else {
            Task { await files.download(row) }
        }
    }

    /// Read a file the Files app picked (security-scoped) and upload it.
    private func importFile(_ url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url) else {
            files.errorText = "Could not read \(url.lastPathComponent)."
            return
        }
        guard data.count <= Self.maxUploadBytes else {
            files.errorText = "\(url.lastPathComponent) is too large to send over the link."
            return
        }
        let name = url.lastPathComponent
        Task { await files.upload(Array(data), named: name) }
    }

    /// Load a picked photo or video as data and upload it, named after the
    /// time it was picked and its type's usual extension.
    private func importPhoto(_ item: PhotosPickerItem) {
        let ext = item.supportedContentTypes.first?.preferredFilenameExtension ?? "jpg"
        Task {
            guard let data = try? await item.loadTransferable(type: Data.self) else {
                files.errorText = "Could not load the photo."
                return
            }
            guard data.count <= Self.maxUploadBytes else {
                files.errorText = "That item is too large to send over the link."
                return
            }
            let stamp = Journal.noteName(for: Date()).dropLast(4)
            await files.upload(Array(data), named: "IMG_\(stamp).\(ext)")
        }
    }

    private func bytes(_ value: Int) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(value), countStyle: .file)
    }
}

/// One listing row: icon, name, size and date, and the lock badge.
private struct FileRow: View {
    let row: FilesModel.Row

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: row.entry.dir ? "folder.fill" : icon)
                .foregroundColor(row.entry.dir ? .accentColor : .secondary)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(row.entry.name)
                    .lineLimit(1)
                if !row.entry.dir {
                    Text(detail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            if row.protected {
                Image(systemName: "lock.fill")
                    .foregroundStyle(.secondary)
                    .accessibilityLabel("Encrypted")
            }
            if row.entry.dir {
                Image(systemName: "chevron.right")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
        }
    }

    private var icon: String {
        switch FilePreviewKind.fileExtension(row.entry.name) {
        case "txt", "md", "json", "csv", "log": return "doc.text"
        case "wav", "mp3", "m4a": return "waveform"
        case "jpg", "jpeg", "png", "heic", "gif", "bmp": return "photo"
        default: return "doc"
        }
    }

    private var detail: String {
        let size = ByteCountFormatter.string(fromByteCount: Int64(row.entry.size), countStyle: .file)
        // Hardware listings report mtime 0.
        guard row.entry.mtime > 0 else { return size }
        let date = Date(timeIntervalSince1970: TimeInterval(row.entry.mtime))
        return "\(size) · \(date.formatted(date: .abbreviated, time: .shortened))"
    }
}

/// A downloaded file: text shown in place, everything else through Quick
/// Look, and a Share button either way. Ciphertext is only offered to Share.
struct FilePreviewView: View {
    let file: FilesModel.Downloaded

    @Environment(\.dismiss) private var dismiss
    @State private var text: String?
    @State private var quickLookURL: URL?

    var body: some View {
        NavigationStack {
            Group {
                switch file.kind {
                case .text:
                    ScrollView {
                        Text(text ?? "")
                            .font(.system(.footnote, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding()
                    }
                case .quickLook:
                    VStack(spacing: 16) {
                        Image(systemName: "doc.viewfinder")
                            .font(.system(size: 48))
                            .foregroundStyle(.secondary)
                        Button("Open in Quick Look") { quickLookURL = file.url }
                            .buttonStyle(.borderedProminent)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                case .encrypted:
                    VStack(spacing: 12) {
                        Image(systemName: "lock.doc")
                            .font(.system(size: 48))
                            .foregroundStyle(.secondary)
                        Text("Encrypted on the pager. Open it in Notes to read it; here it can only be shared as ciphertext.")
                            .multilineTextAlignment(.center)
                            .foregroundStyle(.secondary)
                    }
                    .padding()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .navigationTitle(file.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
                ToolbarItem(placement: .primaryAction) {
                    ShareLink(item: file.url)
                }
            }
            .quickLookPreview($quickLookURL)
            .task {
                guard file.kind == .text, text == nil else { return }
                text = (try? Data(contentsOf: file.url)).map { String(decoding: $0, as: UTF8.self) } ?? ""
            }
        }
    }
}
