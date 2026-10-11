package telegram

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// The SSRF guard is the security boundary for this proxy (it attaches the
// caller's bearer token to whatever it fetches), so cover the bypass tricks
// explicitly.
func TestAllowedTelegramURL(t *testing.T) {
	allowed := []string{
		"https://api.telegram.org/bot123456:ABC/getMe",
		"https://api.telegram.org/bot1/sendMessage",
		"https://API.TELEGRAM.ORG/bot1/getMe", // DNS is case-insensitive
		"https://api.telegram.org:443/bot1/getMe",
	}
	for _, u := range allowed {
		if !allowedTelegramURL(u) {
			t.Errorf("allowedTelegramURL(%q) = false, want true", u)
		}
	}

	blocked := []string{
		"",
		"http://api.telegram.org/bot1/getMe", // wrong scheme
		"https://169.254.169.254/latest/meta-data/",    // cloud metadata
		"http://localhost:8080/admin",                  // localhost admin
		"https://127.0.0.1/",                           // loopback
		"https://api.telegram.org.evil.com/bot1/getMe", // suffix trick
		"https://evil.com/api.telegram.org",            // path trick
		"https://api.telegram.org@evil.com/bot1/getMe", // userinfo trick
		"ftp://api.telegram.org/",                      // wrong scheme
		"file:///etc/passwd",                           // local file
		"https://[::1]/",                               // ipv6 loopback
	}
	for _, u := range blocked {
		if allowedTelegramURL(u) {
			t.Errorf("allowedTelegramURL(%q) = true, want false (SSRF)", u)
		}
	}
}

// A rejected URL must be turned away before any network call, so this test is
// deterministic without a fake upstream.
func TestProxyRejectsSSRFURL(t *testing.T) {
	h := New()
	body := `{"url":"http://169.254.169.254/latest/meta-data/","method":"GET","token":"secret"}`
	rec := httptest.NewRecorder()
	h.proxy(rec, httptest.NewRequest(http.MethodPost, "/api/telegram/proxy", strings.NewReader(body)))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("code = %d, want %d", rec.Code, http.StatusBadRequest)
	}
}

// The POST-only rule lives in the Go 1.22 route pattern, so go through the mux.
func TestProxyRejectsNonPost(t *testing.T) {
	mux := http.NewServeMux()
	New().Register(mux)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/telegram/proxy", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("code = %d, want %d", rec.Code, http.StatusMethodNotAllowed)
	}
}
