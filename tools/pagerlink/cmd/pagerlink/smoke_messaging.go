package main

// The messaging, screen and firmware-update half of the smoke test: notify.post,
// clip.push, text.send and pager.find (PROTOCOL.md §6.5), screen.capture (§6.6)
// and ota.status / ota.apply (§6.7). These steps need the emulator: test.ui.open
// and test.inbox are emulator-only hooks, and the emulator's ota.apply ends with
// phase "installed" instead of rebooting.

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

// screenResult is what the screen.data receiver goroutine reports once the
// rgb565-rle stream has arrived or the transfer failed.
type screenResult struct {
	data   []byte
	err    error
	w, h   int
	format string
}

// smokeMessaging runs the messaging, screen and OTA smoke steps and reports
// each. c is the live client returned by smokeFS; addr is what it dialed.
func smokeMessaging(c *pagerlink.Client, addr string, timeout time.Duration) bool {
	pass := true

	// inbox reads the stored inbox ring as test.inbox reports it (/inbox.jsonl).
	inbox := func() (string, bool) {
		r, err := callInto[struct {
			JSONL string `json:"jsonl"`
		}](c, timeout, "test.inbox", nil)
		if err != nil {
			fmt.Printf("       test.inbox: %v\n", err)
			return "", false
		}
		return r.JSONL, true
	}

	// 1. A fresh notify.post is stored and shown, promptly, and lands in the
	//    inbox. The params are reused by step 2, so the id is minted once here.
	notifyParams := map[string]any{
		"title": "smoke hello",
		"body":  "line1\nline2",
		"id":    "smoke-" + strconv.FormatInt(time.Now().UnixNano(), 10),
		"src":   "smoke",
		"prio":  "normal",
	}
	start := time.Now()
	r, err := callInto[struct {
		Stored bool `json:"stored"`
	}](c, timeout, "notify.post", notifyParams)
	elapsed := time.Since(start)
	ok := err == nil
	if err != nil {
		fmt.Printf("       notify.post: %v\n", err)
	} else if !r.Stored {
		fmt.Printf("       notify.post result %+v: want stored true\n", r)
		ok = false
	}
	if elapsed >= time.Second {
		fmt.Printf("       notify.post took %s, want < 1s\n", elapsed.Round(time.Millisecond))
		ok = false
	}
	if jsonl, iok := inbox(); !iok {
		ok = false
	} else if !strings.Contains(jsonl, "smoke hello") {
		fmt.Printf("       test.inbox %q does not contain \"smoke hello\"\n", jsonl)
		ok = false
	}
	pass = report(ok, "notify.post stored, shown") && pass

	// 2. The same id again is deduped: stored false, dup true.
	ok = true
	type notifyRetryResult struct {
		Stored bool `json:"stored"`
		Dup    bool `json:"dup"`
	}
	if r, err := callInto[notifyRetryResult](c, timeout, "notify.post", notifyParams); err != nil {
		fmt.Printf("       notify.post retry: %v\n", err)
		ok = false
	} else if r.Stored || !r.Dup {
		fmt.Printf("       notify.post retry result %+v: want stored false, dup true\n", r)
		ok = false
	}
	pass = report(ok, "notify.post same id -> dup") && pass

	// 3. A title over 64 bytes is refused before it reaches the inbox.
	pass = report(rpcErrCode(c, timeout, "notify.post",
		map[string]any{"title": strings.Repeat("x", 65)}, "invalid_argument"),
		"notify.post title too long -> invalid_argument") && pass

	// 4. With no editor open, clip.push is kept in the inbox instead of pasted.
	//    Open Inbox first so any editor left from an earlier run is closed.
	ok = true
	if _, err := c.Call("test.ui.open", map[string]any{"app": "Inbox"}, timeout); err != nil {
		fmt.Printf("       test.ui.open Inbox: %v\n", err)
		ok = false
	}
	time.Sleep(500 * time.Millisecond)
	if r, err := callInto[struct {
		Pasted bool `json:"pasted"`
	}](c, timeout, "clip.push", map[string]any{"text": "smoke clip"}); err != nil {
		fmt.Printf("       clip.push: %v\n", err)
		ok = false
	} else if r.Pasted {
		fmt.Printf("       clip.push result %+v: want pasted false\n", r)
		ok = false
	}
	if jsonl, iok := inbox(); !iok {
		ok = false
	} else if !strings.Contains(jsonl, "smoke clip") {
		fmt.Printf("       test.inbox %q does not contain \"smoke clip\"\n", jsonl)
		ok = false
	}
	pass = report(ok, "clip.push with no editor -> kept in inbox") && pass

	// 5. text.send to the editor with no editor open is refused.
	pass = report(rpcErrCode(c, timeout, "text.send",
		map[string]any{"text": "x", "target": "editor"}, "invalid_argument"),
		"text.send editor with editor closed -> invalid_argument") && pass

	// 6. text.send to the keyboard with no text field focused is refused too.
	pass = report(rpcErrCode(c, timeout, "text.send",
		map[string]any{"text": "x", "target": "kb"}, "invalid_argument"),
		"text.send kb with no text field -> invalid_argument") && pass

	// 7. With the editor open, text.send types into the note (16 bytes: "typed
	//    from smoke") and clip.push pastes at the cursor instead of storing.
	//    The app opens asynchronously, so poll text.send until it is accepted.
	ok = true
	if _, err := c.Call("test.ui.open", map[string]any{"app": "Editor"}, timeout); err != nil {
		fmt.Printf("       test.ui.open Editor: %v\n", err)
		ok = false
	}
	typed := 0
	var lastErr error
	deadline := time.Now().Add(3 * time.Second)
	for {
		r, err := callInto[struct {
			Typed int `json:"typed"`
		}](c, timeout, "text.send",
			map[string]any{"text": "typed from smoke", "target": "editor"})
		if err == nil {
			typed, lastErr = r.Typed, nil
			break
		}
		lastErr = err
		if time.Now().After(deadline) {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if lastErr != nil {
		fmt.Printf("       text.send editor: %v\n", lastErr)
		ok = false
	}
	if typed != 16 {
		fmt.Printf("       text.send typed %d, want 16\n", typed)
		ok = false
	}
	if r, err := callInto[struct {
		Pasted bool `json:"pasted"`
	}](c, timeout, "clip.push", map[string]any{"text": "pasted"}); err != nil {
		fmt.Printf("       clip.push pasted: %v\n", err)
		ok = false
	} else if !r.Pasted {
		fmt.Printf("       clip.push result %+v: want pasted true\n", r)
		ok = false
	}
	if _, err := c.Call("test.ui.open", map[string]any{"app": "Inbox"}, timeout); err != nil {
		fmt.Printf("       test.ui.open Inbox: %v\n", err)
		ok = false
	}
	pass = report(ok, "editor open: text.send + clip.push land in the note") && pass

	// 8. pager.find starts the find-me loop and stops it again.
	pass = report(callOK(c, timeout, "pager.find", nil) &&
		callOK(c, timeout, "pager.find", map[string]any{"stop": true}),
		"pager.find start + stop") && pass

	// 9. screen.capture announces screen.data, then streams the framebuffer as
	//    rgb565-rle DATA; the phone decodes it back to exactly w*h pixels.
	screenCh := make(chan screenResult, 1)
	c.Handle("screen.data", func(params json.RawMessage) (any, *pagerlink.RPCError) {
		var p struct {
			Job   string `json:"job"`
			W     int    `json:"w"`
			H     int    `json:"h"`
			Fmt   string `json:"fmt"`
			Size  uint32 `json:"size"`
			CRC32 uint32 `json:"crc32"`
		}
		if err := json.Unmarshal(params, &p); err != nil {
			return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: err.Error()}
		}
		xfer, wait := c.AcceptBytes(p.Size, p.CRC32)
		go func() {
			data, err := wait(15 * time.Second)
			screenCh <- screenResult{data: data, err: err, w: p.W, h: p.H, format: p.Fmt}
		}()
		return map[string]any{"xfer": xfer}, nil
	})
	ok = true
	if r, err := callInto[struct {
		Job string `json:"job"`
	}](c, timeout, "screen.capture", nil); err != nil {
		fmt.Printf("       screen.capture: %v\n", err)
		ok = false
	} else if r.Job == "" {
		fmt.Printf("       screen.capture result %+v: no job\n", r)
		ok = false
	}
	if ok {
		select {
		case sr := <-screenCh:
			if sr.err != nil {
				fmt.Printf("       screen.data: %v\n", sr.err)
				ok = false
			} else if sr.format != "rgb565-rle" || sr.w <= 0 || sr.h <= 0 {
				fmt.Printf("       screen.data: %dx%d, fmt %q\n", sr.w, sr.h, sr.format)
				ok = false
			} else {
				px, derr := pagerlink.DecodeRLE565(sr.data, sr.w, sr.h)
				switch {
				case derr != nil:
					fmt.Printf("       DecodeRLE565(%d, %d): %v\n", sr.w, sr.h, derr)
					ok = false
				case len(px) != sr.w*sr.h:
					fmt.Printf("       DecodeRLE565: %d pixels, want %d\n", len(px), sr.w*sr.h)
					ok = false
				default:
					fmt.Printf("       %dx%d, %d bytes rle\n", sr.w, sr.h, len(sr.data))
				}
			}
		case <-time.After(20 * time.Second):
			fmt.Printf("       screen.data: no transfer within 20s\n")
			ok = false
		}
	}
	pass = report(ok, "screen.capture -> screen.data rgb565-rle decodes to w*h") && pass

	// 10. The OTA status names the running and next slot and a slot size.
	ok = true
	type otaStatusResult struct {
		Running string `json:"running"`
		Next    string `json:"next"`
		Pending bool   `json:"pending"`
		Boots   int    `json:"boots"`
		MaxSize int    `json:"max_size"`
	}
	if st, err := callInto[otaStatusResult](c, timeout, "ota.status", nil); err != nil {
		fmt.Printf("       ota.status: %v\n", err)
		ok = false
	} else if st.Running == "" || st.MaxSize <= 0 {
		fmt.Printf("       ota.status: running %q, max_size %d\n", st.Running, st.MaxSize)
		ok = false
	}
	pass = report(ok, "ota.status") && pass

	// 11. A sha256 that is not 64 hex digits is refused up front.
	pass = report(rpcErrCode(c, timeout, "ota.apply",
		map[string]any{"vol": "int", "path": "/ota-smoke.bin", "size": 1, "sha256": "xyz"},
		"invalid_argument"),
		"ota.apply bad sha256 -> invalid_argument") && pass

	// 12. A well-formed image verifies, writes and installs. The emulator ends
	//     with phase "installed" instead of "reboot".
	var otaImages []string
	img := make([]byte, 200000)
	img[0] = 0xE9
	for i := 1; i < len(img); i++ {
		img[i] = byte(i * 7)
	}
	ok = true
	if err := c.WriteFile("int", "/ota-smoke.bin", img, true, 30*time.Second); err != nil {
		fmt.Printf("       fs.write /ota-smoke.bin: %v\n", err)
		ok = false
	} else {
		otaImages = append(otaImages, "/ota-smoke.bin")
	}
	if ok {
		sum := sha256.Sum256(img)
		phases, last, msg, jerr := otaJob(c, timeout, map[string]any{
			"vol": "int", "path": "/ota-smoke.bin", "size": len(img),
			"sha256": hex.EncodeToString(sum[:]),
		})
		ok = jerr == nil && last == "installed" &&
			slices.Contains(phases, "verify") && slices.Contains(phases, "write")
		if !ok {
			fmt.Printf("       ota.apply: last %q, msg %q, phases %v, err %v\n",
				last, msg, phases, jerr)
		}
	}
	pass = report(ok, "ota.apply good image -> installed") && pass

	// 13. The same file with a different digest is accepted, then the job's
	//     verify pass reports the sha256 mismatch.
	wrong := sha256.Sum256([]byte("other"))
	phases, last, msg, jerr := otaJob(c, timeout, map[string]any{
		"vol": "int", "path": "/ota-smoke.bin", "size": len(img),
		"sha256": hex.EncodeToString(wrong[:]),
	})
	ok = jerr == nil && last == "error" && strings.Contains(msg, "sha256")
	if !ok {
		fmt.Printf("       ota.apply sha mismatch: last %q, msg %q, phases %v, err %v\n",
			last, msg, phases, jerr)
	}
	pass = report(ok, "ota.apply sha mismatch -> evt.ota error") && pass

	// 14. A correctly hashed file that is not an ESP32 app image is refused by
	//     the verify pass instead of being written.
	bad := make([]byte, 5000)
	for i := range bad {
		bad[i] = 0x7F
	}
	ok = true
	if err := c.WriteFile("int", "/ota-bad.bin", bad, true, 30*time.Second); err != nil {
		fmt.Printf("       fs.write /ota-bad.bin: %v\n", err)
		ok = false
	} else {
		otaImages = append(otaImages, "/ota-bad.bin")
	}
	if ok {
		bsum := sha256.Sum256(bad)
		phases, last, msg, jerr := otaJob(c, timeout, map[string]any{
			"vol": "int", "path": "/ota-bad.bin", "size": len(bad),
			"sha256": hex.EncodeToString(bsum[:]),
		})
		ok = jerr == nil && last == "error" && strings.Contains(msg, "not an ESP32 app image")
		if !ok {
			fmt.Printf("       ota.apply non-ESP image: last %q, msg %q, phases %v, err %v\n",
				last, msg, phases, jerr)
		}
	}
	pass = report(ok, "ota.apply non-ESP image -> evt.ota error") && pass

	// 15. Leave nothing behind: the images on the int volume go over fs.delete,
	//     and the image the emulator installed goes from this side.
	ok = true
	for _, path := range otaImages {
		if _, err := c.Call("fs.delete", map[string]any{"vol": "int", "path": path}, timeout); err != nil {
			fmt.Printf("       fs.delete %s: %v\n", path, err)
			ok = false
		}
	}
	pass = report(ok, "OTA images removed") && pass
	removeInstalledImage(addr)

	return pass
}

// removeInstalledImage deletes the file a successful ota.apply leaves on the
// emulator, $PAGERLINK_FS_ROOT/ota/installed.bin (src/hal/ota.cpp). It is outside
// both fs.* volumes, so fs.delete cannot reach it and only a smoke on the
// emulator's own machine can. The root follows the emulator's rule, read from
// this process's environment: $PAGERLINK_FS_ROOT, else /tmp/pagerlink-fs. An
// emulator started with another root keeps its copy, which is why this only
// says what happened and never fails the smoke.
func removeInstalledImage(addr string) {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		fmt.Printf("       installed image: address %q: %v\n", addr, err)
		return
	}
	if ip := net.ParseIP(host); host != "localhost" && (ip == nil || !ip.IsLoopback()) {
		fmt.Printf("       installed image: %s is not this machine, left in place\n", host)
		return
	}
	root := os.Getenv("PAGERLINK_FS_ROOT")
	if root == "" {
		root = "/tmp/pagerlink-fs"
	}
	path := filepath.Join(root, "ota", "installed.bin")
	switch err := os.Remove(path); {
	case err == nil:
		fmt.Printf("       removed %s\n", path)
	case errors.Is(err, os.ErrNotExist):
		fmt.Printf("       %s not found; set PAGERLINK_FS_ROOT to the emulator's root\n", path)
	default:
		fmt.Printf("       remove %s: %v\n", path, err)
	}
}

// otaJob starts one ota.apply and follows its evt.ota stream, ignoring events
// for other jobs, until the job reaches a terminal phase ("installed" or
// "error"). It returns every phase seen for the job, the last phase, that
// phase's msg, and the first transport or decoding error.
func otaJob(c *pagerlink.Client, timeout time.Duration, params any) (phases []string, last, msg string, err error) {
	started, err := callInto[struct {
		Job string `json:"job"`
	}](c, timeout, "ota.apply", params)
	if err != nil {
		return nil, "", "", fmt.Errorf("ota.apply: %w", err)
	}
	if started.Job == "" {
		return nil, "", "", fmt.Errorf("ota.apply: no job")
	}
	deadline := time.Now().Add(60 * time.Second)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return phases, last, msg, fmt.Errorf("ota %s: no terminal evt.ota within 60s", started.Job)
		}
		if remaining > 20*time.Second {
			remaining = 20 * time.Second
		}
		n, ok := waitFor(c, "evt.ota", remaining, nil)
		if !ok {
			return phases, last, msg, fmt.Errorf("ota %s: no evt.ota within %s", started.Job, remaining.Round(time.Second))
		}
		var ev struct {
			Job   string `json:"job"`
			Phase string `json:"phase"`
			Done  int    `json:"done"`
			Total int    `json:"total"`
			Msg   string `json:"msg"`
		}
		if err := json.Unmarshal(n.Params, &ev); err != nil {
			return phases, last, msg, fmt.Errorf("evt.ota params %q: %w", n.Params, err)
		}
		if ev.Job != started.Job {
			continue
		}
		phases = append(phases, ev.Phase)
		last, msg = ev.Phase, ev.Msg
		if ev.Phase == "installed" || ev.Phase == "error" {
			return phases, last, msg, nil
		}
	}
}
