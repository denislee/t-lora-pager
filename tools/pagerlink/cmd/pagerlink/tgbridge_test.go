package main

import (
	"encoding/json"
	"testing"
)

// tgParams: no bridge fields means the phone's own login; a half-configured
// bridge is invalid; the "bad" token stands in for a bridge 401.
func TestTgParamsBridgeOptional(t *testing.T) {
	cases := []struct {
		params string
		code   string // "" = accepted
	}{
		{`{"limit":20}`, ""},
		{`{"bridge":"https://b","token":"t","limit":20}`, ""},
		{`{"bridge":"https://b","limit":20}`, "invalid_argument"},
		{`{"token":"t","limit":20}`, "invalid_argument"},
		{`{"bridge":"https://b","token":"bad","limit":20}`, "unauthorized"},
	}
	for _, c := range cases {
		_, perr := tgParams(json.RawMessage(c.params))
		got := ""
		if perr != nil {
			got = perr.Code
		}
		if got != c.code {
			t.Errorf("tgParams(%s) = %q, want %q", c.params, got, c.code)
		}
	}
}

// tg.messages answers newest first, trimmed to limit, with media only on the
// messages that carry one (the exact bytes ui_telegram.cpp parses).
func TestTgMessagesWireShape(t *testing.T) {
	s := newMockPhone(nil)
	res, perr := s.tgMessages(json.RawMessage(`{"chat_id":101,"limit":2}`))
	if perr != nil {
		t.Fatalf("tgMessages: %v", perr)
	}
	got := string(res.(map[string]json.RawMessage)["body"])
	want := `[{"id":3,"text":"are you there?","from":"Alice","out":false},` +
		`{"id":2,"text":"","from":"Alice","out":false,"media":{"type":"photo","w":800,"h":600}}]`
	if got != want {
		t.Fatalf("body = %s\nwant   %s", got, want)
	}
	if n, b, newest := s.tgExpect(101, 2); n != 2 || b != len("are you there?") || newest != 3 {
		t.Fatalf("tgExpect = %d,%d,%d, want 2,%d,3", n, b, newest, len("are you there?"))
	}
}
