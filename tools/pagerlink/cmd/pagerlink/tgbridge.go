// Telegram via the phone (HUB_REMOVAL_PLAN.md H2.6, design in §2.2): an
// in-memory stub of the user's external tg-bridge for `pagerlink serve`, so
// ui_telegram.cpp's phone path (tg.chats / tg.messages / tg.read / tg.send) can
// be exercised end to end on the emulator with no network.
//
// The four typed handlers answer from a small seeded chat store. As with the
// real bridge, the bearer the pager sends travels only with the one call: this
// file never logs it and the bridge struct never keeps it.
//
// Replies are trimmed to the fields ui_telegram.cpp parses — id/title/unread
// for chats and id/text/from/out/media{type,w,h} for messages. A trimmed body
// that would not fit the 4096 B RES payload is streamed as DATA under the xfer
// the pager offered (PROTOCOL.md §4), the same mechanism fs.read uses; a body
// that big with no xfer is refused.
//
// Bridge state lives on the mockPhone (its tg field, seeded by newMockPhone).
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

// tgMedia is the optional attachment descriptor on a message. The pager only
// renders its type and pixel size today.
type tgMedia struct {
	Type string `json:"type"`
	W    int    `json:"w"`
	H    int    `json:"h"`
}

// tgMsg is one message, and also the trimmed record tg.messages returns. Media
// is nil for a plain text message and then omitted from the wire entirely.
type tgMsg struct {
	ID    int      `json:"id"`
	Text  string   `json:"text"`
	From  string   `json:"from"`
	Out   bool     `json:"out"`
	Media *tgMedia `json:"media,omitempty"`
}

// tgChat is one chat. Msgs are oldest first, matching the bridge's store; the
// wire reply reverses them to newest first.
type tgChat struct {
	ID       int64
	Title    string
	Unread   int
	Msgs     []tgMsg
	LastRead int
}

// newest returns the chat's newest `limit` messages, newest first: the slice
// tg.messages answers with.
func (c *tgChat) newest(limit int) []tgMsg {
	n := min(max(limit, 0), len(c.Msgs))
	out := make([]tgMsg, 0, n)
	for i := len(c.Msgs) - 1; i >= len(c.Msgs)-n; i-- {
		out = append(out, c.Msgs[i])
	}
	return out
}

// tgBridge is the per-phone stub store. bulk counts replies that were streamed
// as DATA instead of answered inline, for the smoke test to observe.
type tgBridge struct {
	mu     sync.Mutex
	chats  []*tgChat
	nextID int
	bulk   int // replies sent as DATA
}

// newTgBridge seeds the stub: Alice (2 unread, a text / photo / text history),
// Team (25 long messages, every third outgoing), and Carol (1 unread).
func newTgBridge() *tgBridge {
	b := &tgBridge{nextID: 100}
	b.chats = []*tgChat{
		{
			ID:     101,
			Title:  "Alice",
			Unread: 2,
			Msgs: []tgMsg{
				{ID: 1, Text: "hi", From: "Alice"},
				{ID: 2, From: "Alice", Media: &tgMedia{Type: "photo", W: 800, H: 600}},
				{ID: 3, Text: "are you there?", From: "Alice"},
			},
		},
		{
			ID:    -100200,
			Title: "Team",
		},
		{
			ID:     303,
			Title:  "Carol",
			Unread: 1,
			Msgs:   []tgMsg{{ID: 40, Text: "yo", From: "Carol"}},
		},
	}
	team := b.chats[1]
	for id := 10; id <= 34; id++ {
		team.Msgs = append(team.Msgs, tgMsg{
			ID:   id,
			Text: fmt.Sprintf("msg %d: ", id) + strings.Repeat("lorem ipsum dolor sit amet ", 15),
			From: "Bob",
			Out:  id%3 == 0,
		})
	}
	return b
}

// tgParams decodes a tg.* request. Numbers are kept as json.Number so integer
// fields can insist on integer syntax, and the bridge fields are validated up
// front: with neither bridge nor token the stub answers as the phone's own
// Telegram login (the real phone's native session); one without the other is
// invalid_argument, and the literal token "bad" stands in for the bridge
// answering 401. The token is only ever compared here — never logged, never
// stored.
func tgParams(params json.RawMessage) (map[string]any, *pagerlink.RPCError) {
	dec := json.NewDecoder(bytes.NewReader(params))
	dec.UseNumber()
	var m map[string]any
	if err := dec.Decode(&m); err != nil {
		return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "bad params"}
	}
	_, hasBridge := m["bridge"]
	_, hasToken := m["token"]
	if !hasBridge && !hasToken {
		return m, nil
	}
	bridge, _ := m["bridge"].(string)
	token, _ := m["token"].(string)
	if bridge == "" || token == "" {
		return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "bridge and token are required"}
	}
	if token == "bad" {
		return nil, &pagerlink.RPCError{Code: "unauthorized", Msg: "bridge said 401"}
	}
	return m, nil
}

// tgInt reads m[key] as an integer. The value must be a json.Number that
// strconv.ParseInt accepts in base 10 — so 7 and (from a JSON number) 7 pass,
// while the string "7", the float 7.5 and the bool true are rejected. The
// error names the field.
func tgInt(m map[string]any, key string) (int64, *pagerlink.RPCError) {
	bad := &pagerlink.RPCError{Code: "invalid_argument", Msg: key + " must be an integer"}
	n, ok := m[key].(json.Number)
	if !ok {
		return 0, bad
	}
	v, err := strconv.ParseInt(n.String(), 10, 64)
	if err != nil {
		return 0, bad
	}
	return v, nil
}

// tgXfer reads the optional bulk-reply transfer id. Absent or 0 means the pager
// offered no receiver; any other value must be an odd id in 1..65535.
func tgXfer(m map[string]any) (uint16, *pagerlink.RPCError) {
	if _, ok := m["xfer"]; !ok {
		return 0, nil
	}
	v, perr := tgInt(m, "xfer")
	if perr != nil {
		return 0, perr
	}
	if v == 0 {
		return 0, nil
	}
	if v < 1 || v > 65535 || v%2 == 0 {
		return 0, &pagerlink.RPCError{Code: "invalid_argument", Msg: "xfer must be an odd 1..65535"}
	}
	return uint16(v), nil
}

// tgLimitXfer reads the paged-reply fields tg.chats and tg.messages share: a
// required limit in 1..100 and the optional bulk-reply xfer.
func tgLimitXfer(m map[string]any) (int, uint16, *pagerlink.RPCError) {
	limit, perr := tgInt(m, "limit")
	if perr != nil {
		return 0, 0, perr
	}
	if limit < 1 || limit > 100 {
		return 0, 0, &pagerlink.RPCError{Code: "invalid_argument", Msg: "limit must be 1..100"}
	}
	xfer, perr := tgXfer(m)
	if perr != nil {
		return 0, 0, perr
	}
	return int(limit), xfer, nil
}

// find returns the chat with id. The caller must hold b.mu. A missing chat is
// the bridge's 404, surfaced as http_error with the decimal status as its msg.
func (b *tgBridge) find(id int64) (*tgChat, *pagerlink.RPCError) {
	for _, c := range b.chats {
		if c.ID == id {
			return c, nil
		}
	}
	return nil, &pagerlink.RPCError{Code: "http_error", Msg: "404"}
}

// tgChatOut is the trimmed chat record tg.chats returns.
type tgChatOut struct {
	ID     int64  `json:"id"`
	Title  string `json:"title"`
	Unread int    `json:"unread"`
}

// tgChats answers tg.chats: the first `limit` chats as a JSON array of
// {id,title,unread}.
func (s *mockPhone) tgChats(params json.RawMessage) (any, *pagerlink.RPCError) {
	m, perr := tgParams(params)
	if perr != nil {
		return nil, perr
	}
	limit, xfer, perr := tgLimitXfer(m)
	if perr != nil {
		return nil, perr
	}

	b := s.tg
	b.mu.Lock()
	n := min(limit, len(b.chats))
	out := make([]tgChatOut, 0, n)
	for _, c := range b.chats[:n] {
		out = append(out, tgChatOut{ID: c.ID, Title: c.Title, Unread: c.Unread})
	}
	body, err := json.Marshal(out)
	b.mu.Unlock()
	if err != nil {
		return nil, &pagerlink.RPCError{Code: "internal", Msg: err.Error()}
	}
	return s.tgReply(body, xfer)
}

// tgMessages answers tg.messages: the chat's newest `limit` messages, newest
// first, each {id,text,from,out} plus media only when the message has one.
func (s *mockPhone) tgMessages(params json.RawMessage) (any, *pagerlink.RPCError) {
	m, perr := tgParams(params)
	if perr != nil {
		return nil, perr
	}
	limit, xfer, perr := tgLimitXfer(m)
	if perr != nil {
		return nil, perr
	}
	chatID, perr := tgInt(m, "chat_id")
	if perr != nil {
		return nil, perr
	}

	b := s.tg
	b.mu.Lock()
	chat, perr := b.find(chatID)
	if perr != nil {
		b.mu.Unlock()
		return nil, perr
	}
	body, err := json.Marshal(chat.newest(limit))
	b.mu.Unlock()
	if err != nil {
		return nil, &pagerlink.RPCError{Code: "internal", Msg: err.Error()}
	}
	return s.tgReply(body, xfer)
}

// tgRead answers tg.read: mark the chat read up to up_to and reply {}.
func (s *mockPhone) tgRead(params json.RawMessage) (any, *pagerlink.RPCError) {
	m, perr := tgParams(params)
	if perr != nil {
		return nil, perr
	}
	chatID, perr := tgInt(m, "chat_id")
	if perr != nil {
		return nil, perr
	}
	upTo, perr := tgInt(m, "up_to")
	if perr != nil {
		return nil, perr
	}
	if _, perr := tgXfer(m); perr != nil {
		return nil, perr
	}

	b := s.tg
	b.mu.Lock()
	chat, perr := b.find(chatID)
	if perr != nil {
		b.mu.Unlock()
		return nil, perr
	}
	chat.Unread = 0
	chat.LastRead = int(upTo)
	b.mu.Unlock()
	return map[string]any{}, nil
}

// tgSend answers tg.send: append the outgoing text and reply {}.
func (s *mockPhone) tgSend(params json.RawMessage) (any, *pagerlink.RPCError) {
	m, perr := tgParams(params)
	if perr != nil {
		return nil, perr
	}
	chatID, perr := tgInt(m, "chat_id")
	if perr != nil {
		return nil, perr
	}
	text, _ := m["text"].(string)
	if text == "" {
		return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "text must be a non-empty string"}
	}
	if _, perr := tgXfer(m); perr != nil {
		return nil, perr
	}

	b := s.tg
	b.mu.Lock()
	chat, perr := b.find(chatID)
	if perr != nil {
		b.mu.Unlock()
		return nil, perr
	}
	chat.Msgs = append(chat.Msgs, tgMsg{ID: b.nextID, Text: text, From: "me", Out: true})
	b.nextID++
	b.mu.Unlock()
	return map[string]any{}, nil
}

// tgReply wraps a trimmed tg.chats / tg.messages body in the inline
// {"body":…} RES when it fits a 4096 B payload. Otherwise it streams the body
// as DATA under the pager's receiver xfer and answers {xfer,size,crc32}; a body
// that big with no xfer is refused. The body (and so the bridge JSON) carries
// no token.
func (s *mockPhone) tgReply(body []byte, xfer uint16) (any, *pagerlink.RPCError) {
	if len(body)+len(`{"body":}`) <= 4096 {
		return map[string]json.RawMessage{"body": body}, nil
	}
	if xfer == 0 {
		return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "reply over 4096 B needs xfer"}
	}
	b := s.tg
	b.mu.Lock()
	b.bulk++
	b.mu.Unlock()
	go func() {
		if err := s.c.SendBytes(xfer, body, 15*time.Second); err != nil {
			fmt.Printf("serve: tg bulk xfer=%d: %v\n", xfer, err)
		}
	}()
	return map[string]any{"xfer": xfer, "size": len(body), "crc32": crc32.ChecksumIEEE(body)}, nil
}

// tgAddIncoming appends an inbound message from Carol and bumps the chat's
// unread count — the smoke test's way of simulating a new arrival.
func (s *mockPhone) tgAddIncoming(chatID int64, text string) {
	b := s.tg
	b.mu.Lock()
	defer b.mu.Unlock()
	chat, _ := b.find(chatID)
	if chat == nil {
		return
	}
	chat.Msgs = append(chat.Msgs, tgMsg{ID: b.nextID, Text: text, From: "Carol", Out: false})
	b.nextID++
	chat.Unread++
}

// tgChatSnapshot returns a copy of the chat (its own Msgs slice), or the zero
// value when the chat is unknown — the smoke test's read-back of unread count,
// read marker and newest message.
func (s *mockPhone) tgChatSnapshot(chatID int64) tgChat {
	b := s.tg
	b.mu.Lock()
	defer b.mu.Unlock()
	chat, _ := b.find(chatID)
	if chat == nil {
		return tgChat{}
	}
	snap := *chat
	snap.Msgs = append([]tgMsg(nil), chat.Msgs...)
	return snap
}

// lastMsg returns the chat's newest message, or the zero value when empty.
func (c tgChat) lastMsg() tgMsg {
	if len(c.Msgs) == 0 {
		return tgMsg{}
	}
	return c.Msgs[len(c.Msgs)-1]
}

// tgBulk returns how many replies were streamed as DATA rather than inline.
func (s *mockPhone) tgBulk() int {
	b := s.tg
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.bulk
}

// tgExpect reports what tg.messages with `limit` would return for the chat:
// the message count, the total text bytes, and the newest id. It reuses the
// handler's newest-first trimming without touching the wire.
func (s *mockPhone) tgExpect(chatID int64, limit int) (count int, textBytes int, newestID int) {
	snap := s.tgChatSnapshot(chatID)
	msgs := snap.newest(limit)
	for _, m := range msgs {
		textBytes += len(m.Text)
		newestID = max(newestID, m.ID)
	}
	return len(msgs), textBytes, newestID
}
