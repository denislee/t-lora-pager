package pagerlink

import (
	"errors"
	"testing"
)

// These tests mirror test/test_link_xfer/test_main.cpp: a Sender and a Receiver
// are wired together over a pair of payload queues so a test can deliver, drop
// or reorder DATA and ACK messages by hand, and a loopback helper carries the
// whole transfer to completion. The lossy harness below adds a drop rule per
// direction and simulates the owner's retransmit timer when both queues run dry.

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

// lossyLoop is the lossy loopback harness: DATA payloads carry sender ->
// receiver and ACK payloads receiver -> sender, and a drop rule can discard the
// k-th (0-based) send in either direction. Every send is counted, including the
// dropped ones, so a test can bound the total DATA traffic.
type lossyLoop struct {
	data      [][]byte
	ack       [][]byte
	dataSends int
	ackSends  int
	dropData  func(i int, payload []byte) bool
	dropAck   func(i int, payload []byte) bool
}

func (l *lossyLoop) sendData(payload []byte) error {
	i := l.dataSends
	l.dataSends++
	if l.dropData != nil && l.dropData(i, payload) {
		return nil
	}
	l.data = append(l.data, append([]byte(nil), payload...))
	return nil
}

func (l *lossyLoop) sendAck(payload []byte) error {
	i := l.ackSends
	l.ackSends++
	if l.dropAck != nil && l.dropAck(i, payload) {
		return nil
	}
	l.ack = append(l.ack, append([]byte(nil), payload...))
	return nil
}

func (l *lossyLoop) deliverAllData(rx *Receiver) {
	for len(l.data) > 0 {
		p := l.data[0]
		l.data = l.data[1:]
		rx.OnData(p)
	}
}

func (l *lossyLoop) deliverAllAcks(tx *Sender) {
	for len(l.ack) > 0 {
		p := l.ack[0]
		l.ack = l.ack[1:]
		tx.OnAck(p)
	}
}

// newLossyXfer wires a size-byte transfer through loop, writing received bytes
// into dst. It returns the sender, the receiver and the source pattern.
func newLossyXfer(loop *lossyLoop, size int, dst []byte) (*Sender, *Receiver, []byte) {
	src := makeSource(size)
	tx := NewSender(uint32(size), MaxDataChunk, 0, readFrom(src), loop.sendData)
	rx := NewReceiver(uint32(size), InitialCredits, 4, 0,
		func(off uint32, b []byte) error {
			copy(dst[off:], b)
			return nil
		},
		loop.sendAck)
	return tx, rx, src
}

// runLossy drives a transfer to completion through loop and returns how many
// times the retransmit timer fired. When both queues run dry with the sender
// still Active it fires the timer exactly as SendBytes does: Retransmit, then
// Pump.
func runLossy(t *testing.T, loop *lossyLoop, tx *Sender, rx *Receiver) int {
	t.Helper()

	firings := 0
	tx.Pump()
	for step := 0; step < 100000; step++ {
		if tx.State() == XferDone && rx.State() == XferDone {
			return firings
		}
		loop.deliverAllData(rx)
		loop.deliverAllAcks(tx)
		tx.Pump()
		if len(loop.data) == 0 && len(loop.ack) == 0 && tx.State() != XferDone {
			firings++
			tx.Retransmit()
			tx.Pump()
		}
	}
	t.Fatalf("transfer stalled: tx=%v rx=%v sent=%d acked=%d",
		tx.State(), rx.State(), tx.Sent(), tx.Acked())
	return firings
}

// runLossyInterleaved drives a transfer through loop one message at a time and
// returns how many times the retransmit timer fired. Unlike runLossy's batched
// delivery, it alternates directions: after one DATA it feeds every pending ACK
// to the sender, pumping after each, and only when both queues run dry with the
// sender still Active does it fire the timer exactly as SendBytes does:
// Retransmit, then Pump. It is bounded to 200000 steps.
func runLossyInterleaved(t *testing.T, loop *lossyLoop, tx *Sender, rx *Receiver) int {
	t.Helper()

	firings := 0
	tx.Pump()
	for step := 0; step < 200000; step++ {
		if tx.State() == XferDone && rx.State() == XferDone {
			return firings
		}
		if len(loop.data) > 0 {
			p := loop.data[0]
			loop.data = loop.data[1:]
			rx.OnData(p)
			for len(loop.ack) > 0 {
				p := loop.ack[0]
				loop.ack = loop.ack[1:]
				tx.OnAck(p)
				tx.Pump()
			}
			continue
		}
		if len(loop.ack) > 0 {
			for len(loop.ack) > 0 {
				p := loop.ack[0]
				loop.ack = loop.ack[1:]
				tx.OnAck(p)
				tx.Pump()
			}
			continue
		}
		if tx.State() != XferDone {
			firings++
			tx.Retransmit()
			tx.Pump()
		}
	}
	t.Fatalf("interleaved transfer stalled after 200000 steps: tx=%v rx=%v data sends=%d ack sends=%d timer firings=%d",
		tx.State(), rx.State(), loop.dataSends, loop.ackSends, firings)
	return firings
}

// assertReceived checks the destination bytes against the source pattern.
func assertReceived(t *testing.T, src, dst []byte) {
	t.Helper()
	if len(src) != len(dst) {
		t.Fatalf("lengths = %d / %d, want equal", len(src), len(dst))
	}
	for i := range src {
		if src[i] != dst[i] {
			t.Fatalf("byte %d = %#x, want %#x", i, dst[i], src[i])
		}
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
	// wrong offset, but the receiver re-ACKs offset 0 only once (one re-ACK per
	// gap) without writing.
	loop.data = loop.data[1:]
	loop.deliverAllData(rx)
	if rx.Received() != 0 {
		t.Fatalf("received = %d, want 0", rx.Received())
	}

	// Feeding the re-ACK rewinds the sender to offset 0.
	if len(loop.ack) != 1 {
		t.Fatalf("re-ACKs = %d, want 1", len(loop.ack))
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

// --- lossy harness: retransmit timeout, one rewind / two ACKs per gap -------

// TestLossyDropMiddleData drops one middle DATA send of a 102400-byte
// transfer. The receiver sends at most two ACKs for the gap — the offset if the
// sender has not seen it, then the duplicate — so the duplicate-ACK rewind
// fires and no timer is needed. Both a send just reported by a regular ACK (the
// 5th) and one it never reported (the 6th) are covered.
func TestLossyDropMiddleData(t *testing.T) {
	for _, c := range []struct {
		name string
		drop int // 0-based DATA send index
	}{
		{"drop-sixth-send", 5},
		{"drop-fifth-send", 4},
	} {
		t.Run(c.name, func(t *testing.T) {
			const size = 102400
			dst := make([]byte, size)
			loop := &lossyLoop{}
			loop.dropData = func(i int, _ []byte) bool { return i == c.drop }
			tx, rx, src := newLossyXfer(loop, size, dst)

			firings := runLossy(t, loop, tx, rx)

			if tx.State() != XferDone || rx.State() != XferDone {
				t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
			}
			if loop.dataSends > 52 {
				t.Fatalf("DATA sends = %d, want <= 52", loop.dataSends)
			}
			if firings != 0 {
				t.Fatalf("timer firings = %d, want 0", firings)
			}
			assertReceived(t, src, dst)
		})
	}
}

// TestLossyDropLastData drops the last of the 3 DATA sends in an 8192-byte
// transfer: only the retransmit timer can recover it.
func TestLossyDropLastData(t *testing.T) {
	const size = 8192 // 3 DATA with the 4092-byte chunk
	dst := make([]byte, size)
	loop := &lossyLoop{}
	loop.dropData = func(i int, _ []byte) bool { return i == 2 } // the last DATA
	tx, rx, src := newLossyXfer(loop, size, dst)

	firings := runLossy(t, loop, tx, rx)

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if firings != 1 {
		t.Fatalf("timer firings = %d, want 1", firings)
	}
	assertReceived(t, src, dst)
}

// TestLossyDropFirstData drops DATA #0 of a 262144-byte transfer. The very
// first re-ACK is a duplicate, so the sender rewinds without the timer.
func TestLossyDropFirstData(t *testing.T) {
	const size = 262144
	dst := make([]byte, size)
	loop := &lossyLoop{}
	loop.dropData = func(i int, _ []byte) bool { return i == 0 } // DATA #0
	tx, rx, src := newLossyXfer(loop, size, dst)

	firings := runLossy(t, loop, tx, rx)

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if loop.dataSends > 130 {
		t.Fatalf("DATA sends = %d, want <= 130", loop.dataSends)
	}
	if firings > 1 {
		t.Fatalf("timer firings = %d, want <= 1", firings)
	}
	assertReceived(t, src, dst)
}

// TestLossyDropFinalAck drops the receiver's completion ACK of a 102400-byte
// transfer. The receiver is Done; the sender recovers on its retransmit timer,
// and the Done receiver re-ACKs the final offset.
func TestLossyDropFinalAck(t *testing.T) {
	const size = 102400
	dst := make([]byte, size)
	loop := &lossyLoop{}
	dropped := false
	loop.dropAck = func(_ int, payload []byte) bool {
		n, _, ok := DecodeAck(payload)
		if ok && n == size && !dropped {
			dropped = true
			return true
		}
		return false
	}
	tx, rx, src := newLossyXfer(loop, size, dst)

	firings := runLossy(t, loop, tx, rx)

	if rx.State() != XferDone {
		t.Fatalf("receiver state = %v, want Done", rx.State())
	}
	if tx.State() != XferDone {
		t.Fatalf("sender state = %v, want Done", tx.State())
	}
	if firings != 1 {
		t.Fatalf("timer firings = %d, want exactly 1", firings)
	}
	assertReceived(t, src, dst)
}

// TestLossyLossless pins the lossless baseline: 102400 B is 26 DATA sends and
// needs no timer at all.
func TestLossyLossless(t *testing.T) {
	const size = 102400
	dst := make([]byte, size)
	loop := &lossyLoop{}
	tx, rx, src := newLossyXfer(loop, size, dst)

	firings := runLossy(t, loop, tx, rx)

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if loop.dataSends != 26 {
		t.Fatalf("DATA sends = %d, want 26", loop.dataSends)
	}
	if firings != 0 {
		t.Fatalf("timer firings = %d, want 0", firings)
	}
	assertReceived(t, src, dst)
}

// TestInterleavedDropMidData drops the 6th DATA send (index 5) of a 102400-byte
// transfer and delivers one message at a time, ACK-first in the sender's
// direction. The duplicate-ACK rewind recovers the gap without the timer.
func TestInterleavedDropMidData(t *testing.T) {
	const size = 102400
	dst := make([]byte, size)
	loop := &lossyLoop{}
	loop.dropData = func(i int, _ []byte) bool { return i == 5 } // DATA #5
	tx, rx, src := newLossyXfer(loop, size, dst)

	firings := runLossyInterleaved(t, loop, tx, rx)

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if loop.dataSends > 52 {
		t.Fatalf("DATA sends = %d, want <= 52", loop.dataSends)
	}
	if firings != 0 {
		t.Fatalf("timer firings = %d, want 0", firings)
	}
	assertReceived(t, src, dst)
}

// TestInterleavedDropFirstData drops DATA #0 of a 262144-byte transfer and
// delivers one message at a time. The very first re-ACK is a duplicate, so the
// sender rewinds without waiting for the timer.
func TestInterleavedDropFirstData(t *testing.T) {
	const size = 262144
	dst := make([]byte, size)
	loop := &lossyLoop{}
	loop.dropData = func(i int, _ []byte) bool { return i == 0 } // DATA #0
	tx, rx, src := newLossyXfer(loop, size, dst)

	firings := runLossyInterleaved(t, loop, tx, rx)

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if loop.dataSends > 130 {
		t.Fatalf("DATA sends = %d, want <= 130", loop.dataSends)
	}
	if firings > 1 {
		t.Fatalf("timer firings = %d, want <= 1", firings)
	}
	assertReceived(t, src, dst)
}

// TestInterleavedLossless pins the interleaved lossless baseline: 102400 B is
// 26 DATA sends and needs no timer at all.
func TestInterleavedLossless(t *testing.T) {
	const size = 102400
	dst := make([]byte, size)
	loop := &lossyLoop{}
	tx, rx, src := newLossyXfer(loop, size, dst)

	firings := runLossyInterleaved(t, loop, tx, rx)

	if tx.State() != XferDone || rx.State() != XferDone {
		t.Fatalf("states = %v / %v, want Done/Done", tx.State(), rx.State())
	}
	if loop.dataSends != 26 {
		t.Fatalf("DATA sends = %d, want 26", loop.dataSends)
	}
	if firings != 0 {
		t.Fatalf("timer firings = %d, want 0", firings)
	}
	assertReceived(t, src, dst)
}

// TestSenderRewindsOncePerAckedOffset feeds two identical no-progress ACKs:
// only the first rewinds the send point.
func TestSenderRewindsOncePerAckedOffset(t *testing.T) {
	const size = 20 * MaxDataChunk
	src := makeSource(size)
	loop := &xferLoop{}
	tx := NewSender(uint32(size), MaxDataChunk, 0, readFrom(src), loop.sendData)

	tx.Pump()
	if tx.Sent() == 0 {
		t.Fatal("sender produced no DATA")
	}

	tx.OnAck(EncodeAck(0, InitialCredits))
	if tx.Sent() != 0 || tx.Acked() != 0 {
		t.Fatalf("after first duplicate ACK: sent/acked = %d/%d, want 0/0",
			tx.Sent(), tx.Acked())
	}

	tx.Pump()
	if tx.Sent() == 0 {
		t.Fatal("pump after the rewind sent nothing")
	}

	// The second identical ACK makes no progress, but the offset was already
	// rewound once: the send point must not move back again.
	tx.OnAck(EncodeAck(0, InitialCredits))
	if tx.Sent() <= tx.Acked() {
		t.Fatalf("after second duplicate ACK: sent/acked = %d/%d, want sent > acked",
			tx.Sent(), tx.Acked())
	}
}

// TestReceiverReAcksOncePerGap feeds several wrong-offset DATA messages for one
// gap: a gap the sender already knows costs one duplicate ACK, a gap it has not
// seen costs two (the offset, then the duplicate). In-order DATA re-arms the
// gap rule.
func TestReceiverReAcksOncePerGap(t *testing.T) {
	const size = 10 * MaxDataChunk
	src := makeSource(size)
	loop := &xferLoop{}
	rx := NewReceiver(uint32(size), InitialCredits, 4, 0,
		func(uint32, []byte) error { return nil },
		loop.sendAck)

	for i := 0; i < 3; i++ {
		feedData(rx, MaxDataChunk, make([]byte, MaxDataChunk)) // expects 0
	}
	if len(loop.ack) != 1 {
		t.Fatalf("ACKs after 3 wrong-offset DATA = %d, want 1", len(loop.ack))
	}
	if rx.lastAck != 0 || rx.next != 0 {
		t.Fatalf("last_ack/next = %d/%d, want 0/0", rx.lastAck, rx.next)
	}

	// In-order DATA is accepted and does not ACK below ack_every.
	feedData(rx, 0, src[:MaxDataChunk])
	if rx.Received() != MaxDataChunk {
		t.Fatalf("received = %d, want %d", rx.Received(), MaxDataChunk)
	}
	if len(loop.ack) != 1 {
		t.Fatalf("ACKs after in-order DATA = %d, want 1", len(loop.ack))
	}

	// A new gap at an offset the sender has not seen re-ACKs twice: the
	// progress ACK, then the duplicate.
	feedData(rx, 2*MaxDataChunk, make([]byte, MaxDataChunk))
	if len(loop.ack) != 3 {
		t.Fatalf("ACKs after the next gap = %d, want 3", len(loop.ack))
	}
	for _, p := range loop.ack[1:] {
		if next, _, ok := DecodeAck(p); !ok || next != MaxDataChunk {
			t.Fatalf("gap ACK next = %d (ok %v), want %d", next, ok, MaxDataChunk)
		}
	}
}

// TestSenderRetransmit checks the owner-driven retransmit hook: nothing to do
// when everything is acknowledged, a rewind to acked when bytes are in flight.
func TestSenderRetransmit(t *testing.T) {
	const size = 4 * MaxDataChunk
	src := makeSource(size)
	loop := &xferLoop{}

	// A zero-size transfer is Done immediately and has nothing unacknowledged.
	zero := NewSender(0, MaxDataChunk, 0,
		func(uint32, int) ([]byte, error) { return nil, nil },
		loop.sendData)
	if zero.State() != XferDone {
		t.Fatalf("zero-size state = %v, want Done", zero.State())
	}
	if zero.Retransmit() {
		t.Fatal("Retransmit on a zero-size Done sender returned true")
	}

	// Bytes in flight: rewind to the acknowledged offset.
	tx := NewSender(uint32(size), MaxDataChunk, 0, readFrom(src), loop.sendData)
	tx.Pump()
	if !tx.Retransmit() {
		t.Fatal("Retransmit with bytes in flight returned false")
	}
	if tx.Sent() != tx.Acked() {
		t.Fatalf("after Retransmit: sent/acked = %d/%d, want equal", tx.Sent(), tx.Acked())
	}

	// After Done there is nothing left to retransmit.
	done := NewSender(uint32(size), MaxDataChunk, 0, readFrom(src), loop.sendData)
	done.Pump()
	done.OnAck(EncodeAck(uint32(size), InitialCredits))
	if done.State() != XferDone {
		t.Fatalf("state = %v, want Done", done.State())
	}
	if done.Retransmit() {
		t.Fatal("Retransmit after Done returned true")
	}
}
