// Package pagerlink implements the Go side of the PagerLink v1 protocol
// described in protocol/PROTOCOL.md: the §2 packet framing and reassembly, the
// §3 message header, and the §5 TCP record transport used to talk to a pager
// (or its emulator). It is stdlib-only on purpose — the hub ships as a single
// static binary and the codec is small enough that a dependency would cost
// more than it saves.
//
// The pager firmware (src/hal/link/) and the iOS companion (ios/PagerKit) are
// the other two implementations of this spec, and all three are tested against
// the golden vectors in protocol/vectors/*.json. This file must therefore stay
// byte-for-byte identical to protocol/tools/mkvectors.py, which is the oracle.
package pagerlink

import (
	"encoding/binary"
	"fmt"
)

// Wire constants (PROTOCOL.md §2-§3). Every multi-byte integer is
// little-endian.
const (
	Version    = 1                      // ver byte every message starts with
	HeaderLen  = 6                      // u8 ver, u8 type, u16 id, u16 len
	MaxPayload = 4096                   // len is a u16 but is capped at 4 KiB
	MaxMessage = HeaderLen + MaxPayload // largest reassembled message (4102)
	StartFlag  = 0x80                   // bit7: first packet of a message
	EndFlag    = 0x40                   // bit6: last packet of a message
	SeqMask    = 0x3F                   // bits5..0: rolling packet sequence

	// TCPPacketSize is the fixed packet size P over the TCP transport (§5),
	// equivalent to ATT_MTU 247. Each packet carries P-1 message bytes plus
	// the flags byte.
	TCPPacketSize = 244
)

// MsgType is the message-layer type byte (§3). Values above Ack are malformed.
type MsgType uint8

const (
	Req  MsgType = 0 // request
	Res  MsgType = 1 // response
	Err  MsgType = 2 // error
	Evt  MsgType = 3 // event
	Data MsgType = 4 // bulk-transfer data
	Ack  MsgType = 5 // bulk-transfer credit
)

// Message is one PagerLink message: a 6-byte header plus an opaque payload.
// The payload's meaning depends on Type (JSON for REQ/RES/ERR/EVT, a u32
// offset + bytes for DATA, u32 next_expected_offset + u16 credits for ACK).
type Message struct {
	Type    MsgType
	ID      uint16
	Payload []byte
}

// DecodeError reports why a byte string could not be decoded as a Message.
// Reason is the short label used by the golden vectors.
type DecodeError struct {
	Reason string
}

func (e *DecodeError) Error() string { return "pagerlink: malformed message: " + e.Reason }

// Encode serialises m to its wire form. It refuses a payload longer than
// MaxPayload or a type above Ack rather than emitting a message the peer would
// have to drop.
func (m Message) Encode() ([]byte, error) {
	if len(m.Payload) > MaxPayload {
		return nil, fmt.Errorf("pagerlink: payload %d bytes exceeds %d", len(m.Payload), MaxPayload)
	}
	if m.Type > Ack {
		return nil, fmt.Errorf("pagerlink: invalid message type %d", m.Type)
	}
	b := make([]byte, HeaderLen+len(m.Payload))
	b[0] = Version
	b[1] = byte(m.Type)
	binary.LittleEndian.PutUint16(b[2:4], m.ID)
	binary.LittleEndian.PutUint16(b[4:6], uint16(len(m.Payload)))
	copy(b[HeaderLen:], m.Payload)
	return b, nil
}

// Decode parses a reassembled message. The error is always a *DecodeError and
// the checks run in the order the firmware applies them (§3): length first,
// then version, type, payload cap, and finally the declared-vs-actual length
// match — so a truncated header reports "short" and a bad type inside an
// otherwise oversized message reports "type", exactly as the vectors expect.
func Decode(b []byte) (Message, error) {
	if len(b) < HeaderLen {
		return Message{}, &DecodeError{Reason: "short"}
	}
	if b[0] != Version {
		return Message{}, &DecodeError{Reason: "version"}
	}
	if b[1] > byte(Ack) {
		return Message{}, &DecodeError{Reason: "type"}
	}
	n := int(binary.LittleEndian.Uint16(b[4:6]))
	if n > MaxPayload {
		return Message{}, &DecodeError{Reason: "oversize"}
	}
	if HeaderLen+n != len(b) {
		return Message{}, &DecodeError{Reason: "length"}
	}
	payload := make([]byte, n)
	copy(payload, b[HeaderLen:])
	return Message{
		Type:    MsgType(b[1]),
		ID:      binary.LittleEndian.Uint16(b[2:4]),
		Payload: payload,
	}, nil
}

// Segmenter cuts messages into packets, owning the send-side sequence counter
// for one direction of one link (§2). Seq starts at 0 on a fresh connection and
// advances by one (mod 64) per packet across message boundaries; Segment
// updates it in place so back-to-back calls stay contiguous.
type Segmenter struct {
	Seq uint8
}

// Segment splits msg into packets of at most packetSize-1 message bytes each.
// The first packet carries START, the last END (a one-packet message carries
// both). packetSize below 2 yields no packets. Seq is advanced to the next
// unused value.
func (s *Segmenter) Segment(msg []byte, packetSize int) [][]byte {
	room := packetSize - 1
	if room < 1 {
		return nil
	}
	var out [][]byte
	for off := 0; off < len(msg); off += room {
		flags := s.Seq & SeqMask
		if off == 0 {
			flags |= StartFlag
		}
		end := off + room
		if end >= len(msg) {
			end = len(msg)
			flags |= EndFlag
		}
		pkt := make([]byte, 1+end-off)
		pkt[0] = flags
		copy(pkt[1:], msg[off:end])
		out = append(out, pkt)
		s.Seq = (s.Seq + 1) & SeqMask
	}
	return out
}

// Event is one thing the Reassembler observed while feeding a packet: either a
// delivered Message (Reset empty) or a Reset with its reason. Exactly one of
// the two fields is non-empty, mirroring the golden vectors' event objects.
type Event struct {
	Message []byte // complete reassembled message
	Reset   string // "empty", "seq_gap", "unexpected_start", "unexpected_continue" or "oversize"
}

// Reassembler rebuilds messages from received packets for one direction of one
// link (§2). It is not safe for concurrent use: feed it from a single reader.
//
// State mirrors mkvectors.py's Reassembler exactly, including the discarding
// state entered after a seq_gap or an oversize reassembly, in which
// continue-only packets are dropped silently so that one fault produces one
// reset rather than one per remaining packet.
type Reassembler struct {
	expected   uint8
	partial    []byte
	discarding bool
}

// Feed consumes one packet and returns the events it produced (an empty packet
// makes none, a mid-message start can make two). A reset never fails an
// outstanding request — the RPC layer times it out instead.
func (r *Reassembler) Feed(pkt []byte) []Event {
	if len(pkt) < 2 {
		return []Event{{Reset: "empty"}}
	}
	flags := pkt[0]
	data := pkt[1:]
	seq := flags & SeqMask

	var events []Event
	if seq != r.expected {
		events = append(events, Event{Reset: "seq_gap"})
		r.partial = nil
		r.discarding = true
	}
	r.expected = (seq + 1) & SeqMask

	if flags&StartFlag != 0 {
		r.discarding = false
		if r.partial != nil {
			events = append(events, Event{Reset: "unexpected_start"})
		}
		r.partial = []byte{}
	} else if r.partial == nil {
		if !r.discarding {
			events = append(events, Event{Reset: "unexpected_continue"})
		}
		return events
	}

	if len(r.partial)+len(data) > MaxMessage {
		events = append(events, Event{Reset: "oversize"})
		r.partial = nil
		r.discarding = true
		return events
	}
	r.partial = append(r.partial, data...)
	if flags&EndFlag != 0 {
		events = append(events, Event{Message: r.partial})
		r.partial = nil
	}
	return events
}
