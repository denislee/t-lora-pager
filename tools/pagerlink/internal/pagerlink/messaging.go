package pagerlink

// Client-side statement of the PROTOCOL.md §6.5 messaging rules and the §6.6
// rgb565-rle decoder. The pager (src/hal/companion_msg.cpp, src/hal/rle565.cpp)
// enforces the same rules; protocol/vectors/messaging.json and screen.json pin
// both, so a phone that checks here first never sends what the pager refuses.

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"unicode/utf8"
)

// Messaging field limits in bytes (PROTOCOL.md §6.5).
const (
	MsgTitleMax    = 64
	MsgBodyMax     = 512
	MsgIDMax       = 32
	MsgSrcMax      = 32
	MsgClipMax     = 4000
	MsgTextSendMax = 2000
)

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
		if (c < 0x20 || c == 0x7f) && !containsRune(r.ctl, c) {
			return r.key + ": control character"
		}
	}
	return ""
}

func containsRune(s string, c rune) bool {
	for _, x := range s {
		if x == c {
			return true
		}
	}
	return false
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

// DecodeRLE565 decodes an rgb565-rle stream for a w x h image.
func DecodeRLE565(data []byte, w, h int) ([]uint16, error) {
	if len(data)%4 != 0 {
		return nil, fmt.Errorf("length not a multiple of 4")
	}
	total := w * h
	out := make([]uint16, 0, total)
	for i := 0; i < len(data); i += 4 {
		count := int(binary.LittleEndian.Uint16(data[i:]))
		px := binary.LittleEndian.Uint16(data[i+2:])
		if count == 0 {
			return nil, fmt.Errorf("zero-length run")
		}
		if len(out)+count > total {
			return nil, fmt.Errorf("more pixels than w*h")
		}
		for k := 0; k < count; k++ {
			out = append(out, px)
		}
	}
	if len(out) != total {
		return nil, fmt.Errorf("fewer pixels than w*h")
	}
	return out, nil
}
