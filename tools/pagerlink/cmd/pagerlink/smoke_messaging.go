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
	"fmt"
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
// each. c is the live client returned by smokeFS.
func smokeMessaging(c *pagerlink.Client, timeout time.Duration) bool {
	pass := true

	// inbox reads the stored inbox ring as test.inbox reports it (/inbox.jsonl).
	inbox := func() (string, bool) {
		res, err := c.Call("test.inbox", nil, timeout)
		if err != nil {
			fmt.Printf("       test.inbox: %v\n", err)
			return "", false
		}
		var r struct {
			JSONL string `json:"jsonl"`
		}
		if err := json.Unmarshal(res, &r); err != nil {
			fmt.Printf("       test.inbox result %q: %v\n", res, err)
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
	res, err := c.Call("notify.post", notifyParams, timeout)
	elapsed := time.Since(start)
	ok := err == nil
	if err != nil {
		fmt.Printf("       notify.post: %v\n", err)
	} else {
		var r struct {
			Stored bool `json:"stored"`
		}
		if uerr := json.Unmarshal(res, &r); uerr != nil {
			fmt.Printf("       notify.post result %q: %v\n", res, uerr)
			ok = false
		} else if !r.Stored {
			fmt.Printf("       notify.post result %q: want stored true\n", res)
			ok = false
		}
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
	if res, err := c.Call("notify.post", notifyParams, timeout); err != nil {
		fmt.Printf("       notify.post retry: %v\n", err)
		ok = false
	} else {
		var r struct {
			Stored bool `json:"stored"`
			Dup    bool `json:"dup"`
		}
		if uerr := json.Unmarshal(res, &r); uerr != nil {
			fmt.Printf("       notify.post retry result %q: %v\n", res, uerr)
			ok = false
		} else if r.Stored || !r.Dup {
			fmt.Printf("       notify.post retry result %q: want stored false, dup true\n", res)
			ok = false
		}
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
	if res, err := c.Call("clip.push", map[string]any{"text": "smoke clip"}, timeout); err != nil {
		fmt.Printf("       clip.push: %v\n", err)
		ok = false
	} else {
		var r struct {
			Pasted bool `json:"pasted"`
		}
		if uerr := json.Unmarshal(res, &r); uerr != nil {
			fmt.Printf("       clip.push result %q: %v\n", res, uerr)
			ok = false
		} else if r.Pasted {
			fmt.Printf("       clip.push result %q: want pasted false\n", res)
			ok = false
		}
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
		res, err = c.Call("text.send",
			map[string]any{"text": "typed from smoke", "target": "editor"}, timeout)
		if err == nil {
			var r struct {
				Typed int `json:"typed"`
			}
			if uerr := json.Unmarshal(res, &r); uerr != nil {
				lastErr = uerr
			} else {
				typed, lastErr = r.Typed, nil
				break
			}
		} else {
			lastErr = err
		}
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
	if res, err := c.Call("clip.push", map[string]any{"text": "pasted"}, timeout); err != nil {
		fmt.Printf("       clip.push pasted: %v\n", err)
		ok = false
	} else {
		var r struct {
			Pasted bool `json:"pasted"`
		}
		if uerr := json.Unmarshal(res, &r); uerr != nil {
			fmt.Printf("       clip.push result %q: %v\n", res, uerr)
			ok = false
		} else if !r.Pasted {
			fmt.Printf("       clip.push result %q: want pasted true\n", res)
			ok = false
		}
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
	if res, err := c.Call("screen.capture", nil, timeout); err != nil {
		fmt.Printf("       screen.capture: %v\n", err)
		ok = false
	} else {
		var r struct {
			Job string `json:"job"`
		}
		if uerr := json.Unmarshal(res, &r); uerr != nil {
			fmt.Printf("       screen.capture result %q: %v\n", res, uerr)
			ok = false
		} else if r.Job == "" {
			fmt.Printf("       screen.capture result %q: no job\n", res)
			ok = false
		}
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
	if res, err := c.Call("ota.status", nil, timeout); err != nil {
		fmt.Printf("       ota.status: %v\n", err)
		ok = false
	} else {
		var st struct {
			Running string `json:"running"`
			Next    string `json:"next"`
			Pending bool   `json:"pending"`
			Boots   int    `json:"boots"`
			MaxSize int    `json:"max_size"`
		}
		if uerr := json.Unmarshal(res, &st); uerr != nil {
			fmt.Printf("       ota.status result %q: %v\n", res, uerr)
			ok = false
		} else if st.Running == "" || st.MaxSize <= 0 {
			fmt.Printf("       ota.status: running %q, max_size %d\n", st.Running, st.MaxSize)
			ok = false
		}
	}
	pass = report(ok, "ota.status") && pass

	// 11. A sha256 that is not 64 hex digits is refused up front.
	pass = report(rpcErrCode(c, timeout, "ota.apply",
		map[string]any{"vol": "int", "path": "/ota-smoke.bin", "size": 1, "sha256": "xyz"},
		"invalid_argument"),
		"ota.apply bad sha256 -> invalid_argument") && pass

	// 12. A well-formed image verifies, writes and installs. The emulator ends
	//     with phase "installed" instead of "reboot".
	img := make([]byte, 200000)
	img[0] = 0xE9
	for i := 1; i < len(img); i++ {
		img[i] = byte(i * 7)
	}
	ok = true
	if err := c.WriteFile("int", "/ota-smoke.bin", img, true, 30*time.Second); err != nil {
		fmt.Printf("       fs.write /ota-smoke.bin: %v\n", err)
		ok = false
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

	return pass
}

// otaJob starts one ota.apply and follows its evt.ota stream, ignoring events
// for other jobs, until the job reaches a terminal phase ("installed" or
// "error"). It returns every phase seen for the job, the last phase, that
// phase's msg, and the first transport or decoding error.
func otaJob(c *pagerlink.Client, timeout time.Duration, params any) (phases []string, last, msg string, err error) {
	res, err := c.Call("ota.apply", params, timeout)
	if err != nil {
		return nil, "", "", err
	}
	var started struct {
		Job string `json:"job"`
	}
	if err := json.Unmarshal(res, &started); err != nil {
		return nil, "", "", fmt.Errorf("ota.apply result %q: %w", res, err)
	}
	if started.Job == "" {
		return nil, "", "", fmt.Errorf("ota.apply result %q: no job", res)
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
		n, ok := waitEvent(c, "evt.ota", remaining)
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
