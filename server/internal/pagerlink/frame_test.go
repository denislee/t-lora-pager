package pagerlink

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// The golden vectors are produced by protocol/tools/mkvectors.py and are the
// oracle for every codec, so these tests compare against them directly rather
// than re-deriving expected bytes. The path is relative to the package dir,
// which is the test's working directory.
func loadVectors(t *testing.T, name string, dst any) {
	t.Helper()
	path := filepath.Join("..", "..", "..", "protocol", "vectors", name)
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	if err := json.Unmarshal(b, dst); err != nil {
		t.Fatalf("parse %s: %v", path, err)
	}
}

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("hex %q: %v", s, err)
	}
	return b
}

// message.json: every valid entry round-trips and every malformed entry fails
// with the named reason.
func TestMessageVectors(t *testing.T) {
	var v struct {
		Valid []struct {
			Name       string `json:"name"`
			Type       uint8  `json:"type"`
			ID         uint16 `json:"id"`
			PayloadHex string `json:"payload_hex"`
			BytesHex   string `json:"bytes_hex"`
		} `json:"valid"`
		Malformed []struct {
			Name     string `json:"name"`
			BytesHex string `json:"bytes_hex"`
			Reason   string `json:"reason"`
		} `json:"malformed"`
	}
	loadVectors(t, "message.json", &v)
	if len(v.Valid) == 0 || len(v.Malformed) == 0 {
		t.Fatal("message.json missing cases")
	}

	for _, c := range v.Valid {
		raw := mustHex(t, c.BytesHex)
		got, err := Decode(raw)
		if err != nil {
			t.Errorf("%s: Decode: %v", c.Name, err)
			continue
		}
		if uint8(got.Type) != c.Type || got.ID != c.ID || hex.EncodeToString(got.Payload) != c.PayloadHex {
			t.Errorf("%s: decoded type=%d id=%d payload=%x", c.Name, got.Type, got.ID, got.Payload)
		}
		enc, err := got.Encode()
		if err != nil {
			t.Errorf("%s: Encode: %v", c.Name, err)
			continue
		}
		if hex.EncodeToString(enc) != c.BytesHex {
			t.Errorf("%s: Encode = %x, want %s", c.Name, enc, c.BytesHex)
		}
	}

	for _, c := range v.Malformed {
		_, err := Decode(mustHex(t, c.BytesHex))
		var de *DecodeError
		if !errors.As(err, &de) {
			t.Errorf("%s: Decode error = %v, want *DecodeError", c.Name, err)
			continue
		}
		if de.Reason != c.Reason {
			t.Errorf("%s: reason = %q, want %q", c.Name, de.Reason, c.Reason)
		}
	}
}

// segment.json: packets and next_seq match at every packet size, and cases
// that start at seq 0 reassemble back to the original message.
func TestSegmentVectors(t *testing.T) {
	var cases []struct {
		Name       string   `json:"name"`
		PacketSize int      `json:"packet_size"`
		StartSeq   uint8    `json:"start_seq"`
		MessageHex string   `json:"message_hex"`
		PacketsHex []string `json:"packets_hex"`
		NextSeq    uint8    `json:"next_seq"`
	}
	loadVectors(t, "segment.json", &cases)
	if len(cases) == 0 {
		t.Fatal("segment.json missing cases")
	}

	for _, c := range cases {
		msg := mustHex(t, c.MessageHex)
		seg := Segmenter{Seq: c.StartSeq}
		pkts := seg.Segment(msg, c.PacketSize)

		if len(pkts) != len(c.PacketsHex) {
			t.Errorf("%s: got %d packets, want %d", c.Name, len(pkts), len(c.PacketsHex))
			continue
		}
		for i, p := range pkts {
			if hex.EncodeToString(p) != c.PacketsHex[i] {
				t.Errorf("%s: packet %d = %x, want %s", c.Name, i, p, c.PacketsHex[i])
			}
		}
		if seg.Seq != c.NextSeq {
			t.Errorf("%s: next_seq = %d, want %d", c.Name, seg.Seq, c.NextSeq)
		}

		if c.StartSeq != 0 {
			continue
		}
		var r Reassembler
		var msgs [][]byte
		for _, p := range pkts {
			for _, ev := range r.Feed(p) {
				if ev.Reset != "" {
					t.Errorf("%s: unexpected reset %q", c.Name, ev.Reset)
				}
				if ev.Message != nil {
					msgs = append(msgs, ev.Message)
				}
			}
		}
		if len(msgs) != 1 || hex.EncodeToString(msgs[0]) != c.MessageHex {
			t.Errorf("%s: reassembled %d messages to %x, want 1 to %s",
				c.Name, len(msgs), msgs, c.MessageHex)
		}
	}
}

// reassemble.json: the packets produce exactly the recorded events, in order.
func TestReassembleVectors(t *testing.T) {
	var cases []struct {
		Name       string   `json:"name"`
		PacketsHex []string `json:"packets_hex"`
		Events     []struct {
			Kind   string `json:"kind"`
			Hex    string `json:"hex"`
			Reason string `json:"reason"`
		} `json:"events"`
	}
	loadVectors(t, "reassemble.json", &cases)
	if len(cases) == 0 {
		t.Fatal("reassemble.json missing cases")
	}

	for _, c := range cases {
		var r Reassembler
		var got []Event
		for _, h := range c.PacketsHex {
			got = append(got, r.Feed(mustHex(t, h))...)
		}

		if len(got) != len(c.Events) {
			t.Errorf("%s: got %d events, want %d (%v)", c.Name, len(got), len(c.Events), got)
			continue
		}
		for i, want := range c.Events {
			switch want.Kind {
			case "message":
				if got[i].Reset != "" || hex.EncodeToString(got[i].Message) != want.Hex {
					t.Errorf("%s: event %d = %+v, want message %s", c.Name, i, got[i], want.Hex)
				}
			case "reset":
				if got[i].Message != nil || got[i].Reset != want.Reason {
					t.Errorf("%s: event %d = %+v, want reset %q", c.Name, i, got[i], want.Reason)
				}
			default:
				t.Errorf("%s: unknown vector kind %q", c.Name, want.Kind)
			}
		}
	}
}

// fakeServer answers sys.ping by echoing params and everything else with ERR
// unsupported, over a net.Pipe using the same record framing as the client.
func fakeServer(t *testing.T, conn net.Conn) {
	t.Helper()
	defer conn.Close()
	var re Reassembler
	var seg Segmenter
	hdr := make([]byte, 2)
	for {
		if _, err := io.ReadFull(conn, hdr); err != nil {
			return
		}
		pkt := make([]byte, int(binary.LittleEndian.Uint16(hdr)))
		if _, err := io.ReadFull(conn, pkt); err != nil {
			return
		}
		for _, ev := range re.Feed(pkt) {
			if ev.Reset != "" {
				continue
			}
			msg, err := Decode(ev.Message)
			if err != nil || msg.Type != Req {
				continue
			}
			var req struct {
				M string          `json:"m"`
				P json.RawMessage `json:"p"`
			}
			_ = json.Unmarshal(msg.Payload, &req)

			out := Message{Type: Err, ID: msg.ID, Payload: []byte(`{"code":"unsupported","msg":"x"}`)}
			if req.M == "sys.ping" {
				if len(req.P) == 0 {
					req.P = json.RawMessage("{}")
				}
				out = Message{Type: Res, ID: msg.ID, Payload: req.P}
			}
			raw, err := out.Encode()
			if err != nil {
				t.Errorf("server encode: %v", err)
				return
			}
			for _, p := range seg.Segment(raw, TCPPacketSize) {
				rec := make([]byte, 2+len(p))
				binary.LittleEndian.PutUint16(rec, uint16(len(p)))
				copy(rec[2:], p)
				if _, err := conn.Write(rec); err != nil {
					return
				}
			}
		}
	}
}

func TestClientCallAndError(t *testing.T) {
	clientEnd, serverEnd := net.Pipe()
	c := newClient(clientEnd)
	defer c.Close()
	go fakeServer(t, serverEnd)

	res, err := c.Call("sys.ping", map[string]int{"t": 42}, time.Second)
	if err != nil {
		t.Fatalf("Call(sys.ping): %v", err)
	}
	var got map[string]int
	if err := json.Unmarshal(res, &got); err != nil {
		t.Fatalf("ping result %s: %v", res, err)
	}
	if got["t"] != 42 {
		t.Fatalf("ping echoed %v, want t=42", got)
	}

	_, err = c.Call("foo", nil, time.Second)
	var re *RPCError
	if !errors.As(err, &re) {
		t.Fatalf("Call(foo) error = %v, want *RPCError", err)
	}
	if re.Code != "unsupported" {
		t.Fatalf("Call(foo) code = %q, want \"unsupported\"", re.Code)
	}
}

func TestClientCallTimeout(t *testing.T) {
	clientEnd, serverEnd := net.Pipe()
	c := newClient(clientEnd)
	defer c.Close()
	// Drain requests but never reply, so the Call must expire.
	go func() {
		buf := make([]byte, 4096)
		for {
			if _, err := serverEnd.Read(buf); err != nil {
				return
			}
		}
	}()

	start := time.Now()
	_, err := c.Call("sys.ping", nil, 40*time.Millisecond)
	if err == nil || err.Error() != "timeout" {
		t.Fatalf("Call error = %v, want timeout", err)
	}
	if elapsed := time.Since(start); elapsed < 30*time.Millisecond {
		t.Fatalf("Call returned after %v, want >= timeout", elapsed)
	}
}
