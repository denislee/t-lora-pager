// Package chat mounts /api/chat — a single-shot chat endpoint that proxies
// to Groq's free OpenAI-compatible API. The device sends either a typed
// prompt or a base64-encoded WAV; we transcribe with Whisper if needed,
// run the chat completion, and return the reply as plain text.
//
// Why Groq: free tier, OpenAI-compatible (so the same client code can swap
// to Ollama later by changing one URL), and fast enough that the small
// pager screen doesn't need streaming for v1.
//
// History lives in process memory keyed by device_id. A pager is
// effectively single-user, so a sync.Mutex-guarded map is plenty — we
// don't pull in SQLite for ~20 messages per session.
package chat

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"mime/multipart"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/lilygo/lilyhub/internal/httpx"
)

const (
	groqBase     = "https://api.groq.com/openai/v1"
	sttModel     = "whisper-large-v3-turbo"
	chatModel    = "llama-3.3-70b-versatile"
	systemPrompt = "You are a concise assistant on a small pager screen. " +
		"Answer in plain text, no markdown, ≤120 words unless asked."

	maxHistory       = 20        // messages, dropping oldest pairs
	maxContent       = 4 * 1024  // per-message cap before we trim
	maxRequestBytes  = 8 << 20   // 8 MiB JSON envelope
	sessionIdleLimit = time.Hour // sessions idle this long are reaped
	maxSessions      = 256       // hard cap on live sessions (LRU-evicted)
	clientTimeout    = 60 * time.Second
)

type Request struct {
	DeviceID string `json:"device_id"`
	Text     string `json:"text,omitempty"`
	AudioB64 string `json:"audio_b64,omitempty"`
	Reset    bool   `json:"reset,omitempty"`
}

type Response struct {
	Transcript string `json:"transcript,omitempty"`
	Reply      string `json:"reply"`
}

type message struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type session struct {
	msgs    []message
	updated time.Time
}

type Handler struct {
	client    *http.Client
	apiKey    string
	baseURL   string // upstream base; a field (not the const) so tests can point it at a fake
	sttLang   string
	sttPrompt string
	mu        sync.Mutex
	sessions  map[string]*session
	lastReap  time.Time
}

// Defaults bias the transcription toward Brazilian Portuguese. Whisper's
// `language` field only takes ISO-639-1 (no country variant), so to push
// the model toward pt-BR vocabulary/spelling we send a short biasing
// prompt in the same regional style — Whisper uses it as a vocabulary
// hint, not as text that gets prepended to the output.
const (
	defaultSTTLang   = "pt"
	defaultSTTPrompt = "Olá, tudo bem? Estou usando um celular, " +
		"ônibus, trem, geladeira, abacaxi, açaí, cafezinho, computador."
)

func New() *Handler {
	key := os.Getenv("GROQ_API_KEY")
	// STT_LANG: ISO-639-1 hint. Default "pt" (Portuguese, any region).
	//   Set STT_LANG_AUTO=1 to disable and let Whisper auto-detect.
	// STT_PROMPT: free-form vocabulary/style hint used to bias regional
	//   variants (e.g. pt-BR vs pt-PT). Set STT_PROMPT="" to send no
	//   prompt at all.
	sttLang := os.Getenv("STT_LANG")
	if sttLang == "" && os.Getenv("STT_LANG_AUTO") == "" {
		sttLang = defaultSTTLang
	}
	sttPrompt, hasPrompt := os.LookupEnv("STT_PROMPT")
	if !hasPrompt {
		sttPrompt = defaultSTTPrompt
	}
	if key == "" {
		log.Printf("chat: GROQ_API_KEY not set — /api/chat will return 503")
	} else {
		log.Printf("chat: handler ready (model=%s stt=%s stt_lang=%q "+
			"stt_prompt_len=%d key=%s)",
			chatModel, sttModel, sttLang, len(sttPrompt), redactKey(key))
	}
	return &Handler{
		client:    &http.Client{Timeout: clientTimeout},
		apiKey:    key,
		baseURL:   groqBase,
		sttLang:   sttLang,
		sttPrompt: sttPrompt,
		sessions:  make(map[string]*session),
	}
}

// redactKey returns a safe-to-log fingerprint of the API key — first 6
// chars (Groq prefixes are like "gsk_…") plus the length. Never log the
// full key, even at debug level.
func redactKey(k string) string {
	if len(k) <= 6 {
		return fmt.Sprintf("len=%d", len(k))
	}
	return fmt.Sprintf("%s...len=%d", k[:6], len(k))
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("POST /api/chat", h.chat)
}

func (h *Handler) chat(w http.ResponseWriter, r *http.Request) {
	start := time.Now()
	var req Request
	if err := httpx.DecodeJSON(w, r, maxRequestBytes, &req); err != nil {
		log.Printf("chat: %s bad json: %v", r.RemoteAddr, err)
		return
	}
	if req.DeviceID == "" {
		log.Printf("chat: %s missing device_id", r.RemoteAddr)
		http.Error(w, "device_id required", http.StatusBadRequest)
		return
	}

	log.Printf("chat: req device=%s text_len=%d audio_b64_len=%d reset=%v",
		req.DeviceID, len(req.Text), len(req.AudioB64), req.Reset)

	if req.Reset {
		h.mu.Lock()
		delete(h.sessions, req.DeviceID)
		h.mu.Unlock()
		log.Printf("chat: device=%s session reset", req.DeviceID)
		httpx.WriteJSON(w, &Response{Reply: ""})
		return
	}

	if h.apiKey == "" {
		log.Printf("chat: device=%s rejected — GROQ_API_KEY not set", req.DeviceID)
		http.Error(w, "GROQ_API_KEY not set on hub", http.StatusServiceUnavailable)
		return
	}

	resp := &Response{}
	prompt := strings.TrimSpace(req.Text)

	// Audio takes precedence: if both are set, voice is what we treat as
	// the user's actual turn, and the device is free to send a hint in
	// `text`. We still return the transcript so the device can show
	// what we heard.
	if req.AudioB64 != "" {
		log.Printf("chat: device=%s stt start (audio_bytes~%d)",
			req.DeviceID, base64.StdEncoding.DecodedLen(len(req.AudioB64)))
		sttStart := time.Now()
		t, err := h.transcribe(r.Context(), req.AudioB64)
		req.AudioB64 = "" // stream-decoded already; drop the ~6 MiB string before the LLM call below
		var badAudio *errBadAudio
		if errors.As(err, &badAudio) {
			log.Printf("chat: device=%s bad audio_b64: %v", req.DeviceID, err)
			http.Error(w, "bad audio_b64", http.StatusBadRequest)
			return
		}
		if err != nil {
			log.Printf("chat: device=%s stt FAIL after %s: %v",
				req.DeviceID, time.Since(sttStart).Round(time.Millisecond), err)
			http.Error(w, "stt: "+err.Error(), http.StatusBadGateway)
			return
		}
		log.Printf("chat: device=%s stt ok in %s (%d chars): %q",
			req.DeviceID, time.Since(sttStart).Round(time.Millisecond),
			len(t), httpx.Truncate(t, 80))
		resp.Transcript = t
		prompt = strings.TrimSpace(t)
	}

	if prompt == "" {
		log.Printf("chat: device=%s rejected — empty prompt after trim/STT",
			req.DeviceID)
		http.Error(w, "text or audio_b64 required", http.StatusBadRequest)
		return
	}

	log.Printf("chat: device=%s llm start (prompt_chars=%d): %q",
		req.DeviceID, len(prompt), httpx.Truncate(prompt, 80))
	llmStart := time.Now()
	reply, err := h.complete(r.Context(), req.DeviceID, prompt)
	if err != nil {
		log.Printf("chat: device=%s llm FAIL after %s: %v",
			req.DeviceID, time.Since(llmStart).Round(time.Millisecond), err)
		http.Error(w, "llm: "+err.Error(), http.StatusBadGateway)
		return
	}
	log.Printf("chat: device=%s llm ok in %s (%d chars): %q",
		req.DeviceID, time.Since(llmStart).Round(time.Millisecond),
		len(reply), httpx.Truncate(reply, 80))
	resp.Reply = reply
	httpx.WriteJSON(w, resp)
	log.Printf("chat: device=%s done total=%s",
		req.DeviceID, time.Since(start).Round(time.Millisecond))
}

// errBadAudio marks a failure to base64-decode the caller's audio, as
// opposed to an upstream STT failure — chat() uses it to pick the right
// HTTP status code.
type errBadAudio struct{ err error }

func (e *errBadAudio) Error() string { return "bad audio_b64: " + e.err.Error() }
func (e *errBadAudio) Unwrap() error { return e.err }

// transcribe sends the WAV bytes to Groq's Whisper endpoint as multipart
// form data. audioB64 is decoded straight into the multipart writer so the
// base64 string, the decoded bytes, and the multipart buffer are never all
// live at once — only the string and the (smaller) multipart buffer are.
// We name the file with a .wav extension so Groq's content-sniffer doesn't
// reject it on extension grounds — the actual audio format is detected
// from the bytes.
func (h *Handler) transcribe(ctx context.Context, audioB64 string) (string, error) {
	var buf bytes.Buffer
	buf.Grow(base64.StdEncoding.DecodedLen(len(audioB64)) + 512) // avoid doubling reallocs through ~4.5 MiB of writes
	mw := multipart.NewWriter(&buf)
	if err := mw.WriteField("model", sttModel); err != nil {
		return "", err
	}
	if err := mw.WriteField("response_format", "json"); err != nil {
		return "", err
	}
	if h.sttLang != "" {
		if err := mw.WriteField("language", h.sttLang); err != nil {
			return "", err
		}
	}
	if h.sttPrompt != "" {
		if err := mw.WriteField("prompt", h.sttPrompt); err != nil {
			return "", err
		}
	}
	fw, err := mw.CreateFormFile("file", "audio.wav")
	if err != nil {
		return "", err
	}
	dec := base64.NewDecoder(base64.StdEncoding, strings.NewReader(audioB64))
	if _, err := io.Copy(fw, dec); err != nil {
		return "", &errBadAudio{err}
	}
	if err := mw.Close(); err != nil {
		return "", err
	}

	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost,
		h.baseURL+"/audio/transcriptions", &buf)
	if err != nil {
		return "", err
	}
	httpReq.Header.Set("Authorization", "Bearer "+h.apiKey)
	httpReq.Header.Set("Content-Type", mw.FormDataContentType())

	body, err := h.do(httpReq)
	if err != nil {
		return "", err
	}
	var out struct {
		Text  string `json:"text"`
		Error *struct {
			Message string `json:"message"`
		} `json:"error,omitempty"`
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	if out.Error != nil {
		return "", fmt.Errorf("groq: %s", out.Error.Message)
	}
	return strings.TrimSpace(out.Text), nil
}

// complete appends the prompt to the device's session, calls Groq's
// chat-completions endpoint, and stores the reply. The session is
// trimmed to maxHistory before the call so the upstream payload stays
// bounded across long conversations.
func (h *Handler) complete(ctx context.Context, deviceID, prompt string) (string, error) {
	h.mu.Lock()
	now := time.Now()
	if now.Sub(h.lastReap) > time.Minute {
		h.reapLocked(now)
		h.lastReap = now
	}
	s, ok := h.sessions[deviceID]
	if !ok {
		s = &session{}
		h.sessions[deviceID] = s
	}
	// Mark the session active before we release the lock for the network call.
	// (Also, stamping now first means the just-created session is the newest,
	// so the LRU cap below never evicts it.)
	// A newly created session (updated == zero) — or one whose last activity is
	// older than the idle cutoff — would otherwise be evictable by a concurrent
	// reapLocked() while this call is in flight, and the reply plus this turn's
	// history get silently dropped when we re-lock below.
	s.updated = now
	if !ok {
		// A new session grew the map — enforce the cap so a burst of unique
		// device_ids can't balloon memory (each session is up to ~80 KiB)
		// before the 1h idle reaper runs. Evicting by `updated` (LRU) after the
		// stamp above guarantees this session is never the victim.
		h.evictSessionsLocked(maxSessions)
	}
	s.msgs = append(s.msgs, message{Role: "user", Content: httpx.Truncate(prompt, maxContent)})
	if len(s.msgs) > maxHistory {
		s.msgs = s.msgs[len(s.msgs)-maxHistory:]
	}
	// Snapshot under the lock so we can release before the network call.
	snapshot := make([]message, 0, len(s.msgs)+1)
	snapshot = append(snapshot, message{Role: "system", Content: systemPrompt})
	snapshot = append(snapshot, s.msgs...)
	h.mu.Unlock()

	body, err := json.Marshal(map[string]any{
		"model":    chatModel,
		"messages": snapshot,
	})
	if err != nil {
		return "", err
	}
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost,
		h.baseURL+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	httpReq.Header.Set("Authorization", "Bearer "+h.apiKey)
	httpReq.Header.Set("Content-Type", "application/json")

	respBody, err := h.do(httpReq)
	if err != nil {
		return "", err
	}
	var out struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
		Error *struct {
			Message string `json:"message"`
		} `json:"error,omitempty"`
	}
	if err := json.Unmarshal(respBody, &out); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	if out.Error != nil {
		return "", fmt.Errorf("groq: %s", out.Error.Message)
	}
	if len(out.Choices) == 0 {
		return "", errors.New("no choices in response")
	}
	reply := strings.TrimSpace(out.Choices[0].Message.Content)

	h.mu.Lock()
	if s, ok := h.sessions[deviceID]; ok {
		s.msgs = append(s.msgs, message{Role: "assistant", Content: httpx.Truncate(reply, maxContent)})
		s.updated = time.Now()
	}
	h.mu.Unlock()

	return reply, nil
}

// do executes the request through httpx.DoWithRetry (3 attempts, retrying
// 429/5xx and transport errors) and returns the body. Non-2xx responses are
// surfaced as errors so the caller doesn't peek at status codes; the error
// string carries a short prefix of the upstream body so Groq's auth /
// rate-limit messages stay legible in the device log. One hub-side retry is
// far cheaper than bouncing the device to its own slow public-internet
// fallback.
func (h *Handler) do(req *http.Request) ([]byte, error) {
	start := time.Now()
	body, status, err := httpx.DoWithRetry(h.client, req, 3, "chat")
	if err != nil {
		log.Printf("chat: upstream %s %s failed after %s: %v",
			req.Method, req.URL.Path, time.Since(start).Round(time.Millisecond), err)
		return nil, err
	}
	log.Printf("chat: upstream %s %s -> %d in %s (%d bytes)",
		req.Method, req.URL.Path, status,
		time.Since(start).Round(time.Millisecond), len(body))
	if status/100 != 2 {
		return body, fmt.Errorf("upstream %d: %s", status, httpx.Truncate(string(body), 200))
	}
	return body, nil
}

// reapLocked drops sessions that have been idle for sessionIdleLimit.
// Called periodically so we don't need a goroutine.
func (h *Handler) reapLocked(now time.Time) {
	cutoff := now.Add(-sessionIdleLimit)
	for k, s := range h.sessions {
		if s.updated.Before(cutoff) {
			delete(h.sessions, k)
		}
	}
}

// evictSessionsLocked drops least-recently-used sessions (oldest `updated`)
// until at most `limit` remain. The idle reaper alone can't bound memory: a
// burst of unique device_ids inside the idle window grows the map unchecked.
// O(n) per eviction, but evictions only fire at the cap and n is small. Caller
// holds h.mu. An evicted session that is mid-flight elsewhere is handled
// gracefully — the in-flight call re-locks, finds it gone, and skips the store.
func (h *Handler) evictSessionsLocked(limit int) {
	for len(h.sessions) > limit {
		var oldestKey string
		var oldestTime time.Time
		first := true
		for k, s := range h.sessions {
			if first || s.updated.Before(oldestTime) {
				oldestKey, oldestTime, first = k, s.updated, false
			}
		}
		if first {
			return
		}
		delete(h.sessions, oldestKey)
	}
}
