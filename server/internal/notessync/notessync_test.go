package notessync

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// validRepo is the guard that keeps req.Repo from traversing the api.github.com
// URL path (e.g. "../../other"), so cover the traversal and malformed cases.
func TestValidRepo(t *testing.T) {
	valid := []string{
		"octocat/Hello-World",
		"my.org/my.repo",
		"a_b/c-d",
		"user123/repo.git",
	}
	for _, r := range valid {
		if !validRepo(r) {
			t.Errorf("validRepo(%q) = false, want true", r)
		}
	}

	invalid := []string{
		"",
		"noslash",
		"owner/",
		"/repo",
		"owner/re/po",    // extra segment
		"../../x",        // traversal
		"a/..",           // dot-dot segment
		"../b",           // dot-dot segment
		".",              // no slash + dot
		"owner/repo\x00", // NUL
		"own er/repo",    // space
		"owner/re?po",    // query char
	}
	for _, r := range invalid {
		if validRepo(r) {
			t.Errorf("validRepo(%q) = true, want false", r)
		}
	}
}

// safeName is applied to every file name in the sync path so a name like
// "../secrets.txt" can't write outside notes/.
func TestSafeNameRejectsTraversal(t *testing.T) {
	ok := []string{"20250101_120000.txt", "note.md", "a-b_c.txt"}
	for _, n := range ok {
		if !safeName(n) {
			t.Errorf("safeName(%q) rejected, want accepted", n)
		}
	}

	bad := []string{
		"",
		"../secrets.txt",
		"a/b",
		"..",
		".",
		".hidden",
		"x\x00y",
		"a\\b",
		strings.Repeat("a", 201),
	}
	for _, n := range bad {
		if safeName(n) {
			t.Errorf("safeName(%q) accepted, want rejected", n)
		}
	}
}

// refPathEscape must preserve literal "/" in branch names like
// "feature/foo" (GitHub's ref endpoints treat it as two path segments, not
// one percent-encoded blob) while still escaping special characters within
// each segment.
func TestRefPathEscape(t *testing.T) {
	cases := []struct{ in, want string }{
		{"main", "main"},
		{"feature/foo", "feature/foo"},
		{"weird branch", "weird%20branch"},
		{"feat/weird branch", "feat/weird%20branch"},
	}
	for _, c := range cases {
		if got := refPathEscape(c.in); got != c.want {
			t.Errorf("refPathEscape(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestCommitMessage(t *testing.T) {
	if got := commitMessage([]string{"a.txt"}); got != "sync: add a.txt" {
		t.Errorf("commitMessage single = %q, want %q", got, "sync: add a.txt")
	}
	got := commitMessage([]string{"a.txt", "b.txt"})
	if !strings.HasPrefix(got, "sync: add 2 notes\n\n") {
		t.Errorf("commitMessage multi = %q, want the count-prefixed shape", got)
	}
	if !strings.Contains(got, "a.txt") || !strings.Contains(got, "b.txt") {
		t.Errorf("commitMessage multi = %q, missing a file name", got)
	}
}

// mockGitHubState is a tiny in-memory stand-in for the handful of GitHub Git
// Data API endpoints commitFiles drives, so runSync can be exercised
// end-to-end through its real endpoint-construction/retry code path
// (notessync's URLs are hardcoded to api.github.com — see githubTestClient —
// unlike chat.go's injectable baseURL).
type mockGitHubState struct {
	mu sync.Mutex

	notesFiles       map[string]bool // listRemote result
	headSHA          string
	treeSHA          string
	blobFailContent  map[string]bool // content_b64 -> fail this blob create
	refConflictsLeft int             // PATCH /git/refs returns 422 this many times before succeeding

	blobSeq, treeSeq, commitSeq                     int
	branchHeadCalls, blobCalls, treeCalls, refCalls int
	commitCalls                                     int
	lastTreeEntries                                 []treeEntry
	lastCommitMsg                                   string
}

func newMockGitHubState() *mockGitHubState {
	return &mockGitHubState{
		notesFiles:      map[string]bool{},
		headSHA:         "commit-0",
		treeSHA:         "tree-0",
		blobFailContent: map[string]bool{},
	}
}

func newMockGitHubServer(t *testing.T, st *mockGitHubState) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		st.mu.Lock()
		defer st.mu.Unlock()

		switch {
		case r.Method == http.MethodGet && strings.HasSuffix(r.URL.Path, "/contents/notes"):
			type entry struct {
				Name string `json:"name"`
				Type string `json:"type"`
			}
			entries := make([]entry, 0, len(st.notesFiles))
			for n := range st.notesFiles {
				entries = append(entries, entry{Name: n, Type: "file"})
			}
			if len(entries) == 0 {
				w.WriteHeader(http.StatusNotFound)
				return
			}
			w.WriteHeader(http.StatusOK)
			_ = json.NewEncoder(w).Encode(entries)

		case r.Method == http.MethodGet && strings.Contains(r.URL.Path, "/branches/"):
			st.branchHeadCalls++
			var resp struct {
				Commit struct {
					SHA    string `json:"sha"`
					Commit struct {
						Tree struct {
							SHA string `json:"sha"`
						} `json:"tree"`
					} `json:"commit"`
				} `json:"commit"`
			}
			resp.Commit.SHA = st.headSHA
			resp.Commit.Commit.Tree.SHA = st.treeSHA
			w.WriteHeader(http.StatusOK)
			_ = json.NewEncoder(w).Encode(resp)

		case r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/git/blobs"):
			st.blobCalls++
			var body struct {
				Content  string `json:"content"`
				Encoding string `json:"encoding"`
			}
			b, _ := io.ReadAll(r.Body)
			_ = json.Unmarshal(b, &body)
			if st.blobFailContent[body.Content] {
				w.WriteHeader(http.StatusUnprocessableEntity)
				_, _ = w.Write([]byte(`{"message":"blob create failed"}`))
				return
			}
			st.blobSeq++
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(struct {
				SHA string `json:"sha"`
			}{SHA: fmt.Sprintf("blob-%d", st.blobSeq)})

		case r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/git/trees"):
			st.treeCalls++
			var body struct {
				BaseTree string      `json:"base_tree"`
				Tree     []treeEntry `json:"tree"`
			}
			b, _ := io.ReadAll(r.Body)
			_ = json.Unmarshal(b, &body)
			st.lastTreeEntries = body.Tree
			st.treeSeq++
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(struct {
				SHA string `json:"sha"`
			}{SHA: fmt.Sprintf("tree-%d", st.treeSeq)})

		case r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/git/commits"):
			st.commitCalls++
			var body struct {
				Message string   `json:"message"`
				Tree    string   `json:"tree"`
				Parents []string `json:"parents"`
			}
			b, _ := io.ReadAll(r.Body)
			_ = json.Unmarshal(b, &body)
			st.lastCommitMsg = body.Message
			st.commitSeq++
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(struct {
				SHA string `json:"sha"`
			}{SHA: fmt.Sprintf("commit-%d", st.commitSeq)})

		case r.Method == http.MethodPatch && strings.Contains(r.URL.Path, "/git/refs/heads/"):
			st.refCalls++
			var body struct {
				SHA   string `json:"sha"`
				Force bool   `json:"force"`
			}
			b, _ := io.ReadAll(r.Body)
			_ = json.Unmarshal(b, &body)
			if st.refConflictsLeft > 0 {
				st.refConflictsLeft--
				// Simulate someone else's concurrent commit moving the
				// branch between this attempt's branchHead read and now.
				st.headSHA = "concurrent-" + st.headSHA
				st.treeSHA = "concurrent-" + st.treeSHA
				w.WriteHeader(http.StatusUnprocessableEntity)
				_, _ = w.Write([]byte(`{"message":"Update is not a fast forward"}`))
				return
			}
			st.headSHA = body.SHA
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte(`{}`))

		default:
			w.WriteHeader(http.StatusInternalServerError)
			t.Errorf("mock GitHub server: unexpected request %s %s", r.Method, r.URL.Path)
		}
	}))
}

// rewriteHostTransport redirects any request to target, preserving path and
// query — how these tests point notessync's hardcoded api.github.com URLs at
// an httptest server without adding a baseURL injection point to production
// code just for tests.
type rewriteHostTransport struct {
	target *url.URL
}

func (t rewriteHostTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	req = req.Clone(req.Context())
	req.URL.Scheme = t.target.Scheme
	req.URL.Host = t.target.Host
	req.Host = t.target.Host
	return http.DefaultTransport.RoundTrip(req)
}

func githubTestClient(t *testing.T, srv *httptest.Server) *http.Client {
	t.Helper()
	target, err := url.Parse(srv.URL)
	if err != nil {
		t.Fatalf("parse test server URL: %v", err)
	}
	return &http.Client{Transport: rewriteHostTransport{target: target}, Timeout: 5 * time.Second}
}

// newMockHandler starts a mock GitHub server over st and returns a Handler
// whose client is pointed at it. The server is closed on test cleanup.
func newMockHandler(t *testing.T, st *mockGitHubState) *Handler {
	t.Helper()
	srv := newMockGitHubServer(t, st)
	t.Cleanup(srv.Close)
	return &Handler{client: githubTestClient(t, srv), maxParallel: 4}
}

func testSyncRequest(files ...SyncFile) *SyncRequest {
	return &SyncRequest{Repo: "octocat/vault", Branch: "main", Token: "tok", Files: files}
}

func TestRunSyncCommitsNewFilesInOneCommit(t *testing.T) {
	st := newMockGitHubState()
	st.notesFiles["already.txt"] = true
	h := newMockHandler(t, st)

	req := testSyncRequest(
		SyncFile{Name: "already.txt", ContentB64: "ZXhpc3Rpbmc="},
		SyncFile{Name: "a.txt", ContentB64: "YQ=="},
		SyncFile{Name: "b.txt", ContentB64: "Yg=="},
	)
	resp, err := h.runSync(context.Background(), req)
	if err != nil {
		t.Fatalf("runSync: %v", err)
	}
	if resp.Already != 1 || resp.Uploaded != 2 || len(resp.Errors) != 0 {
		t.Fatalf("resp = %+v, want Already=1 Uploaded=2 no errors", resp)
	}

	st.mu.Lock()
	defer st.mu.Unlock()
	if st.blobCalls != 2 {
		t.Errorf("blobCalls = %d, want 2", st.blobCalls)
	}
	if st.treeCalls != 1 || st.commitCalls != 1 || st.refCalls != 1 {
		t.Errorf("treeCalls=%d commitCalls=%d refCalls=%d, want 1 each (single batched commit)",
			st.treeCalls, st.commitCalls, st.refCalls)
	}
	if len(st.lastTreeEntries) != 2 {
		t.Fatalf("tree entries = %d, want 2", len(st.lastTreeEntries))
	}
	paths := map[string]bool{}
	for _, e := range st.lastTreeEntries {
		paths[e.Path] = true
	}
	if !paths["notes/a.txt"] || !paths["notes/b.txt"] {
		t.Errorf("tree paths = %v, want notes/a.txt and notes/b.txt", paths)
	}
	if !strings.Contains(st.lastCommitMsg, "a.txt") || !strings.Contains(st.lastCommitMsg, "b.txt") {
		t.Errorf("commit message = %q, missing an uploaded file name", st.lastCommitMsg)
	}
}

func TestRunSyncPartialBlobFailureExcludesFileFromCommit(t *testing.T) {
	st := newMockGitHubState()
	st.blobFailContent["YmFk"] = true // "bad" fails to blob
	h := newMockHandler(t, st)

	req := testSyncRequest(
		SyncFile{Name: "good.txt", ContentB64: "Z29vZA=="},
		SyncFile{Name: "bad.txt", ContentB64: "YmFk"},
	)
	resp, err := h.runSync(context.Background(), req)
	if err != nil {
		t.Fatalf("runSync: %v", err)
	}
	if resp.Uploaded != 1 {
		t.Errorf("Uploaded = %d, want 1", resp.Uploaded)
	}
	if len(resp.Errors) != 1 || resp.Errors[0].Name != "bad.txt" {
		t.Fatalf("Errors = %+v, want exactly one error for bad.txt", resp.Errors)
	}

	st.mu.Lock()
	defer st.mu.Unlock()
	if len(st.lastTreeEntries) != 1 || st.lastTreeEntries[0].Path != "notes/good.txt" {
		t.Errorf("tree entries = %+v, want only notes/good.txt (the failed blob must not enter the commit)",
			st.lastTreeEntries)
	}
}

func TestRunSyncAllBlobFailuresSkipCommit(t *testing.T) {
	st := newMockGitHubState()
	st.blobFailContent["YmFk"] = true
	h := newMockHandler(t, st)

	req := testSyncRequest(SyncFile{Name: "bad.txt", ContentB64: "YmFk"})
	resp, err := h.runSync(context.Background(), req)
	if err != nil {
		t.Fatalf("runSync: %v", err)
	}
	if resp.Uploaded != 0 || len(resp.Errors) != 1 {
		t.Fatalf("resp = %+v, want Uploaded=0 and one error", resp)
	}

	st.mu.Lock()
	defer st.mu.Unlock()
	if st.branchHeadCalls != 0 || st.treeCalls != 0 || st.commitCalls != 0 || st.refCalls != 0 {
		t.Errorf("expected zero tree/commit/ref calls when every blob fails, got branchHead=%d tree=%d commit=%d ref=%d",
			st.branchHeadCalls, st.treeCalls, st.commitCalls, st.refCalls)
	}
}

func TestRunSyncNoNewFilesSkipsGitDataAPICalls(t *testing.T) {
	st := newMockGitHubState()
	st.notesFiles["a.txt"] = true
	h := newMockHandler(t, st)

	req := testSyncRequest(SyncFile{Name: "a.txt", ContentB64: "YQ=="})
	resp, err := h.runSync(context.Background(), req)
	if err != nil {
		t.Fatalf("runSync: %v", err)
	}
	if resp.Already != 1 || resp.Uploaded != 0 || len(resp.Errors) != 0 {
		t.Fatalf("resp = %+v, want Already=1 only", resp)
	}

	st.mu.Lock()
	defer st.mu.Unlock()
	if st.branchHeadCalls != 0 || st.blobCalls != 0 || st.treeCalls != 0 || st.commitCalls != 0 || st.refCalls != 0 {
		t.Errorf("expected zero Git Data API calls when nothing is new, got branchHead=%d blob=%d tree=%d commit=%d ref=%d",
			st.branchHeadCalls, st.blobCalls, st.treeCalls, st.commitCalls, st.refCalls)
	}
}

// A 422 on the ref update means another sync landed between branchHead and
// this attempt's PATCH; commitFiles must refetch the head and retry the
// tree/commit/ref-update (reusing the already-created blobs) rather than
// failing the whole batch.
func TestRunSyncRefConflictRetriesAgainstFreshHead(t *testing.T) {
	st := newMockGitHubState()
	st.refConflictsLeft = 1
	h := newMockHandler(t, st)

	req := testSyncRequest(SyncFile{Name: "a.txt", ContentB64: "YQ=="})
	resp, err := h.runSync(context.Background(), req)
	if err != nil {
		t.Fatalf("runSync: %v", err)
	}
	if resp.Uploaded != 1 || len(resp.Errors) != 0 {
		t.Fatalf("resp = %+v, want Uploaded=1 no errors", resp)
	}

	st.mu.Lock()
	defer st.mu.Unlock()
	if st.branchHeadCalls != 2 {
		t.Errorf("branchHeadCalls = %d, want 2 (refetch after the conflict)", st.branchHeadCalls)
	}
	if st.refCalls != 2 {
		t.Errorf("refCalls = %d, want 2 (conflict then success)", st.refCalls)
	}
	if st.blobCalls != 1 {
		t.Errorf("blobCalls = %d, want 1 (blobs aren't recreated on a ref-update retry)", st.blobCalls)
	}
}

func TestRunSyncRefConflictExhaustsRetriesReportsErrors(t *testing.T) {
	st := newMockGitHubState()
	st.refConflictsLeft = 999 // always conflicts
	h := newMockHandler(t, st)

	req := testSyncRequest(SyncFile{Name: "a.txt", ContentB64: "YQ=="})
	resp, err := h.runSync(context.Background(), req)
	if err != nil {
		t.Fatalf("runSync: %v", err)
	}
	if resp.Uploaded != 0 {
		t.Errorf("Uploaded = %d, want 0", resp.Uploaded)
	}
	if len(resp.Errors) != 1 || resp.Errors[0].Name != "a.txt" {
		t.Fatalf("Errors = %+v, want exactly one error for a.txt", resp.Errors)
	}

	st.mu.Lock()
	defer st.mu.Unlock()
	if st.refCalls != 3 {
		t.Errorf("refCalls = %d, want 3 (maxRefAttempts)", st.refCalls)
	}
	if st.blobCalls != 1 {
		t.Errorf("blobCalls = %d, want 1 (not recreated across ref-update retries)", st.blobCalls)
	}
}

// serve routes req through a mux with the handler registered, so the Go 1.22
// method patterns (POST-only) are exercised as in production.
func serve(h *Handler, req *http.Request) *httptest.ResponseRecorder {
	mux := http.NewServeMux()
	h.Register(mux)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	return rec
}

func TestRoutesRejectWrongMethod(t *testing.T) {
	h := &Handler{notesDir: t.TempDir()}
	for _, path := range []string{"/api/notes/sync", "/api/notes/upload"} {
		rec := serve(h, httptest.NewRequest(http.MethodGet, path, nil))
		if rec.Code != http.StatusMethodNotAllowed {
			t.Errorf("GET %s: code=%d, want 405", path, rec.Code)
		}
	}
}

// The upload reply is what the device's HTTPClient reads, so it must carry
// Content-Length and Connection: close (P6.41) and keep the 200 status.
func TestUploadStoresFileAndRepliesWithLength(t *testing.T) {
	dir := t.TempDir()
	h := &Handler{notesDir: filepath.Join(dir, "notes")} // MkdirAll creates it
	body := `{"name":"a.txt","content_b64":"aGVsbG8="}`
	rec := serve(h, httptest.NewRequest(http.MethodPost, "/api/notes/upload", strings.NewReader(body)))
	if rec.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q, want 200", rec.Code, rec.Body.String())
	}
	if got := rec.Header().Get("Content-Length"); got != strconv.Itoa(rec.Body.Len()) {
		t.Errorf("Content-Length=%q, want %d", got, rec.Body.Len())
	}
	if got := rec.Header().Get("Connection"); got != "close" {
		t.Errorf("Connection=%q, want close", got)
	}
	var resp UploadResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("reply not JSON: %v", err)
	}
	if !resp.Stored || resp.Bytes != 5 || resp.Path != "a.txt" {
		t.Errorf("resp=%+v, want stored 5 bytes at a.txt", resp)
	}
	got, err := os.ReadFile(filepath.Join(dir, "notes", "a.txt"))
	if err != nil || string(got) != "hello" {
		t.Fatalf("stored file = %q, %v; want hello", got, err)
	}
}

func TestUploadRejectsBadInput(t *testing.T) {
	h := &Handler{notesDir: t.TempDir()}
	cases := map[string]string{
		"bad json":      `{"name":`,
		"unknown field": `{"name":"a.txt","content_b64":"","extra":1}`,
		"bad name":      `{"name":"../x","content_b64":"aGk="}`,
		"bad base64":    `{"name":"a.txt","content_b64":"!!!"}`,
	}
	for name, body := range cases {
		rec := serve(h, httptest.NewRequest(http.MethodPost, "/api/notes/upload", strings.NewReader(body)))
		if rec.Code != http.StatusBadRequest {
			t.Errorf("%s: code=%d, want 400", name, rec.Code)
		}
	}
}

func TestSyncHandlerValidatesAndRepliesWithLength(t *testing.T) {
	st := newMockGitHubState()
	h := newMockHandler(t, st)

	for name, body := range map[string]string{
		"bad json": `{`,
		"bad repo": `{"repo":"../x","token":"t"}`,
		"no token": `{"repo":"o/r"}`,
	} {
		rec := serve(h, httptest.NewRequest(http.MethodPost, "/api/notes/sync", strings.NewReader(body)))
		if rec.Code != http.StatusBadRequest {
			t.Errorf("%s: code=%d, want 400", name, rec.Code)
		}
	}

	body := `{"repo":"octocat/vault","token":"tok","files":[{"name":"a.txt","content_b64":"YQ=="}]}`
	rec := serve(h, httptest.NewRequest(http.MethodPost, "/api/notes/sync", strings.NewReader(body)))
	if rec.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q, want 200", rec.Code, rec.Body.String())
	}
	if got := rec.Header().Get("Content-Length"); got != strconv.Itoa(rec.Body.Len()) {
		t.Errorf("Content-Length=%q, want %d", got, rec.Body.Len())
	}
	if got := rec.Header().Get("Connection"); got != "close" {
		t.Errorf("Connection=%q, want close", got)
	}
	var resp SyncResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("reply not JSON: %v", err)
	}
	if resp.Uploaded != 1 {
		t.Errorf("resp=%+v, want Uploaded=1 (branch defaults to main)", resp)
	}
}

// A listing failure (non-404 error status) aborts the sync with a 502.
func TestSyncHandlerListFailureIs502(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusUnauthorized)
		_, _ = w.Write([]byte(`{"message":"Bad credentials"}`))
	}))
	t.Cleanup(srv.Close)
	h := &Handler{client: githubTestClient(t, srv), maxParallel: 4}
	body := `{"repo":"o/r","token":"t","files":[{"name":"a.txt","content_b64":"YQ=="}]}`
	rec := serve(h, httptest.NewRequest(http.MethodPost, "/api/notes/sync", strings.NewReader(body)))
	if rec.Code != http.StatusBadGateway || !strings.Contains(rec.Body.String(), "github 401") {
		t.Fatalf("code=%d body=%q, want 502 naming github 401", rec.Code, rec.Body.String())
	}
}
