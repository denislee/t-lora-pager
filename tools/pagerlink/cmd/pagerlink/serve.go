package main

// pagerlink serve: the mock phone for emulator work on HUB_REMOVAL_PLAN.md H2.
//
// The pager dials us (main already did); serve registers the phone-side
// responder handlers and answers pager-initiated requests (D→P) with canned
// results, optionally overridden per method by a -responses JSON file. It keeps
// running until SIGINT/SIGTERM or the link drops.
//
// Link-drop detection: the Client's Events() channel is never closed on
// shutdown (the reader simply stops), so a closed channel cannot signal a drop.
// Instead serve polls sys.ping every 5 s; a failing call means the reader has
// shut the link down, and serve returns. The closed-Events alternative does not
// work here.

import (
	"encoding/json"
	"flag"
	"fmt"
	"hash/crc32"
	"os"
	"os/signal"
	"path/filepath"
	"sync"
	"syscall"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

// mockPhone answers the pager's D→P requests on a connected Client. It is the
// emulator stand-in for the iOS companion: canned results for loc/weather/chat,
// a receiver for uploaded voice bytes, and an fs.read-driven notes sync.
type mockPhone struct {
	c        *pagerlink.Client
	notesOut string

	mu      sync.Mutex
	job     int
	audio   map[uint16]int // chat.audio.begin xfer -> bytes received
	replies map[string]json.RawMessage
}

// noteFile is one entry of a notes.sync manifest.
type noteFile struct {
	Name  string `json:"name"`
	Size  uint32 `json:"size"`
	Mtime uint32 `json:"mtime"`
	CRC32 uint32 `json:"crc32"`
}

// cmdServe implements `pagerlink serve`; the caller has already dialed.
func cmdServe(c *pagerlink.Client, addr string, timeout time.Duration, args []string) int {
	fs := flag.NewFlagSet("serve", flag.ContinueOnError)
	responses := fs.String("responses", "", "JSON file mapping method -> result object")
	notesOut := fs.String("notes-out", "", "directory to write notes bytes fetched during notes.sync")
	if err := fs.Parse(args); err != nil {
		return 2
	}

	srv := &mockPhone{c: c, notesOut: *notesOut, audio: make(map[uint16]int)}
	if *responses != "" {
		if err := srv.loadResponses(*responses); err != nil {
			fmt.Fprintf(os.Stderr, "pagerlink serve: %v\n", err)
			return 1
		}
	}
	srv.register()

	fmt.Printf("serve: mock phone on %s (Ctrl-C to stop)\n", addr)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(sig)

	tick := time.NewTicker(5 * time.Second)
	defer tick.Stop()
	for {
		select {
		case s := <-sig:
			fmt.Printf("serve: %v, stopping\n", s)
			return 0
		case <-tick.C:
			if _, err := c.Call("sys.ping", map[string]any{"t": time.Now().UnixMilli()}, timeout); err != nil {
				fmt.Printf("serve: link closed (%v)\n", err)
				return 0
			}
		}
	}
}

// register installs every handler. Built-in methods are registered first so a
// -responses entry for the same method replaces them (Handle overwrites).
func (s *mockPhone) register() {
	s.handle("sys.ping", func(params json.RawMessage) (any, *pagerlink.RPCError) {
		return params, nil // echo, per PROTOCOL.md §3
	})
	s.handle("loc.get", func(json.RawMessage) (any, *pagerlink.RPCError) {
		return json.RawMessage(`{"lat":-23.55,"lon":-46.63,"acc_m":30,"city":"Sao Paulo"}`), nil
	})
	s.handle("weather.get", func(json.RawMessage) (any, *pagerlink.RPCError) {
		return json.RawMessage(`{"src":"mock","key":"auto"}`), nil
	})
	s.handle("chat.ask", s.chatAsk)
	s.handle("chat.audio.begin", s.chatAudioBegin)
	s.handle("notes.sync", s.notesSync)

	for method, result := range s.replies {
		m, r := method, result
		s.handle(m, func(json.RawMessage) (any, *pagerlink.RPCError) {
			return r, nil
		})
	}
}

// handle registers h under method, logging each incoming request first.
func (s *mockPhone) handle(method string, h pagerlink.Handler) {
	s.c.Handle(method, func(params json.RawMessage) (any, *pagerlink.RPCError) {
		p := string(params)
		if p == "" {
			p = "{}"
		}
		fmt.Printf("serve: %s %s\n", method, truncate(p, 200))
		return h(params)
	})
}

// chatAsk answers chat.ask. With audio_xfer naming a received voice transfer it
// replies with the byte count and a transcript; otherwise it echoes params.text.
func (s *mockPhone) chatAsk(params json.RawMessage) (any, *pagerlink.RPCError) {
	var p struct {
		Text      string `json:"text"`
		AudioXfer uint16 `json:"audio_xfer"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "bad chat.ask params"}
	}
	if p.AudioXfer != 0 {
		s.mu.Lock()
		n, ok := s.audio[p.AudioXfer]
		s.mu.Unlock()
		if ok {
			return map[string]any{
				"reply":      fmt.Sprintf("(mock) audio %d bytes", n),
				"transcript": "(mock) transcript",
			}, nil
		}
	}
	return map[string]any{"reply": "(mock) " + p.Text}, nil
}

// chatAudioBegin accepts a voice upload: it installs the byte sink before
// answering {xfer}, then a goroutine waits for the DATA, checks its CRC, logs
// the outcome, and records the byte count for a later chat.ask audio_xfer.
func (s *mockPhone) chatAudioBegin(params json.RawMessage) (any, *pagerlink.RPCError) {
	var p struct {
		Size  uint32 `json:"size"`
		CRC32 uint32 `json:"crc32"`
		Fmt   string `json:"fmt"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "bad chat.audio.begin params"}
	}
	id, wait := s.c.AcceptBytes(p.Size, p.CRC32)
	go func() {
		data, err := wait(60 * time.Second)
		if err != nil {
			fmt.Printf("serve: chat.audio.begin xfer=%d fmt=%q: %v\n", id, p.Fmt, err)
			return
		}
		s.mu.Lock()
		s.audio[id] = len(data)
		s.mu.Unlock()
		fmt.Printf("serve: chat.audio.begin xfer=%d fmt=%q: %d bytes, crc32 0x%08x ok\n",
			id, p.Fmt, len(data), crc32.ChecksumIEEE(data))
	}()
	return map[string]any{"xfer": id}, nil
}

// notesSync answers {job} and pulls the manifest in the background.
func (s *mockPhone) notesSync(params json.RawMessage) (any, *pagerlink.RPCError) {
	var p struct {
		Targets []string   `json:"targets"`
		Files   []noteFile `json:"files"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "bad notes.sync params"}
	}
	s.mu.Lock()
	s.job++
	job := s.job
	s.mu.Unlock()
	go s.runNotesSync(job, p.Files)
	return map[string]any{"job": job}, nil
}

// runNotesSync pulls each manifest file with fs.read and reports progress on
// evt.notes.sync. A file whose bytes fail their CRC counts as an error.
func (s *mockPhone) runNotesSync(job int, files []noteFile) {
	uploaded := 0
	errs := []string{}
	for _, f := range files {
		data, _, err := s.c.ReadFile("int", "/notes/"+f.Name, 60*time.Second)
		if err == nil && s.notesOut != "" {
			if werr := os.WriteFile(filepath.Join(s.notesOut, filepath.Base(f.Name)), data, 0o644); werr != nil {
				err = werr
			}
		}
		if err != nil {
			errs = append(errs, f.Name)
			fmt.Printf("serve: notes.sync job=%d %s: %v\n", job, f.Name, err)
		} else {
			uploaded++
		}
		s.emitNotes(job, uploaded, errs, false)
	}
	s.emitNotes(job, uploaded, errs, true)
}

// emitNotes sends one evt.notes.sync progress update.
func (s *mockPhone) emitNotes(job, uploaded int, errs []string, done bool) {
	if err := s.c.Emit("evt.notes.sync", map[string]any{
		"job":      job,
		"phase":    "upload",
		"uploaded": uploaded,
		"already":  0,
		"errors":   errs,
		"done":     done,
	}); err != nil {
		fmt.Printf("serve: emit evt.notes.sync: %v\n", err)
	}
}

// loadResponses reads the method -> result JSON file.
func (s *mockPhone) loadResponses(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var replies map[string]json.RawMessage
	if err := json.Unmarshal(data, &replies); err != nil {
		return fmt.Errorf("parse %s: %w", path, err)
	}
	s.replies = replies
	return nil
}

// truncate caps s at max runes, appending an ellipsis when it cut.
func truncate(s string, max int) string {
	r := []rune(s)
	if len(r) <= max {
		return s
	}
	return string(r[:max]) + "…"
}
