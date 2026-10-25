package pagerlink

// These tests drive a Client against a fake pager over a net.Pipe. The fake
// speaks the real wire format (wire_test.go) and serves the fs.*
// bulk operations the handlers_fs.cpp pager implements: fs.read (RES then DATA
// driven by the client's ACKs) and fs.write.begin/commit/abort (a Receiver that
// ACKs the client's DATA).

import (
	"bytes"
	"encoding/json"
	"errors"
	"hash/crc32"
	"net"
	"sync"
	"testing"
	"time"
)

// fakeConfig is the fake pager's fixed behaviour, set before it starts serving.
type fakeConfig struct {
	readData    map[string][]byte // path -> bytes fs.read serves
	crcOverride map[string]uint32 // path -> CRC reported instead of the real one
	failBegin   bool              // answer fs.write.begin with ERR invalid_argument
	dropData    bool              // ignore upload DATA (never ACK), to stall WriteFile
}

// fakeXfer is one live transfer on the fake pager.
type fakeXfer struct {
	id      uint16
	upload  bool   // true when DATA flows client -> fake pager
	path    string // upload destination
	wantCRC uint32
	tx      *Sender   // reads (client downloads)
	rx      *Receiver // writes (client uploads)
	buf     bytes.Buffer
}

// fakePager is a single-goroutine pager: it reads records, dispatches messages
// and writes replies through one Segmenter, exactly as the firmware link task
// does.
type fakePager struct {
	wire
	t   *testing.T
	cfg fakeConfig

	xfers  map[uint16]*fakeXfer
	nextID uint16 // odd upload ids, the pager's parity

	mu     sync.Mutex
	files  map[string][]byte // committed uploads
	aborts map[uint16]bool
}

func newFakePager(t *testing.T, conn net.Conn, cfg fakeConfig) *fakePager {
	if cfg.readData == nil {
		cfg.readData = map[string][]byte{}
	}
	if cfg.crcOverride == nil {
		cfg.crcOverride = map[string]uint32{}
	}
	f := &fakePager{
		wire:   wire{conn: conn},
		t:      t,
		cfg:    cfg,
		xfers:  make(map[uint16]*fakeXfer),
		nextID: 1,
		files:  make(map[string][]byte),
		aborts: make(map[uint16]bool),
	}
	go f.serve()
	return f
}

func (f *fakePager) serve() {
	defer f.conn.Close()
	f.readLoop(f.handle)
}

// write encodes one message and writes every packet as a record.
func (f *fakePager) write(t MsgType, id uint16, payload []byte) {
	if err := f.writeMsg(t, id, payload); err != nil {
		f.t.Errorf("fake pager encode: %v", err)
	}
}

func (f *fakePager) writeErr(id uint16, code, msg string) {
	body, _ := json.Marshal(map[string]string{"code": code, "msg": msg})
	f.write(Err, id, body)
}

// written returns the committed bytes for path, if the fake has any.
func (f *fakePager) written(path string) ([]byte, bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	b, ok := f.files[path]
	return b, ok
}

func (f *fakePager) handle(msg Message) {
	switch msg.Type {
	case Req:
		f.handleReq(msg)
	case Data:
		f.handleData(msg)
	case Ack:
		f.handleAck(msg)
	}
}

func (f *fakePager) handleReq(msg Message) {
	var req struct {
		M string          `json:"m"`
		P json.RawMessage `json:"p"`
	}
	if err := json.Unmarshal(msg.Payload, &req); err != nil {
		f.writeErr(msg.ID, "bad_request", "bad")
		return
	}
	switch req.M {
	case "fs.read":
		f.onRead(msg.ID, req.P)
	case "fs.write.begin":
		f.onWriteBegin(msg.ID, req.P)
	case "fs.write.commit":
		f.onWriteCommit(msg.ID, req.P)
	case "fs.write.abort":
		f.onWriteAbort(msg.ID, req.P)
	default:
		f.writeErr(msg.ID, "unsupported", "x")
	}
}

func (f *fakePager) onRead(reqID uint16, params json.RawMessage) {
	var p struct {
		Path string `json:"path"`
		Xfer uint16 `json:"xfer"`
	}
	if err := json.Unmarshal(params, &p); err != nil || p.Xfer == 0 || p.Xfer%2 != 0 {
		f.writeErr(reqID, "invalid_argument", "bad xfer")
		return
	}
	data, ok := f.cfg.readData[p.Path]
	if !ok {
		f.writeErr(reqID, "invalid_argument", "not found")
		return
	}
	crc := crc32.ChecksumIEEE(data)
	if alt, ok := f.cfg.crcOverride[p.Path]; ok {
		crc = alt
	}
	body, _ := json.Marshal(map[string]any{
		"xfer": p.Xfer, "size": uint32(len(data)), "crc32": crc, "protected": false,
	})
	f.write(Res, reqID, body)

	tx := NewSender(uint32(len(data)), MaxDataChunk, 0,
		func(off uint32, n int) ([]byte, error) { return data[off : off+uint32(n)], nil },
		func(payload []byte) error { f.write(Data, p.Xfer, payload); return nil },
	)
	f.xfers[p.Xfer] = &fakeXfer{id: p.Xfer, tx: tx}
	if tx.Pump() != XferActive {
		delete(f.xfers, p.Xfer)
	}
}

func (f *fakePager) handleAck(msg Message) {
	x, ok := f.xfers[msg.ID]
	if !ok || x.upload || x.tx == nil {
		return
	}
	x.tx.OnAck(msg.Payload)
	if x.tx.Pump() != XferActive {
		delete(f.xfers, msg.ID)
	}
}

func (f *fakePager) onWriteBegin(reqID uint16, params json.RawMessage) {
	if f.cfg.failBegin {
		f.writeErr(reqID, "invalid_argument", "no")
		return
	}
	var p struct {
		Path      string `json:"path"`
		Size      uint32 `json:"size"`
		CRC32     uint32 `json:"crc32"`
		Overwrite bool   `json:"overwrite"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		f.writeErr(reqID, "invalid_argument", "bad params")
		return
	}
	id := f.nextID
	f.nextID += 2
	x := &fakeXfer{id: id, upload: true, path: p.Path, wantCRC: p.CRC32}
	x.rx = NewReceiver(p.Size, InitialCredits, 4, 0,
		func(_ uint32, b []byte) error { x.buf.Write(b); return nil },
		func(payload []byte) error { f.write(Ack, id, payload); return nil },
	)
	f.xfers[id] = x
	body, _ := json.Marshal(map[string]any{"xfer": id, "offset": 0})
	f.write(Res, reqID, body)
}

func (f *fakePager) handleData(msg Message) {
	if f.cfg.dropData {
		return
	}
	x, ok := f.xfers[msg.ID]
	if !ok || !x.upload || x.rx == nil {
		return
	}
	if x.rx.OnData(msg.Payload) == XferFailed {
		delete(f.xfers, msg.ID)
	}
}

func (f *fakePager) onWriteCommit(reqID uint16, params json.RawMessage) {
	var p struct {
		Xfer uint16 `json:"xfer"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		f.writeErr(reqID, "invalid_argument", "bad params")
		return
	}
	x, ok := f.xfers[p.Xfer]
	if !ok || !x.upload {
		f.writeErr(reqID, "invalid_argument", "no such transfer")
		return
	}
	if x.rx.State() != XferDone {
		f.writeErr(reqID, "invalid_argument", "incomplete")
		return
	}
	got := x.buf.Bytes()
	if crc32.ChecksumIEEE(got) != x.wantCRC {
		f.writeErr(reqID, "invalid_argument", "crc mismatch")
		return
	}
	f.mu.Lock()
	f.files[x.path] = append([]byte(nil), got...)
	f.mu.Unlock()
	delete(f.xfers, p.Xfer)
	f.write(Res, reqID, []byte(`{}`))
}

func (f *fakePager) onWriteAbort(reqID uint16, params json.RawMessage) {
	var p struct {
		Xfer uint16 `json:"xfer"`
	}
	_ = json.Unmarshal(params, &p)
	delete(f.xfers, p.Xfer)
	f.mu.Lock()
	f.aborts[p.Xfer] = true
	f.mu.Unlock()
	f.write(Res, reqID, []byte(`{}`))
}

func TestReadFileRoundTrip(t *testing.T) {
	const size = 100 * 1024
	src := makeSource(size)
	clientEnd, serverEnd := net.Pipe()
	newFakePager(t, serverEnd, fakeConfig{readData: map[string][]byte{"/note.bin": src}})
	c := newClient(clientEnd)
	defer c.Close()

	data, info, err := c.ReadFile("int", "/note.bin", 5*time.Second)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if len(data) != size {
		t.Fatalf("ReadFile returned %d bytes, want %d", len(data), size)
	}
	if !bytes.Equal(data, src) {
		t.Fatal("ReadFile bytes differ from the source")
	}
	if info.Size != size {
		t.Fatalf("info.Size = %d, want %d", info.Size, size)
	}
	if want := crc32.ChecksumIEEE(src); info.CRC32 != want {
		t.Fatalf("info.CRC32 = %#x, want %#x", info.CRC32, want)
	}
}

func TestWriteFileRoundTrip(t *testing.T) {
	const size = 100 * 1024
	src := makeSource(size)
	clientEnd, serverEnd := net.Pipe()
	f := newFakePager(t, serverEnd, fakeConfig{})
	c := newClient(clientEnd)
	defer c.Close()

	if err := c.WriteFile("int", "/up.bin", src, true, 5*time.Second); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	got, ok := f.written("/up.bin")
	if !ok {
		t.Fatal("fake pager stored no file")
	}
	if !bytes.Equal(got, src) {
		t.Fatal("stored bytes differ from the source")
	}
}

func TestReadFileCRCMismatch(t *testing.T) {
	src := makeSource(4096)
	clientEnd, serverEnd := net.Pipe()
	newFakePager(t, serverEnd, fakeConfig{
		readData:    map[string][]byte{"/bad.bin": src},
		crcOverride: map[string]uint32{"/bad.bin": crc32.ChecksumIEEE(src) ^ 0xFFFF},
	})
	c := newClient(clientEnd)
	defer c.Close()

	_, _, err := c.ReadFile("int", "/bad.bin", 5*time.Second)
	if err == nil || err.Error() != "crc mismatch" {
		t.Fatalf("ReadFile error = %v, want crc mismatch", err)
	}
}

func TestWriteFileError(t *testing.T) {
	clientEnd, serverEnd := net.Pipe()
	newFakePager(t, serverEnd, fakeConfig{failBegin: true})
	c := newClient(clientEnd)
	defer c.Close()

	err := c.WriteFile("int", "/x.bin", []byte("hello"), false, 5*time.Second)
	var re *RPCError
	if !errors.As(err, &re) {
		t.Fatalf("WriteFile error = %v, want *RPCError", err)
	}
	if re.Code != "invalid_argument" {
		t.Fatalf("WriteFile code = %q, want invalid_argument", re.Code)
	}
}

func TestWriteFileTimeoutAborts(t *testing.T) {
	clientEnd, serverEnd := net.Pipe()
	f := newFakePager(t, serverEnd, fakeConfig{dropData: true})
	c := newClient(clientEnd)
	defer c.Close()

	// More than the initial credit window, so the sender must wait for an ACK
	// the fake pager never sends.
	data := make([]byte, 20*MaxDataChunk)
	err := c.WriteFile("int", "/x.bin", data, false, 100*time.Millisecond)
	if err == nil || err.Error() != "fs.write: timeout" {
		t.Fatalf("WriteFile error = %v, want fs.write: timeout", err)
	}

	// The abort RPC arrives asynchronously; poll for it.
	deadline := time.Now().Add(2 * time.Second)
	for {
		f.mu.Lock()
		n := len(f.aborts)
		f.mu.Unlock()
		if n > 0 {
			return
		}
		if time.Now().After(deadline) {
			t.Fatal("fake pager saw no fs.write.abort")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestAllocXferEvenAndUnique(t *testing.T) {
	clientEnd, serverEnd := net.Pipe()
	defer serverEnd.Close()
	c := newClient(clientEnd)
	defer c.Close()

	// An id already held is skipped.
	held := c.openXfer(2)
	first, _ := c.allocXfer()
	if first != 4 {
		t.Fatalf("allocXfer with 2 held = %d, want 4", first)
	}
	c.closeXfer(2)
	_ = held

	seen := map[uint16]bool{first: true}
	for i := 0; i < 16; i++ {
		id, ch := c.allocXfer()
		if id == 0 || id%2 != 0 {
			t.Fatalf("allocXfer id = %d, want even non-zero", id)
		}
		if seen[id] {
			t.Fatalf("allocXfer returned duplicate id %d", id)
		}
		seen[id] = true
		if ch == nil {
			t.Fatal("allocXfer returned a nil channel")
		}
	}

	// Ids wrap 65534 -> 2.
	c.xferMu.Lock()
	c.nextXfer = 65534
	delete(c.xfers, 65534)
	c.xferMu.Unlock()
	if id, _ := c.allocXfer(); id != 65534 {
		t.Fatalf("allocXfer at wrap = %d, want 65534", id)
	}
	if id, _ := c.allocXfer(); id != 2 {
		t.Fatalf("allocXfer after wrap = %d, want 2", id)
	}
}
