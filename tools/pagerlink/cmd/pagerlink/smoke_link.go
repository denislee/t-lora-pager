package main

// The pager-initiated half of the smoke test: `pagerlink smoke` asks the pager
// to run the emulator-only test.callback hook (src/hal/link/handlers_test.cpp),
// which drives hw_link_call()/hw_link_send_bytes() back at us, and checks the
// evt.test.result the hook emits. The hook is compiled only for the emulator,
// so these steps pass against the emulator alone.

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

// testResult is the evt.test.result payload. The chat steps fill step/ok/code/
// msg/reply/transcript/bytes; the call steps fill step/ok/code/msg/json; the sink
// step fills step/ok/code/bytes/crc32; the notes steps fill step/ok/code/msg/
// uploaded/already/errors/seeded; the tg steps fill step/op/ok/status/uplink/
// count/unread_total/newest_id/marked_to/text_bytes/notified; the weather step
// fills step/ok/msg/city/hourly_count/daily_count.
type testResult struct {
	Step        string            `json:"step"`
	OK          bool              `json:"ok"`
	Code        string            `json:"code"`
	Msg         string            `json:"msg"`
	JSON        json.RawMessage   `json:"json"`
	Bytes       int               `json:"bytes"`
	CRC32       uint32            `json:"crc32"`
	Reply       string            `json:"reply"`
	Transcript  string            `json:"transcript"`
	Uploaded    int               `json:"uploaded"`
	Already     int               `json:"already"`
	Errors      int               `json:"errors"`
	Seeded      map[string]string `json:"seeded"`
	Op          string            `json:"op"`
	Status      string            `json:"status"`
	Uplink      string            `json:"uplink"`
	Count       int               `json:"count"`
	UnreadTotal int               `json:"unread_total"`
	NewestID    int               `json:"newest_id"`
	MarkedTo    int               `json:"marked_to"`
	TextBytes   int               `json:"text_bytes"`
	Notified    int               `json:"notified"`
	City        string            `json:"city"`
	HourlyCount int               `json:"hourly_count"`
	DailyCount  int               `json:"daily_count"`
}

// sinkResult is what the test.sink.begin receiver goroutine reports: how many
// bytes arrived and whether the transfer (including its CRC) succeeded.
type sinkResult struct {
	n   int
	err error
}

// smokeLink drives the seventeen pager-initiated steps and reports each. c is
// the live client returned by smokeFS.
func smokeLink(c *pagerlink.Client, timeout time.Duration) bool {
	pass := true
	sinkCh := make(chan sinkResult, 1)

	// The phone-side handlers the pager's steps call.
	c.Handle("sys.ping", func(params json.RawMessage) (any, *pagerlink.RPCError) {
		return params, nil
	})
	c.Handle("test.slow", func(json.RawMessage) (any, *pagerlink.RPCError) {
		time.Sleep(2 * time.Second)
		return nil, nil
	})
	c.Handle("test.sink.begin", func(params json.RawMessage) (any, *pagerlink.RPCError) {
		var p struct {
			Size  uint32 `json:"size"`
			CRC32 uint32 `json:"crc32"`
		}
		if err := json.Unmarshal(params, &p); err != nil {
			return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: err.Error()}
		}
		id, wait := c.AcceptBytes(p.Size, p.CRC32)
		go func() {
			data, err := wait(30 * time.Second)
			sinkCh <- sinkResult{n: len(data), err: err}
		}()
		return map[string]any{"xfer": id}, nil
	})

	// The phone-side chat handlers (same code the `serve` command uses).
	phone := &mockPhone{c: c, audio: make(map[uint16]*audioXfer)}
	c.Handle("chat.audio.begin", phone.chatAudioBegin)
	c.Handle("chat.ask", phone.chatAsk)
	c.Handle("notes.sync", phone.notesSync)
	c.Handle("tg.chats", phone.tgChats)
	c.Handle("tg.messages", phone.tgMessages)
	c.Handle("tg.read", phone.tgRead)
	c.Handle("tg.send", phone.tgSend)

	// a. A pager-initiated RPC end to end.
	r, ok := linkStep(c, timeout, map[string]any{
		"method": "sys.ping", "params": map[string]any{"t": 7},
	})
	var echo struct {
		T int `json:"t"`
	}
	if ok && (r.Step != "call" || !r.OK || json.Unmarshal(r.JSON, &echo) != nil || echo.T != 7) {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone call (hw_link_call) sys.ping") && pass

	// b. An unknown method comes back as ERR unsupported.
	r, ok = linkStep(c, timeout, map[string]any{"method": "nope.unknown"})
	if ok && (r.Step != "call" || r.OK || r.Code != "unsupported") {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone unknown method -> unsupported") && pass

	// c. A handler slower than the call's timeout fails with "timeout".
	r, ok = linkStep(c, timeout, map[string]any{"method": "test.slow", "timeout_ms": 500})
	if ok && (r.Step != "call" || r.OK || r.Code != "timeout") {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone call timeout") && pass

	// d. The pager streams 200000 generated bytes into test.sink.begin; we must
	//    receive them all with a matching CRC and the pager must report ok.
	r, ok = linkStep(c, timeout, map[string]any{"sink": "test.sink.begin", "bytes": 200000})
	if ok && (r.Step != "sink" || !r.OK) {
		ok = false
	}
	if ok {
		select {
		case s := <-sinkCh:
			if s.err != nil || s.n != 200000 {
				ok = false
				fmt.Printf("       sink: %d bytes, err=%v\n", s.n, s.err)
			}
		case <-time.After(15 * time.Second):
			ok = false
			fmt.Printf("       sink: no result within 15s\n")
		}
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone 200 KB DATA (hw_link_send_bytes)") && pass

	// e. The pager asks chat.ask with text; the mock phone echoes it.
	r, ok = linkStep(c, timeout, map[string]any{"chat": "text", "text": "hello pager"})
	if ok && (r.Step != "chat" || !r.OK || r.Reply != "(mock) hello pager") {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone chat.ask text (chat_link_ask)") && pass

	// f. The pager streams a 64 KB voice note into chat.audio.begin then asks
	//    chat.ask with the returned xfer; the mock phone must have all the bytes.
	const voicePCM = 65536
	r, ok = linkStep(c, timeout, map[string]any{"chat": "voice", "bytes": voicePCM})
	if ok && (r.Step != "chat" || !r.OK || r.Bytes != voicePCM+44 ||
		r.Reply != fmt.Sprintf("(mock) audio %d bytes", voicePCM+44) || r.Transcript == "") {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone chat voice 64 KB (chat.audio.begin + DATA + chat.ask)") && pass

	// g. The pager syncs three internal notes; we must fetch each one and see
	//    the raw bytes (Salted__ ciphertext) byte-for-byte.
	pass = report(notesStep(c, timeout, phone, "github", map[string]string{
		"a-plain.txt": "int/a-plain.txt", "b-shared.txt": "int/b-shared.txt", "c-secret.txt": "int/c-secret.txt",
	}, 3), "pager->phone notes.sync github: 3 internal notes, raw bytes (Salted__ ciphertext) byte-for-byte") && pass

	// h. With the SD volume present the manifest gains d-sdonly.txt; the
	//    internal copy of the shared name still wins the duplicate.
	pass = report(notesStep(c, timeout, phone, "phone", map[string]string{
		"a-plain.txt": "int/a-plain.txt", "b-shared.txt": "int/b-shared.txt", "c-secret.txt": "int/c-secret.txt", "d-sdonly.txt": "sd/d-sdonly.txt",
	}, 4), "pager->phone notes.sync phone: internal + SD, internal wins the duplicate") && pass

	// i. - q. The Telegram via the phone steps (HUB_REMOVAL_PLAN.md H2.6).
	pass = tgSteps(c, timeout, phone) && pass

	// r. - s. weather.push + cache read-back (IOS_COMPANION_PLAN.md Track C 9).
	pass = weatherPushSteps(c, timeout) && pass

	// t. - u. weather.get via the pager's phone step (HUB_REMOVAL_PLAN.md H2.3).
	pass = weatherGetSteps(c, timeout) && pass

	return pass
}

// tgSteps runs the Telegram-via-the-phone steps (HUB_REMOVAL_PLAN.md H2.6): the
// pager runs ui_telegram's worker code through test.callback {tg}; the mock
// phone's stub bridge answers. Each step is reported like the others.
func tgSteps(c *pagerlink.Client, timeout time.Duration, phone *mockPhone) bool {
	pass := true
	const bridge = "http://stub-bridge.invalid"
	const token = "tok"

	// tg runs one test.callback {tg:…} step, filling in the stub bridge and
	// token the pager forwards to the phone. ok is false on a transport failure
	// or when the step was not the tg hook.
	tg := func(params map[string]any) (testResult, bool) {
		params["bridge"] = bridge
		if _, has := params["token"]; !has {
			params["token"] = token
		}
		r, ok := linkStep(c, timeout, params)
		if !ok || r.Step != "tg" {
			return r, false
		}
		return r, true
	}

	// i. The pager lists the phone's chats through the stub bridge.
	r, ok := tg(map[string]any{"tg": "chats"})
	if ok && !(r.OK && r.Status == "via phone" && r.Uplink == "phone" && r.Count == 3 && r.UnreadTotal == 3) {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone tg.chats: 3 chats, 3 unread, status \"via phone\"") && pass

	// j. A bad token makes the bridge answer 401.
	r, ok = tg(map[string]any{"tg": "chats", "token": "bad"})
	if ok && !(!r.OK && r.Status == "HTTP 401") {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone tg.chats bad token -> unauthorized -> \"HTTP 401\"") && pass

	// k. A non-integer chat_id is rejected before it reaches the bridge.
	r, ok = linkStep(c, timeout, map[string]any{
		"method": "tg.messages",
		"params": map[string]any{"bridge": bridge, "token": token, "chat_id": "-100200", "limit": 20},
	})
	if ok && !(r.Step == "call" && !r.OK && r.Code == "invalid_argument") {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone tg.messages non-integer chat_id -> invalid_argument") && pass

	// l. A reply over 4096 B streams as DATA; all 20 messages parse and the chat
	//    is marked read up to the newest id.
	bulkBefore := phone.tgBulk()
	wantN, wantBytes, wantNewest := phone.tgExpect(-100200, 20)
	r, ok = tg(map[string]any{"tg": "msgs", "chat_id": -100200})
	if ok && !(r.OK && wantN == 20 && wantBytes > 4096 && r.Count == wantN && r.TextBytes == wantBytes &&
		r.NewestID == wantNewest && r.MarkedTo == wantNewest && phone.tgBulk() == bulkBefore+1 &&
		phone.tgLastRead(-100200) == wantNewest) {
		ok = false
	}
	if !ok {
		fmt.Printf("       want N=%d bytes=%d newest=%d\n", wantN, wantBytes, wantNewest)
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone tg.messages bulk >4096 B (DATA to the pager): all 20 messages parsed, mark-read") && pass

	// m. Sending appends an outgoing message to the chat.
	r, ok = tg(map[string]any{"tg": "send", "chat_id": 101, "text": "hi from pager"})
	last := phone.tgLastMsg(101)
	if ok && !(r.OK && r.Status == "Sent" && last.Text == "hi from pager" && last.Out) {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone tg.send appends to the chat") && pass

	// n. Opening a chat inline carries the messages and the read marker zeroes
	//    the unread count.
	wantN, _, wantNewest = phone.tgExpect(101, 20)
	r, ok = tg(map[string]any{"tg": "msgs", "chat_id": 101})
	if ok && !(r.OK && r.Status == "via phone" && r.Count == wantN && r.MarkedTo == wantNewest &&
		phone.tgUnread(101) == 0 && phone.tgLastRead(101) == wantNewest) {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "pager->phone open chat inline + tg.read zeroes unread") && pass

	// o. Regression test (H2.6): with Wi-Fi down and the link up, exactly one new
	//    message fires the notifier once, and only once.
	r, ok = tg(map[string]any{"tg": "notify", "reset": true})
	if ok && !(r.OK && r.Notified == 0 && r.Uplink == "phone") {
		ok = false
	}
	if ok {
		phone.tgAddIncoming(303, "ping 1")
		r, ok = tg(map[string]any{"tg": "notify"})
		if ok && !(r.OK && r.Notified == 1) {
			ok = false
		}
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "notifier, Wi-Fi down + link up: a new message fires exactly once") && pass

	// p. With no new message the notifier stays silent, deduping the earlier one.
	r, ok = tg(map[string]any{"tg": "notify"})
	if ok && !(r.OK && r.Notified == 0) {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "notifier: no new message -> silent (dedup)") && pass

	// q. With the link down too, a new message stays quiet and the status is
	//    "Offline".
	phone.tgAddIncoming(303, "ping 2")
	r, ok = tg(map[string]any{"tg": "notify", "link_down": true})
	if ok && !(!r.OK && r.Notified == 0 && r.Status == "Offline" && r.Uplink == "none") {
		ok = false
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	pass = report(ok, "notifier, link down too: silent, status \"Offline\"") && pass

	return pass
}

// linkStep runs one test.callback step and returns the evt.test.result the
// pager emits, waiting up to 15 s for it. ok is false when the call itself
// fails or no result arrives.
func linkStep(c *pagerlink.Client, timeout time.Duration, params any) (testResult, bool) {
	if _, err := c.Call("test.callback", params, timeout); err != nil {
		fmt.Printf("       test.callback: %v\n", err)
		return testResult{}, false
	}
	n, ok := waitResult(c, 15*time.Second)
	if !ok {
		fmt.Printf("       no evt.test.result within 15s\n")
		return testResult{}, false
	}
	var r testResult
	if err := json.Unmarshal(n.Params, &r); err != nil {
		fmt.Printf("       evt.test.result params %q: %v\n", n.Params, err)
		return testResult{}, false
	}
	return r, true
}

// waitResult drains c.Events() until an evt.test.result arrives, ignoring every
// other event, or within elapses.
func waitResult(c *pagerlink.Client, within time.Duration) (pagerlink.Notification, bool) {
	deadline := time.After(within)
	for {
		select {
		case n := <-c.Events():
			if n.Name == "evt.test.result" {
				return n, true
			}
		case <-deadline:
			return pagerlink.Notification{}, false
		}
	}
}

// notesStep runs one notes.sync step (mode picks the pager's fixture set), then
// checks each fetched file in a fresh notes-out directory against the hex bytes
// the pager reported for its seeded label in evt.test.result. wantUploaded is
// the expected number of uploaded files and want maps each received file name
// to the seeded label whose bytes it must equal. The c-secret.txt fixture must
// arrive as Salted__ ciphertext.
func notesStep(c *pagerlink.Client, timeout time.Duration, phone *mockPhone, mode string, want map[string]string, wantUploaded int) bool {
	dir, err := os.MkdirTemp("", "pagerlink-notes-"+mode+"-")
	if err != nil {
		fmt.Printf("       notes: MkdirTemp: %v\n", err)
		return false
	}
	defer os.RemoveAll(dir)

	phone.mu.Lock()
	phone.notesOut = dir
	phone.mu.Unlock()

	r, ok := linkStep(c, timeout, map[string]any{"notes": mode})
	if ok && (r.Step != "notes" || !r.OK || r.Uploaded != wantUploaded || r.Errors != 0) {
		ok = false
	}
	if ok {
		for name, label := range want {
			enc, found := r.Seeded[label]
			if !found {
				fmt.Printf("       notes %s: no seeded bytes for %s\n", name, label)
				ok = false
				break
			}
			expected, derr := hex.DecodeString(enc)
			if derr != nil {
				fmt.Printf("       notes %s: bad seeded hex %q: %v\n", name, enc, derr)
				ok = false
				break
			}
			if name == "c-secret.txt" && !bytes.HasPrefix(expected, []byte("Salted__")) {
				fmt.Printf("       notes %s: expected Salted__ ciphertext\n", name)
				ok = false
				break
			}
			got, rerr := os.ReadFile(filepath.Join(dir, name))
			if rerr != nil {
				fmt.Printf("       notes %s: %v\n", name, rerr)
				ok = false
				break
			}
			if !bytes.Equal(got, expected) {
				fmt.Printf("       notes %s: got %d bytes, want %d (%s)\n", name, len(got), len(expected), label)
				ok = false
				break
			}
		}
	}
	if ok {
		entries, derr := os.ReadDir(dir)
		if derr != nil {
			fmt.Printf("       notes: ReadDir: %v\n", derr)
			ok = false
		} else if len(entries) != len(want) {
			fmt.Printf("       notes: %d files in dir, want %d\n", len(entries), len(want))
			ok = false
		}
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
	}
	return ok
}
