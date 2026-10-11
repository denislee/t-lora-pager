package pagerlink

// These tests drive a Client's responder role against a fake peer over a
// net.Pipe. The fake speaks the real wire format (the package's own
// Segmenter/Reassembler and the same u16 record framing) and, unlike the fake
// pager in files_test.go, it can originate REQ messages — the direction the
// responder exists for. It reuses makeSource from xfer_test.go.

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"hash/crc32"
	"io"
	"net"
	"testing"
	"time"
)

// fakePeer is a pager that talks to a Client: it reads records on a background
// goroutine and hands the decoded messages to the test on a channel, and it can
// send REQ/DATA/ACK of its own.
type fakePeer struct {
	t    *testing.T
	conn net.Conn
	seg  Segmenter
	re   Reassembler
	in   chan Message
}

func newFakePeer(t *testing.T, conn net.Conn) *fakePeer {
	p := &fakePeer{t: t, conn: conn, in: make(chan Message, 64)}
	go p.read()
	return p
}

func (p *fakePeer) read() {
	defer close(p.in)
	var hdr [2]byte
	for {
		if _, err := io.ReadFull(p.conn, hdr[:]); err != nil {
			return
		}
		pkt := make([]byte, int(binary.LittleEndian.Uint16(hdr[:])))
		if _, err := io.ReadFull(p.conn, pkt); err != nil {
			return
		}
		for _, ev := range p.re.Feed(pkt) {
			if ev.Reset != "" {
				continue
			}
			msg, err := Decode(ev.Message)
			if err != nil {
				continue
			}
			p.in <- msg
		}
	}
}

// send encodes one message and writes every packet as a record.
func (p *fakePeer) send(t MsgType, id uint16, payload []byte) {
	p.t.Helper()
	raw, err := Message{Type: t, ID: id, Payload: payload}.Encode()
	if err != nil {
		p.t.Fatalf("fake peer encode: %v", err)
	}
	for _, pkt := range p.seg.Segment(raw, TCPPacketSize) {
		rec := make([]byte, 2+len(pkt))
		binary.LittleEndian.PutUint16(rec, uint16(len(pkt)))
		copy(rec[2:], pkt)
		if _, err := p.conn.Write(rec); err != nil {
			return
		}
	}
}

// sendReq marshals {"m":method,"p":params} and sends it as a REQ.
func (p *fakePeer) sendReq(id uint16, method string, params any) {
	p.t.Helper()
	body, err := marshalRequest(method, params)
	if err != nil {
		p.t.Fatalf("marshal request: %v", err)
	}
	p.send(Req, id, body)
}

func (p *fakePeer) recv(timeout time.Duration) (Message, bool) {
	select {
	case m, ok := <-p.in:
		return m, ok
	case <-time.After(timeout):
		return Message{}, false
	}
}

func (p *fakePeer) mustRecv(timeout time.Duration) Message {
	p.t.Helper()
	m, ok := p.recv(timeout)
	if !ok {
		p.t.Fatalf("fake peer: no message within %v", timeout)
	}
	return m
}

func decodeResult(t *testing.T, m Message) map[string]any {
	t.Helper()
	var got map[string]any
	if err := json.Unmarshal(m.Payload, &got); err != nil {
		t.Fatalf("RES payload %q: %v", m.Payload, err)
	}
	return got
}

func decodeErr(t *testing.T, m Message) *RPCError {
	t.Helper()
	var body struct {
		Code string `json:"code"`
		Msg  string `json:"msg"`
	}
	if err := json.Unmarshal(m.Payload, &body); err != nil {
		t.Fatalf("ERR payload %q: %v", m.Payload, err)
	}
	return &RPCError{Code: body.Code, Msg: body.Msg}
}

func TestHandleAnswersPagerRequest(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	c.Handle("sys.ping", func(params json.RawMessage) (any, *RPCError) {
		var p struct {
			T int `json:"t"`
		}
		if err := json.Unmarshal(params, &p); err != nil {
			return nil, &RPCError{Code: "bad_request", Msg: err.Error()}
		}
		return map[string]any{"t": p.T}, nil
	})

	peer.sendReq(1, "sys.ping", map[string]any{"t": 7})

	m := peer.mustRecv(2 * time.Second)
	if m.Type != Res || m.ID != 1 {
		t.Fatalf("reply type=%d id=%d, want RES id 1", m.Type, m.ID)
	}
	if got := decodeResult(t, m); got["t"] != float64(7) {
		t.Fatalf(`RES = %v, want {"t":7}`, got)
	}
}

func TestHandleUnknownMethod(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	peer.sendReq(1, "sys.nope", nil)

	m := peer.mustRecv(2 * time.Second)
	if m.Type != Err || m.ID != 1 {
		t.Fatalf("reply type=%d id=%d, want ERR id 1", m.Type, m.ID)
	}
	if e := decodeErr(t, m); e.Code != "unsupported" {
		t.Fatalf("code = %q, want unsupported", e.Code)
	}
}

func TestHandleEvenIDRejected(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	c.Handle("sys.ping", func(json.RawMessage) (any, *RPCError) {
		t.Fatal("handler ran for an even request id")
		return nil, nil
	})

	// Even id and id 0 are both parity violations.
	for _, id := range []uint16{2, 0} {
		peer.sendReq(id, "sys.ping", map[string]any{"t": 1})
		m := peer.mustRecv(2 * time.Second)
		if m.Type != Err || m.ID != id {
			t.Fatalf("id %d: reply type=%d id=%d, want ERR", id, m.Type, m.ID)
		}
		if e := decodeErr(t, m); e.Code != "invalid_argument" {
			t.Fatalf("id %d: code = %q, want invalid_argument", id, e.Code)
		}
	}
}

func TestHandlerError(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	c.Handle("fs.fail", func(json.RawMessage) (any, *RPCError) {
		return nil, &RPCError{Code: "invalid_argument", Msg: "nope"}
	})
	// A handler error with an empty code must surface as internal (§3.4).
	c.Handle("fs.boom", func(json.RawMessage) (any, *RPCError) {
		return nil, &RPCError{Msg: "kaboom"}
	})

	peer.sendReq(1, "fs.fail", nil)
	if e := decodeErr(t, peer.mustRecv(2*time.Second)); e.Code != "invalid_argument" {
		t.Fatalf("code = %q, want invalid_argument", e.Code)
	}

	peer.sendReq(3, "fs.boom", nil)
	if e := decodeErr(t, peer.mustRecv(2*time.Second)); e.Code != "internal" {
		t.Fatalf("empty-code error = %q, want internal", e.Code)
	}
}

func TestHandleBadRequest(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	// Not JSON at all, and a JSON object with no "m".
	peer.send(Req, 1, []byte("nope"))
	if e := decodeErr(t, peer.mustRecv(2*time.Second)); e.Code != "bad_request" {
		t.Fatalf("non-JSON code = %q, want bad_request", e.Code)
	}
	peer.send(Req, 3, []byte(`{}`))
	if e := decodeErr(t, peer.mustRecv(2*time.Second)); e.Code != "bad_request" {
		t.Fatalf("missing-m code = %q, want bad_request", e.Code)
	}
}

func TestHandleNilResultAndAbsentParams(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	// A nil result marshals to {}; with no "p" the handler still sees valid
	// JSON ({}), which it echoes back to prove the default took effect.
	c.Handle("sys.echo", func(params json.RawMessage) (any, *RPCError) {
		if !bytes.Equal(params, []byte(`{}`)) {
			t.Errorf("params = %q, want {}", params)
		}
		return nil, nil
	})

	peer.sendReq(1, "sys.echo", nil)
	m := peer.mustRecv(2 * time.Second)
	if m.Type != Res || m.ID != 1 {
		t.Fatalf("reply type=%d id=%d, want RES id 1", m.Type, m.ID)
	}
	if string(m.Payload) != "{}" {
		t.Fatalf("RES payload = %q, want {}", m.Payload)
	}
}

// beginPeer starts a handler that accepts a transfer of size/crc and asks the
// peer to start it. It returns the id the fake peer read from the RES.
func beginPeer(t *testing.T, c *Client, peer *fakePeer, size, crc uint32) (uint16, <-chan struct{}, *[]byte, *error) {
	t.Helper()
	var (
		done = make(chan struct{})
		data []byte
		err  error
	)
	c.Handle("x.begin", func(json.RawMessage) (any, *RPCError) {
		id, wait := c.AcceptBytes(size, crc)
		go func() {
			data, err = wait(5 * time.Second)
			close(done)
		}()
		return map[string]any{"xfer": id}, nil
	})

	peer.sendReq(1, "x.begin", map[string]any{"size": size})
	res := peer.mustRecv(2 * time.Second)
	if res.Type != Res || res.ID != 1 {
		t.Fatalf("x.begin reply type=%d id=%d, want RES id 1", res.Type, res.ID)
	}
	id := uint16(decodeResult(t, res)["xfer"].(float64))
	if id == 0 || id%2 != 0 {
		t.Fatalf("xfer id = %d, want even non-zero", id)
	}
	return id, done, &data, &err
}

func TestAcceptBytes(t *testing.T) {
	const size = 50000
	src := makeSource(size)

	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	id, done, data, werr := beginPeer(t, c, peer, size, crc32.ChecksumIEEE(src))

	// Stream the bytes as DATA under the client's ACK window, exactly as the
	// pager would for a read the phone receives.
	tx := NewSender(size, MaxDataChunk, 0,
		func(off uint32, n int) ([]byte, error) { return src[off : off+uint32(n)], nil },
		func(payload []byte) error { peer.send(Data, id, payload); return nil },
	)
	tx.Pump()
	for tx.State() == XferActive {
		m := peer.mustRecv(5 * time.Second)
		if m.Type != Ack || m.ID != id {
			t.Fatalf("during DATA: got type=%d id=%d, want ACK for %d", m.Type, m.ID, id)
		}
		tx.OnAck(m.Payload)
		tx.Pump()
	}

	<-done
	if *werr != nil {
		t.Fatalf("wait: %v", *werr)
	}
	if !bytes.Equal(*data, src) {
		t.Fatalf("received %d bytes, want %d identical", len(*data), size)
	}
}

func TestAcceptBytesCRCMismatch(t *testing.T) {
	src := makeSource(4096)

	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	id, done, _, werr := beginPeer(t, c, peer, uint32(len(src)), crc32.ChecksumIEEE(src)^0xFFFF)

	tx := NewSender(uint32(len(src)), MaxDataChunk, 0,
		func(off uint32, n int) ([]byte, error) { return src[off : off+uint32(n)], nil },
		func(payload []byte) error { peer.send(Data, id, payload); return nil },
	)
	tx.Pump()
	for tx.State() == XferActive {
		m := peer.mustRecv(5 * time.Second)
		if m.Type != Ack {
			continue
		}
		tx.OnAck(m.Payload)
		tx.Pump()
	}

	<-done
	if *werr == nil || (*werr).Error() != "crc mismatch" {
		t.Fatalf("wait error = %v, want crc mismatch", *werr)
	}
}

func TestAcceptBytesZeroSize(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	defer peerEnd.Close()
	c := newClient(clientEnd)
	defer c.Close()

	id, wait := c.AcceptBytes(0, 0)
	if id == 0 || id%2 != 0 {
		t.Fatalf("zero-size xfer id = %d, want even non-zero", id)
	}
	data, err := wait(2 * time.Second)
	if err != nil {
		t.Fatalf("wait: %v", err)
	}
	if len(data) != 0 {
		t.Fatalf("wait returned %d bytes, want 0", len(data))
	}
	// The transfer was closed on exit.
	c.xferMu.Lock()
	_, busy := c.xfers[id]
	c.xferMu.Unlock()
	if busy {
		t.Fatalf("xfer %d still registered after wait", id)
	}
}

func TestEmit(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	if err := c.Emit("evt.battery", map[string]any{"pct": 50}); err != nil {
		t.Fatalf("Emit: %v", err)
	}
	m := peer.mustRecv(2 * time.Second)
	if m.Type != Evt || m.ID != 0 {
		t.Fatalf("EVT type=%d id=%d, want EVT id 0", m.Type, m.ID)
	}
	var n Notification
	if err := json.Unmarshal(m.Payload, &n); err != nil {
		t.Fatalf("EVT payload %q: %v", m.Payload, err)
	}
	if n.Name != "evt.battery" {
		t.Fatalf("event = %q, want evt.battery", n.Name)
	}
	var p struct {
		Pct int `json:"pct"`
	}
	if err := json.Unmarshal(n.Params, &p); err != nil || p.Pct != 50 {
		t.Fatalf("params = %q (pct=%d, err=%v), want {\"pct\":50}", n.Params, p.Pct, err)
	}

	// Nil params go out as {}.
	if err := c.Emit("evt.fs.changed", nil); err != nil {
		t.Fatalf("Emit nil: %v", err)
	}
	m = peer.mustRecv(2 * time.Second)
	if !bytes.Contains(m.Payload, []byte(`"p":{}`)) {
		t.Fatalf("nil-params payload = %q, want \"p\":{}", m.Payload)
	}

	// An unmarshalable param fails before anything is sent.
	if err := c.Emit("evt.bad", json.RawMessage("{")); err == nil {
		t.Fatal("Emit with invalid params returned nil")
	}
}
