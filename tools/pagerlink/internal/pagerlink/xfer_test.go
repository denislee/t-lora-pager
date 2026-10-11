package pagerlink

import (
	"errors"
	"testing"
)

// These tests mirror test/test_link_xfer/test_main.cpp: a Sender and a Receiver
// are wired together over a pair of payload queues so a test can deliver, drop
// or reorder DATA and ACK messages by hand, and a loopback helper carries the
// whole transfer to completion.

// xferLoop carries encoded DATA payloads one way and encoded ACK payloads the
// other, mirroring the C++ Loop harness.
type xferLoop struct {
	data [][]byte // DATA payloads: sender -> receiver
	ack  [][]byte // ACK payloads: receiver -> sender
}

func (l *xferLoop) sendData(payload []byte) error {
	l.data = append(l.data, append([]byte(nil), payload...))
	return nil
}

func (l *xferLoop) sendAck(payload []byte) error {
	l.ack = append(l.ack, append([]byte(nil), payload...))
	return nil
}

func (l *xferLoop) deliverData(rx *Receiver) {
	p := l.data[0]
	l.data = l.data[1:]
	rx.OnData(p)
}

func (l *xferLoop) deliverAck(tx *Sender) {
	p := l.ack[0]
	l.ack = l.ack[1:]
	tx.OnAck(p)
}

func (l *xferLoop) deliverAllData(rx *Receiver) {
	for len(l.data) > 0 {
		l.deliverData(rx)
	}
}

func (l *xferLoop) deliverAllAcks(tx *Sender) {
	for len(l.ack) > 0 {
		l.deliverAck(tx)
	}
}

// makeSource is the deterministic pattern (i*7+3) & 0xFF used by the C++ tests.
func makeSource(n int) []byte {
	v := make([]byte, n)
	for i := range v {
		v[i] = byte((i*7 + 3) & 0xFF)
	}
	return v
}

// feedData encodes one DATA message and delivers it to rx.
func feedData(rx *Receiver, offset uint32, b []byte) {
	rx.OnData(EncodeData(offset, b))
}

// readFrom copies from src, the read callback shape the Sender expects.
func readFrom(src []byte) func(off uint32, n int) ([]byte, error) {
	return func(off uint32, n int) ([]byte, error) {
		return src[off : off+uint32(n)], nil
	}
}

func TestXferRoundTripLossless(t *testing.T) {
	const size = 100000
	src := makeSource(size)
	dst := make([]byte, size)

	loop := &xferLoop{}
	tx := NewSender(size, 4092, 0, readFrom(src), loop.sendData)
	rx := NewReceiver(size, InitialCredits, 4, 0,
		func(off uint32, b []byte) error { copy(dst[off:], b); return nil },
		loop.sendAck)

	for i := 0; i < 1000000 && (tx.State() != XferDone || rx.State() != XferDone); i++ {
		tx.Pump()
		loop.deliverAllData(rx)
		loop.deliverAllAcks(tx)
	}

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if rx.Received() != size {
		t.Fatalf("received = %d, want %d", rx.Received(), size)
	}
	for i := range src {
		if src[i] != dst[i] {
			t.Fatalf("byte %d = %#x, want %#x", i, dst[i], src[i])
		}
	}
}

func TestSenderRespectsCredits(t *testing.T) {
	const size = 20 * 4092
	src := makeSource(size)

	loop := &xferLoop{}
	tx := NewSender(size, 4092, 0, readFrom(src), loop.sendData)

	tx.Pump() // no ACKs delivered yet

	if len(loop.data) != InitialCredits {
		t.Fatalf("in-flight DATA = %d, want %d", len(loop.data), InitialCredits)
	}
	if tx.Sent() != 16*4092 {
		t.Fatalf("sent = %d, want %d", tx.Sent(), 16*4092)
	}
	if tx.State() != XferActive {
		t.Fatalf("state = %v, want Active", tx.State())
	}
}

func TestAckReplacesWindow(t *testing.T) {
	const size = 20 * 4092
	src := makeSource(size)

	loop := &xferLoop{}
	tx := NewSender(size, 4092, 0, readFrom(src), loop.sendData)

	tx.Pump() // 16 messages in flight, next = 16*4092

	// Shrink the window to 2 starting from 4*4092: 12 messages are still in
	// flight, so the sender must not emit anything more.
	tx.OnAck(EncodeAck(4*4092, 2))

	before := tx.Sent()
	tx.Pump()

	if tx.Sent() != before || tx.Sent() != 16*4092 {
		t.Fatalf("sent = %d (before %d), want %d", tx.Sent(), before, 16*4092)
	}
	if len(loop.data) != 16 {
		t.Fatalf("in-flight DATA = %d, want 16", len(loop.data))
	}
	if tx.Acked() != 4*4092 {
		t.Fatalf("acked = %d, want %d", tx.Acked(), 4*4092)
	}
}

func TestRewindOnDuplicateAck(t *testing.T) {
	const size = 40 * 4092
	src := makeSource(size)
	dst := make([]byte, size)

	loop := &xferLoop{}
	tx := NewSender(size, 4092, 0, readFrom(src), loop.sendData)
	rx := NewReceiver(size, InitialCredits, 4, 0,
		func(off uint32, b []byte) error { copy(dst[off:], b); return nil },
		loop.sendAck)

	tx.Pump()
	if len(loop.data) == 0 {
		t.Fatal("sender produced no DATA")
	}

	// Drop the first DATA message and deliver the rest: every one is at the
	// wrong offset, so the receiver re-ACKs offset 0 without writing.
	loop.data = loop.data[1:]
	loop.deliverAllData(rx)
	if rx.Received() != 0 {
		t.Fatalf("received = %d, want 0", rx.Received())
	}

	// Feeding the re-ACK rewinds the sender to offset 0.
	if len(loop.ack) == 0 {
		t.Fatal("receiver sent no re-ACK")
	}
	loop.deliverAllAcks(tx)
	if tx.Sent() != 0 || tx.Acked() != 0 {
		t.Fatalf("sent/acked = %d/%d, want 0/0", tx.Sent(), tx.Acked())
	}

	// The transfer now completes losslessly.
	for i := 0; i < 1000000 && (tx.State() != XferDone || rx.State() != XferDone); i++ {
		tx.Pump()
		loop.deliverAllData(rx)
		loop.deliverAllAcks(tx)
	}

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	for i := range src {
		if src[i] != dst[i] {
			t.Fatalf("byte %d = %#x, want %#x", i, dst[i], src[i])
		}
	}
}

func TestReceiverIgnoresWrongOffset(t *testing.T) {
	const size = 3 * 4092

	loop := &xferLoop{}
	writes := 0
	rx := NewReceiver(size, InitialCredits, 4, 0,
		func(uint32, []byte) error { writes++; return nil },
		loop.sendAck)

	feedData(rx, 4092, make([]byte, 4092))

	if writes != 0 {
		t.Fatalf("writes = %d, want 0", writes)
	}
	if rx.Received() != 0 {
		t.Fatalf("received = %d, want 0", rx.Received())
	}
	if rx.State() != XferActive {
		t.Fatalf("state = %v, want Active", rx.State())
	}
	if len(loop.ack) != 1 {
		t.Fatalf("ACKs = %d, want 1", len(loop.ack))
	}
	next, _, ok := DecodeAck(loop.ack[0])
	if !ok || next != 0 {
		t.Fatalf("re-ACK next = %d (ok %v), want 0", next, ok)
	}
}

func TestReceiverOverflowFails(t *testing.T) {
	loop := &xferLoop{}
	rx := NewReceiver(10, InitialCredits, 4, 0,
		func(uint32, []byte) error { return nil },
		loop.sendAck)

	feedData(rx, 0, make([]byte, 11)) // one byte past size

	if rx.State() != XferFailed {
		t.Fatalf("state = %v, want Failed", rx.State())
	}
	if rx.Received() != 0 {
		t.Fatalf("received = %d, want 0", rx.Received())
	}
}

func TestZeroSizeDone(t *testing.T) {
	loop := &xferLoop{}
	reads, writes := 0, 0
	tx := NewSender(0, 4092, 0,
		func(uint32, int) ([]byte, error) { reads++; return nil, nil },
		loop.sendData)
	rx := NewReceiver(0, InitialCredits, 4, 0,
		func(uint32, []byte) error { writes++; return nil },
		loop.sendAck)

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if got := tx.Pump(); got != XferDone {
		t.Fatalf("Pump = %v, want Done", got)
	}
	if reads != 0 || writes != 0 {
		t.Fatalf("reads/writes = %d/%d, want 0/0", reads, writes)
	}
	if len(loop.data) != 0 || len(loop.ack) != 0 {
		t.Fatalf("messages = %d/%d, want 0/0", len(loop.data), len(loop.ack))
	}
}

func TestResumeStartOffset(t *testing.T) {
	const size = 4 * 4092
	src := makeSource(size)
	dst := make([]byte, size)

	loop := &xferLoop{}
	tx := NewSender(size, 4092, 8184, readFrom(src), loop.sendData)

	tx.Pump()
	if len(loop.data) == 0 {
		t.Fatal("sender produced no DATA")
	}
	off, _, ok := DecodeData(loop.data[0])
	if !ok || off != 8184 {
		t.Fatalf("first DATA offset = %d (ok %v), want 8184", off, ok)
	}

	// A receiver resuming at the same offset accepts it.
	rx := NewReceiver(size, InitialCredits, 4, 8184,
		func(o uint32, b []byte) error { copy(dst[o:], b); return nil },
		loop.sendAck)
	if rx.State() != XferActive {
		t.Fatalf("state = %v, want Active", rx.State())
	}
	loop.deliverData(rx)
	if rx.Received() != 8184+4092 {
		t.Fatalf("received = %d, want %d", rx.Received(), 8184+4092)
	}
}

func TestReadErrorFails(t *testing.T) {
	const size = 2 * 4092
	loop := &xferLoop{}
	tx := NewSender(size, 4092, 0,
		func(uint32, int) ([]byte, error) { return nil, errors.New("read") },
		loop.sendData)

	if got := tx.Pump(); got != XferFailed {
		t.Fatalf("Pump = %v, want Failed", got)
	}
	if len(loop.data) != 0 {
		t.Fatalf("DATA sent = %d, want 0", len(loop.data))
	}
}

func TestAckEvery(t *testing.T) {
	const size = 6 * 4092
	src := makeSource(size)

	loop := &xferLoop{}
	rx := NewReceiver(size, InitialCredits, 4, 0,
		func(uint32, []byte) error { return nil },
		loop.sendAck)

	// The first three in-order messages go unacknowledged.
	for i := 0; i < 3; i++ {
		feedData(rx, uint32(i*4092), src[i*4092:(i+1)*4092])
	}
	if len(loop.ack) != 0 {
		t.Fatalf("ACKs = %d, want 0", len(loop.ack))
	}

	// The fourth crosses the threshold: exactly one ACK, for 4*4092.
	feedData(rx, 3*4092, src[3*4092:4*4092])
	if len(loop.ack) != 1 {
		t.Fatalf("ACKs = %d, want 1", len(loop.ack))
	}
	if next, _, ok := DecodeAck(loop.ack[0]); !ok || next != 4*4092 {
		t.Fatalf("ACK next = %d (ok %v), want %d", next, ok, 4*4092)
	}

	// The fifth adds nothing; the sixth completes and ACKs once more.
	feedData(rx, 4*4092, src[4*4092:5*4092])
	if len(loop.ack) != 1 {
		t.Fatalf("ACKs = %d, want 1", len(loop.ack))
	}
	feedData(rx, 5*4092, src[5*4092:6*4092])
	if len(loop.ack) != 2 {
		t.Fatalf("ACKs = %d, want 2", len(loop.ack))
	}
	if rx.State() != XferDone {
		t.Fatalf("state = %v, want Done", rx.State())
	}
	if next, _, ok := DecodeAck(loop.ack[len(loop.ack)-1]); !ok || next != size {
		t.Fatalf("final ACK next = %d (ok %v), want %d", next, ok, size)
	}
}

// TestDataAckCodec covers the u32-offset DATA and u32+u16 ACK payload encodings
// and the short-payload rejections, independent of the golden vectors.
func TestDataAckCodec(t *testing.T) {
	for _, off := range []uint32{0, 1, 4092, 12345, 4294967295} {
		body := []byte{0xAA, 0xBB}
		payload := EncodeData(off, body)
		if len(payload) != 4+len(body) {
			t.Fatalf("EncodeData(%d) length = %d", off, len(payload))
		}
		gotOff, gotBody, ok := DecodeData(payload)
		if !ok || gotOff != off || string(gotBody) != string(body) {
			t.Fatalf("DecodeData(EncodeData(%d)) = %d/%x/%v", off, gotOff, gotBody, ok)
		}
	}

	// Empty body still carries the offset.
	if p := EncodeData(7, nil); len(p) != 4 || p[0] != 7 {
		t.Fatalf("EncodeData(7, nil) = %x", p)
	}
	if _, _, ok := DecodeData([]byte{1, 2, 3}); ok {
		t.Fatal("DecodeData accepted a 3-byte payload")
	}

	if p := EncodeAck(4096, 16); len(p) != 6 || p[0] != 0 || p[1] != 0x10 || p[4] != 16 {
		t.Fatalf("EncodeAck(4096, 16) = %x", p)
	}
	if n, c, ok := DecodeAck(EncodeAck(4294967295, 65535)); !ok || n != 4294967295 || c != 65535 {
		t.Fatalf("DecodeAck(EncodeAck(max)) = %d/%d/%v", n, c, ok)
	}
	for _, n := range []int{0, 5, 7} {
		if _, _, ok := DecodeAck(make([]byte, n)); ok {
			t.Fatalf("DecodeAck accepted a %d-byte payload", n)
		}
	}
}
