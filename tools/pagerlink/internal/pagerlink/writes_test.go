package pagerlink

// These tests pin the write path and the idle-watchdog behaviour of the
// client: a message's records must reach the connection in exactly one Write
// (one flush per message), and Call, ReadFile and AcceptBytes must give up on
// a silent peer rather than block forever. They reuse fakePeer from
// responder_test.go, which speaks the real wire format over a net.Pipe.

import (
	"encoding/json"
	"errors"
	"io"
	"net"
	"sync/atomic"
	"testing"
	"time"
)

// countingConn counts the Write calls made through it. The Client's bufio.Writer
// sits on top, so one flush — one message — must show up as one Write.
type countingConn struct {
	net.Conn
	writes atomic.Int64
}

func (c *countingConn) Write(b []byte) (int, error) {
	c.writes.Add(1)
	return c.Conn.Write(b)
}

// TestSendFlushesOnce asserts that a multi-packet message is buffered and
// flushed as a single conn.Write, and that SendRaw adds exactly one more.
func TestSendFlushesOnce(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	cc := &countingConn{Conn: clientEnd}
	c := newClient(cc)

	// Drain everything the client writes so net.Pipe's synchronous Write
	// never blocks and the count reflects flushes, not partial writes.
	drained := make(chan struct{})
	go func() {
		defer close(drained)
		_, _ = io.Copy(io.Discard, peerEnd)
	}()

	// A 1500-byte bulk payload plus the header spans several TCP packets, so
	// a naive one-flush-per-record implementation would make many Writes.
	payload := EncodeData(0, make([]byte, 1500))
	raw, err := Message{Type: Data, ID: 2, Payload: payload}.Encode()
	if err != nil {
		t.Fatalf("encode DATA: %v", err)
	}
	if pkts := (&Segmenter{}).Segment(raw, TCPPacketSize); len(pkts) < 2 {
		t.Fatalf("test payload segmented into %d packet(s), want several", len(pkts))
	}

	if err := c.SendBulk(Data, 2, payload); err != nil {
		t.Fatalf("SendBulk: %v", err)
	}
	if got := cc.writes.Load(); got != 1 {
		t.Fatalf("SendBulk made %d conn.Write calls, want 1", got)
	}

	if err := c.SendRaw([]byte{1, 2, 3}); err != nil {
		t.Fatalf("SendRaw: %v", err)
	}
	if got := cc.writes.Load(); got != 2 {
		t.Fatalf("after SendRaw: %d conn.Write calls, want 2", got)
	}

	if err := c.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	select {
	case <-drained:
	case <-time.After(time.Second):
		t.Fatal("peer did not see EOF within 1s of Close")
	}
}

// TestCallTimeout has the peer read the REQ and never answer: Call must return
// ErrTimeout on its own deadline.
func TestCallTimeout(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	newFakePeer(t, peerEnd) // reads and drops every message; never replies
	c := newClient(clientEnd)
	defer c.Close()

	done := make(chan error, 1)
	go func() {
		_, err := c.Call("sys.ping", nil, 50*time.Millisecond)
		done <- err
	}()

	select {
	case err := <-done:
		if !errors.Is(err, ErrTimeout) {
			t.Fatalf("Call = %v, want ErrTimeout", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Call did not return within 1s")
	}
}

// TestAcceptBytesIdleTimeout asserts the wait function uses an idle timeout
// rather than waiting forever for DATA that never comes.
func TestAcceptBytesIdleTimeout(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	defer peerEnd.Close() // stay open: a closed peer would surface as disconnect
	c := newClient(clientEnd)
	defer c.Close()

	id, wait := c.AcceptBytes(10, 0)
	if id == 0 || id%2 != 0 {
		t.Fatalf("xfer id = %d, want even non-zero", id)
	}

	done := make(chan error, 1)
	go func() {
		_, err := wait(50 * time.Millisecond)
		done <- err
	}()

	select {
	case err := <-done:
		if err == nil || err.Error() != "accept: timeout" || !errors.Is(err, ErrTimeout) {
			t.Fatalf("wait error = %v, want accept: timeout wrapping ErrTimeout", err)
		}
	case <-time.After(time.Second):
		t.Fatal("wait did not return within 1s")
	}
}

// TestReadFileIdleTimeout has the peer answer the fs.read REQ with a RES that
// promises 10 bytes, then send no DATA: ReadFile must fail on its idle timer,
// not on the fs.read RPC deadline.
func TestReadFileIdleTimeout(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	peer := newFakePeer(t, peerEnd)
	c := newClient(clientEnd)
	defer c.Close()

	done := make(chan error, 1)
	go func() {
		_, _, err := c.ReadFile("int", "/x", 100*time.Millisecond)
		done <- err
	}()

	req := peer.mustRecv(2 * time.Second)
	if req.Type != Req {
		t.Fatalf("first message type=%d, want REQ", req.Type)
	}
	var body struct {
		M string `json:"m"`
		P struct {
			Xfer uint16 `json:"xfer"`
		} `json:"p"`
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		t.Fatalf("REQ payload %q: %v", req.Payload, err)
	}
	if body.M != "fs.read" {
		t.Fatalf("REQ method = %q, want fs.read", body.M)
	}
	if body.P.Xfer == 0 || body.P.Xfer%2 != 0 {
		t.Fatalf("fs.read xfer = %d, want even non-zero", body.P.Xfer)
	}

	// Answer the RPC but never stream the DATA it announced.
	peer.send(Res, req.ID, []byte(`{"size":10,"crc32":0}`))

	select {
	case err := <-done:
		if err == nil || err.Error() != "fs.read: timeout" || !errors.Is(err, ErrTimeout) {
			t.Fatalf("ReadFile error = %v, want fs.read: timeout wrapping ErrTimeout", err)
		}
	case <-time.After(time.Second):
		t.Fatal("ReadFile did not return within 1s")
	}
}

// TestDoneClosesOnDrop asserts Done stays open while the link is up and closes
// once the peer hangs up.
func TestDoneClosesOnDrop(t *testing.T) {
	clientEnd, peerEnd := net.Pipe()
	c := newClient(clientEnd)
	defer c.Close()

	select {
	case <-c.Done():
		t.Fatal("Done closed while the link is up")
	case <-time.After(20 * time.Millisecond):
	}

	peerEnd.Close()
	select {
	case <-c.Done():
	case <-time.After(time.Second):
		t.Fatal("Done not closed within 1s of the peer hanging up")
	}
}
