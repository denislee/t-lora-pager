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
//   - The receiver only ACKs after DATA arrives: every `ackEvery` messages, on
//     completion, and immediately (a re-ACK of its current offset) when a DATA
//     offset is not the one it expects. A sender that gets an ACK with no
//     progress while it has bytes in flight therefore rewinds to that offset
//     (go-back-N) and resends from there.
//
// Neither Sender nor Receiver is safe for concurrent use: drive each from a
// single goroutine, exactly as the firmware drives it from the link task.

import "encoding/binary"

// Bulk-transfer constants (link_xfer.h).
const (
	// MaxDataChunk is the largest DATA chunk: MaxPayload minus the u32 offset.
	MaxDataChunk = 4092
	// InitialCredits is the implicit credit grant at the start of a transfer
	// (link_frame.h INITIAL_CREDITS).
	InitialCredits = 16
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
	credits uint16
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
	}
	return s.state
}

// OnAck feeds one ACK payload. A malformed payload, an ACK below the current
// acknowledged offset, or one beyond what was sent is ignored. It does not send;
// call Pump next.
func (s *Sender) OnAck(payload []byte) XferState {
	if s.state != XferActive {
		return s.state
	}

	next, credits, ok := DecodeAck(payload)
	if !ok {
		return s.state
	}
	// Below what we already know, or beyond what we have sent: ignore.
	if next < s.acked || next > s.next {
		return s.state
	}
	// No progress while bytes are in flight: rewind and resend (go-back-N).
	if next == s.acked && s.next > s.acked {
		s.next = s.acked
	}
	s.acked = next
	s.credits = credits
	if s.acked == s.size {
		s.state = XferDone
	}
	return s.state
}

// State reports the transfer state.
func (s *Sender) State() XferState { return s.state }

// Acked reports the highest acknowledged offset (bytes below it are safe).
func (s *Sender) Acked() uint32 { return s.acked }

// Sent reports the next offset to send — also the number of bytes handed to
// send so far, since DATA is always sent in order from the start offset.
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
		state:    state,
	}
}

func (r *Receiver) sendAck() error {
	return r.ack(EncodeAck(r.next, r.window))
}

// OnData feeds one DATA payload. A malformed payload is ignored. A wrong offset
// makes it re-ACK the current offset and drop the bytes. Bytes past size or a
// failed write / ACK fail the transfer. It returns the state afterwards.
func (r *Receiver) OnData(payload []byte) XferState {
	if r.state != XferActive {
		return r.state
	}

	offset, bytes, ok := DecodeData(payload)
	if !ok {
		return r.state // malformed: ignore
	}

	// Not the offset we are waiting for: re-ACK it and drop the bytes.
	if offset != r.next {
		if err := r.sendAck(); err != nil {
			r.state = XferFailed
		}
		return r.state
	}
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
