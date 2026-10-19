package pagerlink

// Bulk transfer (PROTOCOL.md §4). This file is a deliberate line-for-line port
// of the pager firmware engine in src/hal/link/link_xfer.{h,cpp} so all three
// implementations (firmware, iOS companion, this CLI) share identical
// flow-control semantics. Where the two disagree, link_xfer.cpp is the oracle.
//
// Flow control:
//   - Credits count DATA messages. The sender keeps at most `credits` DATA
//     messages past the last acknowledged offset; the implicit initial grant is
//     InitialCredits.
//   - ACK.next acknowledges every byte below it and its credits replace the
//     sender's window, counting from that offset.
//   - The receiver only ACKs after DATA arrives: every `ackEvery` messages and
//     on completion. A DATA offset that is not the one it expects gets at most
//     two ACKs per gap: one that tells the sender the offset if it has not seen
//     it, then the duplicate that triggers its one rewind — it does not re-ACK
//     again until in-order DATA has arrived. After completion it re-ACKs the
//     final offset for any further DATA, which covers a lost final ACK.
//   - An ACK with no progress while bytes are in flight rewinds the sender to
//     that offset (go-back-N), at most once per acknowledged offset: further
//     duplicate ACKs only update credits. An ACK above the current send point
//     (one that overtook a rewind) moves the send point forward.
//   - Retransmit timeout: an owner that has seen no ACK for RetransmitTimeout
//     while bytes are unacknowledged calls Retransmit and then Pump, which
//     rewinds and resends. This recovers a lost last DATA, re-ACK or final ACK.
//
// Neither Sender nor Receiver is safe for concurrent use: drive each from a
// single goroutine, exactly as the firmware drives it from the link task.

import (
	"encoding/binary"
	"time"
)

// Bulk-transfer constants (link_xfer.h).
const (
	// MaxDataChunk is the largest DATA chunk: MaxPayload minus the u32 offset.
	MaxDataChunk = 4092
	// InitialCredits is the implicit credit grant at the start of a transfer
	// (link_frame.h INITIAL_CREDITS).
	InitialCredits = 16
	// RetransmitTimeout is the bulk sender's retransmit timeout: no ACK for
	// this long with bytes unacknowledged rewinds the sender to the
	// acknowledged offset and resends (PROTOCOL.md §4,
	// link_frame.h XFER_RETRANSMIT_MS).
	RetransmitTimeout = 2 * time.Second
)

// EncodeData builds a DATA payload: the u32 little-endian source offset followed
// by the chunk bytes (link_frame.cpp encode_data).
func EncodeData(offset uint32, b []byte) []byte {
	p := make([]byte, 4+len(b))
	binary.LittleEndian.PutUint32(p[:4], offset)
	copy(p[4:], b)
	return p
}

// DecodeData parses a DATA payload, returning the source offset and the chunk
// bytes (a sub-slice of p). It reports false when p is shorter than the u32
// offset (link_frame.cpp decode_data).
func DecodeData(p []byte) (uint32, []byte, bool) {
	if len(p) < 4 {
		return 0, nil, false
	}
	return binary.LittleEndian.Uint32(p[:4]), p[4:], true
}

// EncodeAck builds an ACK payload: the u32 little-endian next expected offset
// followed by the u16 credit grant (link_frame.cpp encode_ack).
func EncodeAck(next uint32, credits uint16) []byte {
	p := make([]byte, 6)
	binary.LittleEndian.PutUint32(p[:4], next)
	binary.LittleEndian.PutUint16(p[4:6], credits)
	return p
}

// DecodeAck parses an ACK payload. It reports false unless p is exactly six
// bytes (link_frame.cpp decode_ack).
func DecodeAck(p []byte) (uint32, uint16, bool) {
	if len(p) != 6 {
		return 0, 0, false
	}
	return binary.LittleEndian.Uint32(p[:4]), binary.LittleEndian.Uint16(p[4:6]), true
}

// XferState is the state of a bulk transfer, mirroring link_xfer.h XferState.
type XferState int

const (
	XferActive XferState = iota // still moving bytes
	XferDone                    // every byte acknowledged / received
	XferFailed                  // a callback failed, or the peer overran the transfer
)

// Sender sends size bytes as DATA messages under the receiver's credit window.
// It mirrors link_xfer.h XferSender.
type Sender struct {
	size    uint32
	chunk   int
	read    func(off uint32, n int) ([]byte, error)
	send    func(payload []byte) error
	acked   uint32
	next    uint32
	high    uint32 // highest offset ever handed to send
	credits uint16
	rewound bool // rewound since acked last advanced
	state   XferState
}

// NewSender starts a transfer of size bytes, chunk bytes per DATA message
// (clamped to 1..MaxDataChunk), with start (clamped to size) the first offset to
// send — a resumed read. A zero-length remainder is Done immediately, and read
// / send are not called then. A read or send error fails the transfer.
func NewSender(size uint32, chunk int, start uint32,
	read func(off uint32, n int) ([]byte, error),
	send func(payload []byte) error) *Sender {
	if chunk < 1 {
		chunk = 1
	} else if chunk > MaxDataChunk {
		chunk = MaxDataChunk
	}
	acked := start
	if acked > size {
		acked = size
	}
	state := XferActive
	if acked >= size {
		state = XferDone
	}
	return &Sender{
		size:    size,
		chunk:   chunk,
		read:    read,
		send:    send,
		acked:   acked,
		next:    acked,
		high:    acked,
		credits: InitialCredits,
		state:   state,
	}
}

// Pump sends DATA while the window allows and returns the state afterwards.
func (s *Sender) Pump() XferState {
	if s.state != XferActive {
		return s.state
	}

	for s.next < s.size {
		// In-flight DATA messages past the acknowledged offset.
		inflight := (s.next - s.acked + uint32(s.chunk) - 1) / uint32(s.chunk)
		if inflight >= uint32(s.credits) {
			break
		}

		n := s.chunk
		if rem := int(s.size - s.next); n > rem {
			n = rem
		}
		buf, err := s.read(s.next, n)
		if err != nil {
			s.state = XferFailed
			return s.state
		}
		if len(buf) > n {
			buf = buf[:n]
		}
		if err := s.send(EncodeData(s.next, buf)); err != nil {
			s.state = XferFailed
			return s.state
		}
		s.next += uint32(n)
		if s.next > s.high {
			s.high = s.next
		}
	}
	return s.state
}

// OnAck feeds one ACK payload. A malformed payload, an ACK below the current
// acknowledged offset, or one beyond what was ever sent is ignored. An ACK with
// no progress rewinds the send point at most once per acknowledged offset;
// further duplicate ACKs only update credits. It does not send; call Pump next.
func (s *Sender) OnAck(payload []byte) XferState {
	if s.state != XferActive {
		return s.state
	}

	next, credits, ok := DecodeAck(payload)
	if !ok {
		return s.state
	}
	// Below what we already know, or beyond what we ever sent: ignore.
	if next < s.acked || next > s.high {
		return s.state
	}
	if next == s.acked {
		// No progress while bytes are in flight: rewind and resend (go-back-N),
		// once per acknowledged offset.
		if s.next > s.acked && !s.rewound {
			s.next = s.acked
			s.rewound = true
		}
	} else {
		// Progress. Clear the rewind latch, and let an ACK that overtook a
		// rewind move the send point forward.
		s.rewound = false
		if s.next < next {
			s.next = next
		}
	}
	s.acked = next
	s.credits = credits
	if s.acked == s.size {
		s.state = XferDone
	}
	return s.state
}

// Retransmit is the retransmit-timeout hook: the owner calls it when no ACK has
// arrived for RetransmitTimeout while bytes are unacknowledged, then calls
// Pump; it rewinds to the acknowledged offset and reports whether it did.
func (s *Sender) Retransmit() bool {
	if s.state != XferActive || s.high == s.acked {
		return false
	}
	s.next = s.acked
	s.rewound = true
	return true
}

// State reports the transfer state.
func (s *Sender) State() XferState { return s.state }

// Acked reports the highest acknowledged offset (bytes below it are safe).
func (s *Sender) Acked() uint32 { return s.acked }

// Sent reports the next offset to send (it moves back on a rewind).
func (s *Sender) Sent() uint32 { return s.next }

// Receiver receives size bytes of DATA in order and acknowledges them. It
// mirrors link_xfer.h XferReceiver.
type Receiver struct {
	size     uint32
	window   uint16
	ackEvery int
	write    func(off uint32, b []byte) error
	ack      func(payload []byte) error
	sinceAck int
	next     uint32
	lastAck  uint32 // offset of the last ACK sent (starts at start)
	gapAcked bool   // re-ACKed the current offset for the open gap
	state    XferState
}

// NewReceiver receives size bytes, granting window credits in every ACK and
// ACKing every ackEvery (clamped to >= 1) in-order DATA messages, with start the
// first expected offset. A zero-length transfer is Done immediately.
func NewReceiver(size uint32, window uint16, ackEvery int, start uint32,
	write func(off uint32, b []byte) error,
	ack func(payload []byte) error) *Receiver {
	if ackEvery < 1 {
		ackEvery = 1
	}
	state := XferActive
	if start >= size {
		state = XferDone
	}
	return &Receiver{
		size:     size,
		window:   window,
		ackEvery: ackEvery,
		write:    write,
		ack:      ack,
		next:     start,
		lastAck:  start, // the sender starts with acked == start
		state:    state,
	}
}

func (r *Receiver) sendAck() error {
	if err := r.ack(EncodeAck(r.next, r.window)); err != nil {
		return err
	}
	r.lastAck = r.next
	return nil
}

// OnData feeds one DATA payload. A malformed payload is ignored. A wrong offset
// makes it send at most two ACKs for the gap — one that tells the sender the
// offset if it has not seen it, then the duplicate that triggers its one
// rewind — and drop the bytes; it does not ACK that gap again. Once Done it
// re-ACKs the final offset for any well-formed DATA, so a sender that lost the
// completion ACK can recover. Bytes past size or a failed write / ACK fail the
// transfer. It returns the state afterwards.
func (r *Receiver) OnData(payload []byte) XferState {
	if r.state == XferDone {
		// The sender may be retransmitting because our final ACK was lost:
		// re-ACK the completed offset. A failed ACK leaves us Done.
		if _, _, ok := DecodeData(payload); ok {
			_ = r.sendAck()
		}
		return r.state
	}
	if r.state != XferActive {
		return r.state // Failed: unchanged
	}

	offset, bytes, ok := DecodeData(payload)
	if !ok {
		return r.state // malformed: ignore
	}

	// Not the offset we are waiting for: at most two ACKs for this gap, then
	// drop the bytes.
	if offset != r.next {
		if !r.gapAcked {
			r.gapAcked = true
			// First, only if the sender has not seen this offset yet: it is
			// progress for the sender, which learns `next`. Without it a later
			// loss would look like progress and the sender would never rewind.
			if r.lastAck != r.next {
				if err := r.sendAck(); err != nil {
					r.state = XferFailed
					return r.state
				}
			}
			// Then the duplicate that triggers the sender's one rewind.
			if err := r.sendAck(); err != nil {
				r.state = XferFailed
			}
		}
		return r.state
	}
	r.gapAcked = false
	n := len(bytes)
	if uint32(n) > r.size-r.next {
		r.state = XferFailed
		return r.state
	}
	if n > 0 {
		if err := r.write(offset, bytes); err != nil {
			r.state = XferFailed
			return r.state
		}
	}
	r.next += uint32(n)
	r.sinceAck++

	if r.next == r.size {
		r.state = XferDone
		if err := r.sendAck(); err != nil {
			r.state = XferFailed
		}
		return r.state
	}
	if r.sinceAck >= r.ackEvery {
		r.sinceAck = 0
		if err := r.sendAck(); err != nil {
			r.state = XferFailed
		}
	}
	return r.state
}

// State reports the transfer state.
func (r *Receiver) State() XferState { return r.state }

// Received reports the next expected offset — the number of bytes written so far.
func (r *Receiver) Received() uint32 { return r.next }
