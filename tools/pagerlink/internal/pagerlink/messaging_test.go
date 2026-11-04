package pagerlink

// Golden-vector tests for the client-side messaging rules (PROTOCOL.md §6.5)
// and the rgb565-rle codec (§6.6). Vector files are produced by
// protocol/tools/mkvectors.py and are the oracle, so the expected values come
// straight from them.

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"reflect"
	"strings"
	"testing"
	"unicode/utf8"
)

type messagingVectorError struct {
	Code string `json:"code"`
	Msg  string `json:"msg"`
}

type messagingVectorCase struct {
	Name       string                `json:"name"`
	Method     string                `json:"method"`
	ParamsJSON string                `json:"params_json"`
	Ok         json.RawMessage       `json:"ok"`
	Error      *messagingVectorError `json:"error"`
}

// messaging.json: every valid case is accepted ("") and every error case
// returns exactly the recorded invalid_argument msg.
func TestMessagingVectors(t *testing.T) {
	var v struct {
		Cases []messagingVectorCase `json:"cases"`
	}
	loadVectors(t, "messaging.json", &v)
	if len(v.Cases) <= 20 {
		t.Fatalf("messaging.json has %d cases, want > 20", len(v.Cases))
	}

	for _, c := range v.Cases {
		got := CheckMessaging(c.Method, json.RawMessage(c.ParamsJSON))
		if c.Error != nil {
			if got != c.Error.Msg {
				t.Errorf("%s: CheckMessaging = %q, want %q", c.Name, got, c.Error.Msg)
			}
			continue
		}
		if got != "" {
			t.Errorf("%s: CheckMessaging = %q, want \"\"", c.Name, got)
		}
	}
}

// messaging.json limits: every Msg*Max constant equals the vector's byte limit,
// and the vector names no limit the constants do not cover.
func TestMessagingLimits(t *testing.T) {
	var v struct {
		Limits map[string]int `json:"limits"`
	}
	loadVectors(t, "messaging.json", &v)

	want := map[string]int{
		"title":     MsgTitleMax,
		"body":      MsgBodyMax,
		"id":        MsgIDMax,
		"src":       MsgSrcMax,
		"clip":      MsgClipMax,
		"text_send": MsgTextSendMax,
	}
	if !reflect.DeepEqual(v.Limits, want) {
		t.Errorf("messaging.json limits = %v, constants = %v", v.Limits, want)
	}
}

// screen.json: every image round-trips through EncodeRLE565/DecodeRLE565 and
// every malformed stream is rejected.
func TestScreenVectors(t *testing.T) {
	var v struct {
		Images []struct {
			Name      string `json:"name"`
			W         int    `json:"w"`
			H         int    `json:"h"`
			RLEHex    string `json:"rle_hex"`
			PixelsHex string `json:"pixels_hex"`
			Fill      *int   `json:"fill"`
		} `json:"images"`
		Malformed []struct {
			Name   string `json:"name"`
			W      int    `json:"w"`
			H      int    `json:"h"`
			RLEHex string `json:"rle_hex"`
		} `json:"malformed"`
	}
	loadVectors(t, "screen.json", &v)
	if len(v.Images) == 0 || len(v.Malformed) == 0 {
		t.Fatal("screen.json missing images/malformed cases")
	}

	for _, c := range v.Images {
		var px []uint16
		if c.Fill != nil {
			px = make([]uint16, c.W*c.H)
			for i := range px {
				px[i] = uint16(*c.Fill)
			}
		} else {
			raw := mustHex(t, c.PixelsHex)
			if len(raw)%2 != 0 {
				t.Errorf("%s: pixels_hex has odd length", c.Name)
				continue
			}
			px = make([]uint16, len(raw)/2)
			for i := range px {
				px[i] = binary.LittleEndian.Uint16(raw[i*2:])
			}
		}

		if got := hex.EncodeToString(EncodeRLE565(px)); got != c.RLEHex {
			t.Errorf("%s: EncodeRLE565 = %s, want %s", c.Name, got, c.RLEHex)
		}

		got, err := DecodeRLE565(mustHex(t, c.RLEHex), c.W, c.H)
		if err != nil {
			t.Errorf("%s: DecodeRLE565: %v", c.Name, err)
			continue
		}
		if !reflect.DeepEqual(got, px) {
			t.Errorf("%s: DecodeRLE565 = %d pixels, want %d", c.Name, len(got), len(px))
		}
	}

	for _, c := range v.Malformed {
		if _, err := DecodeRLE565(mustHex(t, c.RLEHex), c.W, c.H); err == nil {
			t.Errorf("%s: DecodeRLE565 accepted %s, want error", c.Name, c.RLEHex)
		}
	}
}

// textRule describes one string field.
type textRule struct {
	key      string
	required bool
	max      int
	ctl      string // control characters allowed
}

// checkText applies rule r to params; "" means the field is fine (or absent and
// optional).
func checkText(params map[string]json.RawMessage, r textRule) string {
	raw, ok := params[r.key]
	if !ok {
		if r.required {
			return r.key + ": required"
		}
		return ""
	}
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return r.key + ": expected string"
	}
	if r.required && s == "" {
		return r.key + ": empty"
	}
	if len(s) > r.max {
		return r.key + ": too long"
	}
	if !utf8.ValidString(s) {
		return r.key + ": not valid UTF-8"
	}
	for _, c := range s {
		if (c < 0x20 || c == 0x7f) && !strings.ContainsRune(r.ctl, c) {
			return r.key + ": control character"
		}
	}
	return ""
}

func idCharOK(c byte) bool {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		c == '.' || c == '_' || c == ':' || c == '-'
}

// CheckMessaging validates the params of notify.post, clip.push or text.send
// (rawParams: the JSON object) and returns the pager's invalid_argument msg, or
// "" when the pager accepts them. Unknown methods return "".
func CheckMessaging(method string, rawParams json.RawMessage) string {
	var p map[string]json.RawMessage
	if err := json.Unmarshal(rawParams, &p); err != nil {
		return "params: expected object"
	}
	switch method {
	case "notify.post":
		if m := checkText(p, textRule{"title", true, MsgTitleMax, ""}); m != "" {
			return m
		}
		if m := checkText(p, textRule{"body", false, MsgBodyMax, "\n"}); m != "" {
			return m
		}
		if raw, ok := p["id"]; ok {
			var id string
			if err := json.Unmarshal(raw, &id); err != nil {
				return "id: expected string"
			}
			if id == "" {
				return "id: empty"
			}
			if len(id) > MsgIDMax {
				return "id: too long"
			}
			for i := 0; i < len(id); i++ {
				if !idCharOK(id[i]) {
					return "id: bad character"
				}
			}
		}
		if m := checkText(p, textRule{"src", false, MsgSrcMax, ""}); m != "" {
			return m
		}
		if raw, ok := p["ts"]; ok {
			var n json.Number
			dec := json.NewDecoder(bytes.NewReader(raw))
			dec.UseNumber()
			// json.Number also accepts a quoted "1"; the pager wants a number.
			if len(raw) > 0 && raw[0] == '"' {
				return "ts: expected integer"
			}
			if err := dec.Decode(&n); err != nil {
				return "ts: expected integer"
			}
			v, err := n.Int64()
			if err != nil {
				// A fraction/exponent is not an integer; a too-large integer
				// still is one, just out of range.
				if _, ferr := n.Float64(); ferr == nil && isIntegerLiteral(string(n)) {
					return "ts: out of range"
				}
				return "ts: expected integer"
			}
			if v < 0 || v > 0xFFFFFFFF {
				return "ts: out of range"
			}
		}
		if raw, ok := p["prio"]; ok {
			var s string
			if json.Unmarshal(raw, &s) != nil || (s != "low" && s != "normal" && s != "high") {
				return "prio: not one of low, normal, high"
			}
		}
	case "clip.push":
		return checkText(p, textRule{"text", true, MsgClipMax, "\n\t"})
	case "text.send":
		if m := checkText(p, textRule{"text", true, MsgTextSendMax, "\n\t"}); m != "" {
			return m
		}
		raw, ok := p["target"]
		if !ok {
			return "target: required"
		}
		var s string
		if json.Unmarshal(raw, &s) != nil || (s != "editor" && s != "kb") {
			return "target: not one of editor, kb"
		}
	}
	return ""
}

func isIntegerLiteral(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !(c >= '0' && c <= '9') && !(i == 0 && c == '-') {
			return false
		}
	}
	return true
}

// EncodeRLE565 encodes RGB565 pixels as rgb565-rle (PROTOCOL.md §6.6).
func EncodeRLE565(px []uint16) []byte {
	var out []byte
	for i := 0; i < len(px); {
		v := px[i]
		run := 1
		for i+run < len(px) && px[i+run] == v && run < 0xFFFF {
			run++
		}
		out = binary.LittleEndian.AppendUint16(out, uint16(run))
		out = binary.LittleEndian.AppendUint16(out, v)
		i += run
	}
	return out
}
