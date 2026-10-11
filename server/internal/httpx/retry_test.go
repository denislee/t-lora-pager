package httpx

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// fastBackoff shrinks baseBackoff for the duration of a test so retry tests
// do not actually wait 200ms between attempts.
func fastBackoff(t *testing.T) {
	t.Helper()
	old := baseBackoff
	baseBackoff = time.Millisecond
	t.Cleanup(func() { baseBackoff = old })
}

func getReq(t *testing.T, url string) *http.Request {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		t.Fatalf("http.NewRequest: %v", err)
	}
	return req
}

func TestDoWithRetryRetriesOn429ThenSucceeds(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if atomic.AddInt32(&attempts, 1) <= 2 {
			w.WriteHeader(http.StatusTooManyRequests)
			return
		}
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	}))
	defer srv.Close()

	body, status, err := DoWithRetry(srv.Client(), getReq(t, srv.URL), 3, "test")
	if err != nil {
		t.Fatalf("DoWithRetry: unexpected error: %v", err)
	}
	if status != http.StatusOK {
		t.Fatalf("status = %d, want %d", status, http.StatusOK)
	}
	if string(body) != "ok" {
		t.Fatalf("body = %q, want %q", body, "ok")
	}
	if got := atomic.LoadInt32(&attempts); got != 3 {
		t.Fatalf("attempts = %d, want 3", got)
	}
}

func TestDoWithRetryExhaustsRetriesOnPersistent5xx(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()

	_, status, err := DoWithRetry(srv.Client(), getReq(t, srv.URL), 3, "test")
	if err != nil {
		t.Fatalf("DoWithRetry: unexpected error: %v", err)
	}
	if status != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want %d", status, http.StatusServiceUnavailable)
	}
	if got := atomic.LoadInt32(&attempts); got != 3 {
		t.Fatalf("attempts = %d, want 3", got)
	}
}

func TestDoWithRetryRespectsOneRetryLimit(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()

	_, status, err := DoWithRetry(srv.Client(), getReq(t, srv.URL), 2, "test")
	if err != nil {
		t.Fatalf("DoWithRetry: unexpected error: %v", err)
	}
	if status != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want %d", status, http.StatusServiceUnavailable)
	}
	if got := atomic.LoadInt32(&attempts); got != 2 {
		t.Fatalf("attempts = %d, want 2", got)
	}
}

func TestDoWithRetryDoesNotRetryNonRetryable4xx(t *testing.T) {
	codes := []int{http.StatusBadRequest, http.StatusUnprocessableEntity}
	for _, code := range codes {
		t.Run(http.StatusText(code), func(t *testing.T) {
			fastBackoff(t)
			var attempts int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				atomic.AddInt32(&attempts, 1)
				w.WriteHeader(code)
			}))
			defer srv.Close()

			_, status, err := DoWithRetry(srv.Client(), getReq(t, srv.URL), 3, "test")
			if err != nil {
				t.Fatalf("DoWithRetry: unexpected error: %v", err)
			}
			if status != code {
				t.Fatalf("status = %d, want %d", status, code)
			}
			if got := atomic.LoadInt32(&attempts); got != 1 {
				t.Fatalf("attempts = %d, want 1", got)
			}
		})
	}
}

func TestDoWithRetryReplaysBodyAcrossRetries(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	var mu sync.Mutex
	var lastBody []byte
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		mu.Lock()
		lastBody = b
		mu.Unlock()
		if atomic.AddInt32(&attempts, 1) == 1 {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()

	const payload = `{"hello":"world"}`
	req, err := http.NewRequest(http.MethodPut, srv.URL, bytes.NewReader([]byte(payload)))
	if err != nil {
		t.Fatalf("http.NewRequest: %v", err)
	}
	_, status, err := DoWithRetry(srv.Client(), req, 3, "test")
	if err != nil {
		t.Fatalf("DoWithRetry: unexpected error: %v", err)
	}
	if status != http.StatusOK {
		t.Fatalf("status = %d, want %d", status, http.StatusOK)
	}
	if got := atomic.LoadInt32(&attempts); got != 2 {
		t.Fatalf("attempts = %d, want 2", got)
	}
	mu.Lock()
	got := string(lastBody)
	mu.Unlock()
	if got != payload {
		t.Fatalf("last request body = %q, want %q", got, payload)
	}
}

func TestDoWithRetryReturnsErrorOnPersistentTransportFailure(t *testing.T) {
	fastBackoff(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	url := srv.URL
	srv.Close()

	_, _, err := DoWithRetry(http.DefaultClient, getReq(t, url), 2, "test")
	if err == nil {
		t.Fatal("DoWithRetry: expected error after server closed, got nil")
	}
}

func TestDoWithRetryRejectsOversizedBody(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, strings.Repeat("a", maxBodyBytes+1))
	}))
	defer srv.Close()

	_, _, err := DoWithRetry(srv.Client(), getReq(t, srv.URL), 3, "test")
	if err == nil {
		t.Fatal("DoWithRetry: expected error for oversized body, got nil")
	}
	if !strings.Contains(err.Error(), "exceeds") {
		t.Fatalf("error = %q, want it to contain %q", err.Error(), "exceeds")
	}
	if got := atomic.LoadInt32(&attempts); got != 1 {
		t.Fatalf("attempts = %d, want 1 (oversized body must not be retried)", got)
	}
}

func TestDoWithRetryBodyExactlyAtLimit(t *testing.T) {
	fastBackoff(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, strings.Repeat("a", maxBodyBytes))
	}))
	defer srv.Close()

	body, status, err := DoWithRetry(srv.Client(), getReq(t, srv.URL), 2, "test")
	if err != nil {
		t.Fatalf("DoWithRetry: unexpected error: %v", err)
	}
	if status != http.StatusOK {
		t.Fatalf("status = %d, want %d", status, http.StatusOK)
	}
	if len(body) != maxBodyBytes {
		t.Fatalf("len(body) = %d, want %d", len(body), maxBodyBytes)
	}
}

func TestDoWithRetryStopsOnContextCancel(t *testing.T) {
	old := baseBackoff
	baseBackoff = 5 * time.Second
	t.Cleanup(func() { baseBackoff = old })

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()

	ctx, cancel := context.WithCancel(context.Background())
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, srv.URL, nil)
	if err != nil {
		t.Fatalf("http.NewRequestWithContext: %v", err)
	}
	go func() {
		time.Sleep(20 * time.Millisecond)
		cancel()
	}()

	start := time.Now()
	_, _, err = DoWithRetry(srv.Client(), req, 3, "test")
	elapsed := time.Since(start)

	if !errors.Is(err, context.Canceled) {
		t.Fatalf("err = %v, want context.Canceled", err)
	}
	if elapsed >= 5*time.Second {
		t.Fatalf("DoWithRetry took %s, want it to return well before the 5s backoff", elapsed)
	}
}

func TestTruncate(t *testing.T) {
	tests := []struct {
		name string
		in   string
		n    int
		want string
	}{
		{"shorter than limit", "abc", 5, "abc"},
		{"longer than limit", "abcdef", 3, "abc..."},
		{"exactly at limit", "abc", 3, "abc"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := Truncate(tt.in, tt.n); got != tt.want {
				t.Fatalf("Truncate(%q, %d) = %q, want %q", tt.in, tt.n, got, tt.want)
			}
		})
	}
}
