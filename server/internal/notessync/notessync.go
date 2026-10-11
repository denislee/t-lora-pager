// Package notessync mounts /api/notes/sync — the device-delegated GitHub push
// path. The device sends its local note list (name + base64 content) along
// with the GitHub repo/branch/PAT, and we do the listing + committing against
// api.github.com on the device's behalf: new files land as blob objects,
// batched into a single tree + commit + ref update via the Git Data API,
// rather than one Contents-API PUT (and one commit) per file. Net win is
// wall-clock: a real CPU with HTTP keepalive and parallel blob uploads
// finishes in seconds where the ESP32 takes tens of seconds to a minute, and
// the device only has to make one round-trip to the LAN hub.
//
// Semantics mirror the device-side code in src/apps/ui_notes_sync.cpp exactly:
// additive-only — we never overwrite a name that already exists on the
// remote, and we never delete. So a sync request is safe to retry: at worst
// it's a no-op because the file landed last time.
//
// Encryption: notes are pushed as opaque bytes. If the device encrypts at
// rest (notes_crypto.cpp's "Salted__"-prefixed AES-256-CBC), the hub neither
// sees nor needs the passphrase.
package notessync

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/lilygo/lilyhub/internal/httpx"
)

// maxRequestBytes caps a device request body (notes travel base64 inline).
const maxRequestBytes = 8 << 20

// SyncRequest is the body of POST /api/notes/sync. The token is a GitHub PAT
// with `contents:write` scope; we never log it or persist it. Files carry the
// same opaque bytes the device would have PUT directly — we don't decode or
// re-encode the base64 payload so encrypted notes pass through verbatim.
type SyncRequest struct {
	Repo   string     `json:"repo"`
	Branch string     `json:"branch"`
	Token  string     `json:"token"`
	Files  []SyncFile `json:"files"`
}

type SyncFile struct {
	Name       string `json:"name"`
	ContentB64 string `json:"content_b64"`
}

// SyncResponse mirrors what the device's run_sync() prints to its log: how
// many were already on the remote, how many we uploaded, how many failed.
// The device renders this back to the user; per-file errors are listed so
// they can be retried individually.
type SyncResponse struct {
	Uploaded int         `json:"uploaded"`
	Already  int         `json:"already"`
	Errors   []SyncError `json:"errors,omitempty"`
}

type SyncError struct {
	Name string `json:"name"`
	Err  string `json:"error"`
}

type Handler struct {
	client *http.Client
	// Bound on parallel blob creates (POST /git/blobs). Unlike the old
	// per-file Contents API PUTs, blob creation is content-addressed and
	// stateless — no parent-ref race — so this can be genuinely parallel
	// instead of hardwired to 1.
	maxParallel int
	// Local on-disk note store. Lets the device offload bulk note storage to
	// the hub when its internal flash fills up. Files are saved as opaque
	// bytes (whatever the device sent — encrypted Salted__ blobs pass through
	// untouched). Path defaults to $LILYHUB_NOTES_DIR or, failing that,
	// $HOME/.lilyhub/notes.
	notesDir string
	// Serializes upload writes — the device may fan out multiple uploads in
	// parallel, and the same name landing twice would race on the shared
	// "<name>.tmp" file (one request's write vs. the other's rename). No
	// fsync is done; the temp+rename only keeps readers from seeing a
	// half-written file. Cheap given typical traffic.
	storeMu sync.Mutex
}

func New() *Handler {
	dir := os.Getenv("LILYHUB_NOTES_DIR")
	if dir == "" {
		if home, err := os.UserHomeDir(); err == nil && home != "" {
			dir = filepath.Join(home, ".lilyhub", "notes")
		} else {
			dir = filepath.Join(os.TempDir(), "lilyhub-notes")
		}
	}
	return &Handler{
		client: &http.Client{
			Timeout: 30 * time.Second,
		},
		maxParallel: 4,
		notesDir:    dir,
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("POST /api/notes/sync", h.sync)
	mux.HandleFunc("POST /api/notes/upload", h.upload)
}

func (h *Handler) sync(w http.ResponseWriter, r *http.Request) {
	var req SyncRequest
	if httpx.DecodeJSON(w, r, maxRequestBytes, &req) != nil {
		return
	}
	if !validRepo(req.Repo) {
		http.Error(w, "repo must be owner/name", http.StatusBadRequest)
		return
	}
	if req.Branch == "" {
		req.Branch = "main"
	}
	if req.Token == "" {
		http.Error(w, "token required", http.StatusBadRequest)
		return
	}

	resp, err := h.runSync(r.Context(), &req)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	httpx.WriteJSON(w, resp)
}

func (h *Handler) runSync(ctx context.Context, req *SyncRequest) (*SyncResponse, error) {
	remote, err := h.listRemote(ctx, req)
	if err != nil {
		return nil, fmt.Errorf("list: %w", err)
	}

	// Partition files into "already on remote" (skip) and "to upload". Done
	// here so the parallel section only does work that has to hit GitHub.
	have := make(map[string]bool, len(remote))
	for _, n := range remote {
		have[n] = true
	}

	out := &SyncResponse{}
	var toUpload []SyncFile
	already := 0
	for _, f := range req.Files {
		if f.Name == "" {
			continue
		}
		// Reject traversal/odd names before they reach the GitHub URL path.
		// safeName mirrors the /upload guard (no slash, NUL, or leading dot).
		// Without it a name like "../secrets.txt" writes outside notes/ and
		// bypasses the additive-only guard, which only enumerates notes/.
		if !safeName(f.Name) {
			out.Errors = append(out.Errors, SyncError{Name: f.Name, Err: "invalid name"})
			continue
		}
		if have[f.Name] {
			already++
			continue
		}
		toUpload = append(toUpload, f)
	}
	out.Already = already

	if len(toUpload) == 0 {
		return out, nil
	}

	h.commitFiles(ctx, req, toUpload, out)
	return out, nil
}

// commitFiles uploads every file in toUpload as a single Git Data API commit
// (blobs -> tree -> commit -> ref) instead of the old one-Contents-API-PUT-
// per-file, one-commit-per-file approach. Blob creation is content-addressed
// and race-free, so it runs with real parallelism (h.maxParallel) — the only
// serialization point left is the final ref update, which is retried against
// a freshly-read branch head on a non-fast-forward conflict (another sync
// landing in between), the same race the old code sidestepped by forcing
// every PUT serial, now isolated to one narrow window instead of gating
// every file. Per-file failures (bad blob upload) are reported in
// out.Errors and excluded from the commit; if the tree/commit/ref-update
// step itself fails after exhausting retries, every file that got a blob is
// reported as failed too, since nothing landed — same "safe to retry, at
// worst a no-op" semantics as before.
func (h *Handler) commitFiles(ctx context.Context, req *SyncRequest, toUpload []SyncFile, out *SyncResponse) {
	type blobResult struct {
		file SyncFile
		sha  string
	}
	blobs := make([]blobResult, 0, len(toUpload))
	var mu sync.Mutex
	sem := make(chan struct{}, h.maxParallel)
	var wg sync.WaitGroup

	for _, f := range toUpload {
		wg.Add(1)
		sem <- struct{}{}
		go func(f SyncFile) {
			defer wg.Done()
			defer func() { <-sem }()

			// Forward the device's base64 verbatim per the package contract —
			// no decode/re-encode — so encrypted notes reach GitHub byte-for-
			// byte and we skip two full-size allocations per file. GitHub
			// validates the base64 itself, so a malformed payload surfaces as
			// the blob-create error below rather than a local decode failure.
			// Blob creation has no parent-ref dependency — content-addressed,
			// so concurrent creates never race.
			sha, err := h.postForSHA(ctx, req, "blobs", "blob", struct {
				Content  string `json:"content"`
				Encoding string `json:"encoding"`
			}{Content: f.ContentB64, Encoding: "base64"})
			mu.Lock()
			defer mu.Unlock()
			if err != nil {
				out.Errors = append(out.Errors, SyncError{Name: f.Name, Err: err.Error()})
				return
			}
			blobs = append(blobs, blobResult{file: f, sha: sha})
		}(f)
	}
	wg.Wait()

	if len(blobs) == 0 {
		return
	}

	names := make([]string, len(blobs))
	for i, b := range blobs {
		names[i] = b.file.Name
	}
	message := commitMessage(names)

	const maxRefAttempts = 3
	var lastErr error
	for attempt := 1; attempt <= maxRefAttempts; attempt++ {
		baseCommit, baseTree, err := h.branchHead(ctx, req)
		if err != nil {
			lastErr = err
			break
		}

		entries := make([]treeEntry, len(blobs))
		for i, b := range blobs {
			entries[i] = treeEntry{Path: "notes/" + b.file.Name, Mode: "100644", Type: "blob", SHA: b.sha}
		}
		// base_tree means we only describe the changed paths (the new note
		// files); GitHub fills in everything else from the base.
		newTree, err := h.postForSHA(ctx, req, "trees", "tree", struct {
			BaseTree string      `json:"base_tree"`
			Tree     []treeEntry `json:"tree"`
		}{BaseTree: baseTree, Tree: entries})
		if err != nil {
			lastErr = err
			break
		}
		newCommit, err := h.postForSHA(ctx, req, "commits", "commit", struct {
			Message string   `json:"message"`
			Tree    string   `json:"tree"`
			Parents []string `json:"parents"`
		}{Message: message, Tree: newTree, Parents: []string{baseCommit}})
		if err != nil {
			lastErr = err
			break
		}
		conflict, err := h.updateRef(ctx, req, newCommit)
		if err == nil {
			out.Uploaded += len(blobs)
			return
		}
		lastErr = err
		if !conflict {
			break
		}
		// Branch moved under us since branchHead was read — refetch and
		// retry the tree/commit/ref-update against the new head, reusing
		// the same already-created blobs.
	}

	for _, b := range blobs {
		out.Errors = append(out.Errors, SyncError{Name: b.file.Name, Err: lastErr.Error()})
	}
}

// commitMessage summarizes the batch for the commit subject/body. A single
// file keeps the old "sync: add <name>" shape; a batch gets a count subject
// plus the full name list in the body, since a git log full of "sync: add
// N notes" without detail would be useless for auditing what actually synced.
func commitMessage(names []string) string {
	if len(names) == 1 {
		return "sync: add " + names[0]
	}
	return fmt.Sprintf("sync: add %d notes\n\n%s", len(names), strings.Join(names, "\n"))
}

// listRemote fetches the names currently in `notes/` on the given branch.
// 404 is not an error — it means the folder doesn't exist yet, so every
// local file is a candidate. We only ask for names; SHAs aren't needed for
// additive-only writes.
func (h *Handler) listRemote(ctx context.Context, req *SyncRequest) ([]string, error) {
	// req.Repo is validated (validRepo) to owner/name with URL-safe chars and
	// no "."/".." segments, so it interpolates directly. req.Branch is caller-
	// controlled and unvalidated, so escape it before it lands in the query.
	endpoint := fmt.Sprintf("https://api.github.com/repos/%s/contents/notes?ref=%s",
		req.Repo, url.QueryEscape(req.Branch))
	httpReq, err := h.newGitHubRequest(ctx, http.MethodGet, endpoint, req.Token, nil)
	if err != nil {
		return nil, err
	}

	body, status, err := httpx.DoWithRetry(h.client, httpReq, 2, "notessync") // one retry
	if err != nil {
		return nil, err
	}
	if status == http.StatusNotFound {
		return nil, nil
	}
	if status != http.StatusOK {
		return nil, githubError(status, body)
	}

	var entries []struct {
		Name string `json:"name"`
		Type string `json:"type"`
	}
	if err := json.Unmarshal(body, &entries); err != nil {
		return nil, fmt.Errorf("parse: %w", err)
	}
	out := make([]string, 0, len(entries))
	for _, e := range entries {
		if e.Type != "" && e.Type != "file" {
			continue
		}
		out = append(out, e.Name)
	}
	return out, nil
}

// newGitHubRequest builds a GitHub API request with the headers every call
// in this file needs. body is nil for GETs; a non-nil body sets
// Content-Type: application/json (every write endpoint here sends JSON).
func (h *Handler) newGitHubRequest(ctx context.Context, method, endpoint, token string, body []byte) (*http.Request, error) {
	var r io.Reader
	if body != nil {
		r = bytes.NewReader(body)
	}
	httpReq, err := http.NewRequestWithContext(ctx, method, endpoint, r)
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Authorization", "Bearer "+token)
	httpReq.Header.Set("Accept", "application/vnd.github+json")
	httpReq.Header.Set("User-Agent", "lilyhub/1.0")
	if body != nil {
		httpReq.Header.Set("Content-Type", "application/json")
	}
	return httpReq, nil
}

// refPathEscape escapes a git ref/branch name for use as a URL path,
// preserving literal "/" — GitHub's ref endpoints expect a branch like
// "feature/foo" to appear as two hierarchical path segments, not one
// percent-encoded blob (mirrors go-github's refURLEscape).
func refPathEscape(ref string) string {
	parts := strings.Split(ref, "/")
	for i, p := range parts {
		parts[i] = url.PathEscape(p)
	}
	return strings.Join(parts, "/")
}

// treeEntry is one file in a Git Data API tree-create request.
type treeEntry struct {
	Path string `json:"path"`
	Mode string `json:"mode"`
	Type string `json:"type"`
	SHA  string `json:"sha"`
}

// branchHead returns the current commit SHA and that commit's tree SHA for
// req.Branch — the base every blob/tree/commit in a sync attempt builds on
// top of. Uses the Branches API (one round trip) rather than
// GET .../git/refs + GET .../git/commits (two).
func (h *Handler) branchHead(ctx context.Context, req *SyncRequest) (commitSHA, treeSHA string, err error) {
	endpoint := fmt.Sprintf("https://api.github.com/repos/%s/branches/%s",
		req.Repo, refPathEscape(req.Branch))
	httpReq, err := h.newGitHubRequest(ctx, http.MethodGet, endpoint, req.Token, nil)
	if err != nil {
		return "", "", err
	}
	body, status, err := httpx.DoWithRetry(h.client, httpReq, 3, "notessync")
	if err != nil {
		return "", "", err
	}
	if status != http.StatusOK {
		return "", "", githubError(status, body)
	}

	var parsed struct {
		Commit struct {
			SHA    string `json:"sha"`
			Commit struct {
				Tree struct {
					SHA string `json:"sha"`
				} `json:"tree"`
			} `json:"commit"`
		} `json:"commit"`
	}
	if err := json.Unmarshal(body, &parsed); err != nil {
		return "", "", fmt.Errorf("parse branch head: %w", err)
	}
	if parsed.Commit.SHA == "" || parsed.Commit.Commit.Tree.SHA == "" {
		return "", "", errors.New("branch head response missing commit/tree sha")
	}
	return parsed.Commit.SHA, parsed.Commit.Commit.Tree.SHA, nil
}

// postForSHA POSTs payload as JSON to the repo's Git Data API endpoint
// /git/<path> (blobs, trees, commits) and returns the "sha" of the object it
// created. what names the object in error messages.
func (h *Handler) postForSHA(ctx context.Context, req *SyncRequest, path, what string, payload any) (string, error) {
	buf, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}
	endpoint := fmt.Sprintf("https://api.github.com/repos/%s/git/%s", req.Repo, path)
	httpReq, err := h.newGitHubRequest(ctx, http.MethodPost, endpoint, req.Token, buf)
	if err != nil {
		return "", err
	}
	respBody, status, err := httpx.DoWithRetry(h.client, httpReq, 3, "notessync")
	if err != nil {
		return "", err
	}
	if status/100 != 2 {
		return "", githubError(status, respBody)
	}
	var parsed struct {
		SHA string `json:"sha"`
	}
	if err := json.Unmarshal(respBody, &parsed); err != nil {
		return "", fmt.Errorf("parse %s response: %w", what, err)
	}
	if parsed.SHA == "" {
		return "", fmt.Errorf("%s response missing sha", what)
	}
	return parsed.SHA, nil
}

// updateRef fast-forwards req.Branch to newCommit. GitHub returns 422 when
// this isn't a fast-forward (the branch moved since branchHead was read) —
// the conflict return tells commitFiles whether to refetch the head and
// retry, versus giving up on a harder failure (auth, network, ...).
func (h *Handler) updateRef(ctx context.Context, req *SyncRequest, newCommit string) (conflict bool, err error) {
	body := struct {
		SHA   string `json:"sha"`
		Force bool   `json:"force"`
	}{SHA: newCommit, Force: false}
	buf, _ := json.Marshal(body)

	endpoint := fmt.Sprintf("https://api.github.com/repos/%s/git/refs/heads/%s",
		req.Repo, refPathEscape(req.Branch))
	httpReq, err := h.newGitHubRequest(ctx, http.MethodPatch, endpoint, req.Token, buf)
	if err != nil {
		return false, err
	}
	respBody, status, err := httpx.DoWithRetry(h.client, httpReq, 3, "notessync")
	if err != nil {
		return false, err
	}
	if status/100 != 2 {
		return status == http.StatusUnprocessableEntity, githubError(status, respBody)
	}
	return false, nil
}

// githubError formats a non-success GitHub reply with a short prefix of its
// body, so the API's own message reaches the device's error list.
func githubError(status int, body []byte) error {
	return fmt.Errorf("github %d: %s", status, httpx.Truncate(string(body), 200))
}

// UploadRequest is the body of POST /api/notes/upload. The hub stores the
// raw decoded bytes under notesDir/<name>. Idempotent: a second upload with
// the same name and same bytes is a no-op; different bytes overwrite. The
// device uses this to mirror its internal-flash note store to the hub when
// flash gets tight. Making this store the source of truth for /sync (instead
// of the device sending its files) is only a proposal (P6.33); /sync does
// not read it today.
type UploadRequest struct {
	Name       string `json:"name"`
	ContentB64 string `json:"content_b64"`
}

// UploadResponse is the 200 reply to /api/notes/upload. The device only
// checks the status code (hub_upload_note); the body is for curl debugging.
// It stays a 200 rather than a 204 because older device firmware may check
// for exactly 200.
type UploadResponse struct {
	Stored bool   `json:"stored"`
	Bytes  int    `json:"bytes"`
	Path   string `json:"path,omitempty"`
}

// validRepo enforces exactly "owner/name" with GitHub-legal characters and
// rejects any "." or ".." segment, so req.Repo is safe to interpolate into the
// api.github.com URL path. Without it, a repo like "../../x" — or a segment
// "." / ".." — would traverse the API path, the sync-side equivalent of the
// filename traversal safeName guards against on /upload.
func validRepo(repo string) bool {
	owner, name, ok := strings.Cut(repo, "/")
	if !ok {
		return false
	}
	return validRepoSegment(owner) && validRepoSegment(name)
}

func validRepoSegment(s string) bool {
	if s == "" || s == "." || s == ".." {
		return false
	}
	for _, c := range s {
		switch {
		case c >= 'A' && c <= 'Z':
		case c >= 'a' && c <= 'z':
		case c >= '0' && c <= '9':
		case c == '.' || c == '-' || c == '_':
		default:
			return false
		}
	}
	return true
}

// safeName guards against directory traversal and oddball names landing in
// the notes dir. Notes filenames are app-generated (YYYYMMDD_HHMMSS.txt or
// similar), so the rules can be strict — anything empty, over 200 bytes,
// containing a slash, backslash or NUL, or starting with a dot (which also
// covers "." and "..") is rejected.
func safeName(name string) bool {
	return name != "" && len(name) <= 200 &&
		!strings.HasPrefix(name, ".") &&
		!strings.ContainsAny(name, "/\\\x00")
}

func (h *Handler) upload(w http.ResponseWriter, r *http.Request) {
	var req UploadRequest
	if httpx.DecodeJSON(w, r, maxRequestBytes, &req) != nil {
		return
	}
	name := req.Name
	if !safeName(name) {
		http.Error(w, "invalid name", http.StatusBadRequest)
		return
	}
	bs, err := base64.StdEncoding.DecodeString(req.ContentB64)
	if err != nil {
		http.Error(w, "bad base64: "+err.Error(), http.StatusBadRequest)
		return
	}
	req.ContentB64 = "" // let GC drop the ~1.33x base64 string before the file write below

	h.storeMu.Lock()
	defer h.storeMu.Unlock()

	if err := os.MkdirAll(h.notesDir, 0o755); err != nil {
		http.Error(w, "mkdir: "+err.Error(), http.StatusInternalServerError)
		return
	}
	dst := filepath.Join(h.notesDir, name)
	tmp := dst + ".tmp"
	if err := os.WriteFile(tmp, bs, 0o644); err != nil {
		http.Error(w, "write: "+err.Error(), http.StatusInternalServerError)
		return
	}
	if err := os.Rename(tmp, dst); err != nil {
		_ = os.Remove(tmp)
		http.Error(w, "rename: "+err.Error(), http.StatusInternalServerError)
		return
	}

	httpx.WriteJSON(w, UploadResponse{Stored: true, Bytes: len(bs), Path: name})
}
