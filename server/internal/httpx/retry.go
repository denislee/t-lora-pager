package httpx

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"time"
)

// baseBackoff is the first retry delay; it doubles each attempt (capped at
// 2 s). A package var rather than a const so tests can shrink it.
var baseBackoff = 200 * time.Millisecond

// retryableStatus reports whether an upstream status is worth retrying. 429
// (rate limit) and 5xx (transient upstream) are; other 4xx (auth, bad model,
// validation, GitHub's 422 non-fast-forward) are permanent for this request,
// so retrying just burns time.
func retryableStatus(code int) bool {
	return code == http.StatusTooManyRequests || code >= 500
}

func backoff(attempt int) time.Duration {
	shift := max(attempt-2, 0) // first retry (attempt 2) waits baseBackoff
	return min(baseBackoff<<shift, 2*time.Second)
}

// DoWithRetry sends req with up to maxAttempts tries, backing off between
// them, and retries on transport/read errors and on 429/5xx. It returns the
// final attempt's body and status without judging the status: callers have
// status-specific rules (notessync treats a 404 listing as "no notes yet",
// chat turns any non-2xx into an error). err is non-nil only when no
// response was obtained, the context was cancelled, or the body exceeded
// maxBodyBytes — the last is reported as an error rather than silently
// truncated, since a half-read JSON body only fails later with a misleading
// parse error. logPrefix tags the retry log lines ("chat", "notessync").
//
// Bodies are replayed via req.GetBody, which http.NewRequest populates for
// the bytes.Reader/Buffer bodies the callers use.
func DoWithRetry(client *http.Client, req *http.Request, maxAttempts int, logPrefix string) ([]byte, int, error) {
	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		if attempt > 1 {
			d := backoff(attempt)
			log.Printf("%s: upstream %s %s retry %d/%d after %s (%v)",
				logPrefix, req.Method, req.URL.Path, attempt, maxAttempts, d, lastErr)
			timer := time.NewTimer(d)
			select {
			case <-timer.C:
			case <-req.Context().Done():
				timer.Stop()
				return nil, 0, req.Context().Err()
			}
			if req.GetBody != nil {
				b, err := req.GetBody()
				if err != nil {
					return nil, 0, err
				}
				req.Body = b
			}
		}
		resp, err := client.Do(req)
		if err != nil {
			lastErr = err // transport error — transient, retry
			continue
		}
		// Read one byte past the cap so an oversized body is distinguishable
		// from one that lands exactly on the limit.
		body, err := io.ReadAll(io.LimitReader(resp.Body, maxBodyBytes+1))
		resp.Body.Close()
		if err != nil {
			lastErr = err
			continue
		}
		if len(body) > maxBodyBytes {
			return nil, resp.StatusCode, fmt.Errorf("upstream %d: body exceeds %d bytes", resp.StatusCode, maxBodyBytes)
		}
		if retryableStatus(resp.StatusCode) && attempt < maxAttempts {
			lastErr = fmt.Errorf("upstream %d: %s", resp.StatusCode, Truncate(string(body), 200))
			continue
		}
		return body, resp.StatusCode, nil
	}
	return nil, 0, lastErr
}

// Truncate cuts s to at most n bytes, appending "..." when it had to cut.
// Used to keep upstream error bodies and logged prompts legible.
func Truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}
