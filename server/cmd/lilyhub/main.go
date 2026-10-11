// lilyhub — local-first server for LilyGo pager / watch apps. Runs on a Linux
// box on the same LAN as the device. The device tries it first; on failure it
// falls back to the public internet directly.
//
// Add new apps as new packages exposing a `Register(mux)` method, then wire
// them in main(). main owns the shared TTL cache (handed to weather) and the
// mux; each handler builds its own http.Client with a timeout suited to its
// upstream. Routes use Go 1.22 method patterns, so the mux answers a wrong
// method with 405.
package main

import (
	"context"
	"errors"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/lilygo/lilyhub/internal/cache"
	"github.com/lilygo/lilyhub/internal/chat"
	"github.com/lilygo/lilyhub/internal/httpx"
	"github.com/lilygo/lilyhub/internal/notessync"
	"github.com/lilygo/lilyhub/internal/telegram"
	"github.com/lilygo/lilyhub/internal/weather"
)

func main() {
	addr := flag.String("addr", envOr("LILYHUB_ADDR", ":8080"), "listen address")
	flag.Parse()

	c := cache.New()
	go sweepLoop(c)

	mux := http.NewServeMux()

	// The device never sends an HTTP request to decide hub reachability — it
	// does a raw TCP connect() with a short timeout (hal/hub.h's
	// hub_is_reachable). This endpoint exists for humans/curl to sanity-check
	// the process is up. Keep it cheap and side-effect free.
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok\n"))
	})

	weather.New(c).Register(mux)
	notessync.New().Register(mux)
	chat.New().Register(mux)
	telegram.New().Register(mux)

	srv := &http.Server{
		Addr:              *addr,
		Handler:           httpx.Logger(mux),
		ReadHeaderTimeout: 5 * time.Second,
		// Bound how long a client may take to send a full request body and how
		// long an idle keep-alive connection lingers — otherwise a slow-body
		// sender (the endpoints accept up to 8 MiB) parks a goroutine forever.
		// ReadTimeout is generous enough for an 8 MiB chat/notes upload over
		// LAN. WriteTimeout is intentionally left off: the chat path can
		// legitimately take up to ~60 s waiting on the upstream LLM.
		ReadTimeout: 60 * time.Second,
		IdleTimeout: 120 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	go func() {
		log.Printf("lilyhub listening on %s", *addr)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatal(err)
		}
	}()

	<-ctx.Done()
	log.Print("shutting down")
	// Notes-sync's http.Client timeout is 30s (notessync.go); give it margin to
	// finish an in-flight GitHub call instead of getting cancelled mid-sync.
	// Chat's up-to-60s upstream wait is allowed to die — history is in-memory.
	sd, cancel := context.WithTimeout(context.Background(), 35*time.Second)
	defer cancel()
	_ = srv.Shutdown(sd)
}

func sweepLoop(c *cache.Cache) {
	t := time.NewTicker(5 * time.Minute)
	defer t.Stop()
	for range t.C {
		c.Sweep()
	}
}

func envOr(k, def string) string {
	if v, ok := os.LookupEnv(k); ok {
		return v
	}
	return def
}
