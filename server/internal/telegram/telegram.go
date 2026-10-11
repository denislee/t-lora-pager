package telegram

import (
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/lilygo/lilyhub/internal/httpx"
)

type ProxyRequest struct {
	URL    string `json:"url"`
	Method string `json:"method"`
	Token  string `json:"token"`
	Body   string `json:"body,omitempty"`
}

type Handler struct {
	client *http.Client
}

func New() *Handler {
	return &Handler{
		client: &http.Client{
			Timeout: 15 * time.Second,
		},
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("POST /api/telegram/proxy", h.proxy)
}

// allowedTelegramURL reports whether raw is a URL this proxy may fetch. Only
// https://api.telegram.org (any case, any path/port) is permitted; the host
// match is the SSRF boundary since we forward the caller's bearer token. Host
// comparison is case-insensitive (DNS is), but userinfo tricks like
// "https://api.telegram.org@evil.com" resolve to Hostname()=="evil.com" and
// are correctly rejected.
func allowedTelegramURL(raw string) bool {
	u, err := url.Parse(raw)
	return err == nil && u.Scheme == "https" &&
		strings.ToLower(u.Hostname()) == "api.telegram.org"
}

func (h *Handler) proxy(w http.ResponseWriter, r *http.Request) {
	var req ProxyRequest
	if httpx.DecodeJSON(w, r, 1<<20, &req) != nil {
		return
	}
	// SSRF guard: this proxy attaches the caller-supplied bearer token to the
	// upstream request, so it must only ever reach Telegram. The old check
	// (HasPrefix(req.URL, "http")) let any LAN client relay to cloud metadata
	// endpoints, localhost admin ports, or any internal host — with the token
	// attached.
	if !allowedTelegramURL(req.URL) {
		http.Error(w, "url must be https://api.telegram.org/...", http.StatusBadRequest)
		return
	}
	if req.Method != "GET" && req.Method != "POST" {
		http.Error(w, "invalid method", http.StatusBadRequest)
		return
	}

	var bodyReader io.Reader
	if req.Body != "" {
		bodyReader = strings.NewReader(req.Body)
	}

	upstreamReq, err := http.NewRequestWithContext(r.Context(), req.Method, req.URL, bodyReader)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if req.Token != "" {
		upstreamReq.Header.Set("Authorization", "Bearer "+req.Token)
	}
	if req.Body != "" {
		upstreamReq.Header.Set("Content-Type", "application/json")
	}
	upstreamReq.Header.Set("User-Agent", "lilyhub/1.0")

	resp, err := h.client.Do(upstreamReq)
	if err != nil {
		http.Error(w, "upstream: "+err.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		http.Error(w, "upstream read: "+err.Error(), http.StatusBadGateway)
		return
	}
	// Relay Telegram's status and JSON body verbatim. Telegram reports errors
	// as structured JSON ({"ok":false,"error_code":...,"description":...}); the
	// old code flattened every non-200 into a text/plain 502, discarding the
	// payload the device needs to render a useful message.
	ct := resp.Header.Get("Content-Type")
	if ct == "" {
		ct = "application/json"
	}
	w.Header().Set("Content-Type", ct)
	w.WriteHeader(resp.StatusCode)
	_, _ = w.Write(respBody)
}
