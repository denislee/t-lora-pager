package pagerlink

// wire is the test side of the §5 record transport, shared by the fake peers
// in these tests (fakePeer, fakePager, fakeServer): the package's own
// Segmenter/Reassembler over a conn, with the same u16 little-endian record
// framing the Client uses.

import (
	"encoding/binary"
	"io"
	"net"
)

type wire struct {
	conn net.Conn
	seg  Segmenter
	re   Reassembler
}

// readLoop reads records until the conn fails, handing every decoded message
// to fn on the calling goroutine. Reassembly resets and messages that do not
// decode are skipped.
func (w *wire) readLoop(fn func(Message)) {
	var hdr [2]byte
	for {
		if _, err := io.ReadFull(w.conn, hdr[:]); err != nil {
			return
		}
		pkt := make([]byte, int(binary.LittleEndian.Uint16(hdr[:])))
		if _, err := io.ReadFull(w.conn, pkt); err != nil {
			return
		}
		for _, ev := range w.re.Feed(pkt) {
			if ev.Reset != "" {
				continue
			}
			msg, err := Decode(ev.Message)
			if err != nil {
				continue
			}
			fn(msg)
		}
	}
}

// writeMsg encodes one message and writes every packet as a record. Only an
// encode failure is returned: a write error means the conn is gone, and the
// read side notices that.
func (w *wire) writeMsg(t MsgType, id uint16, payload []byte) error {
	raw, err := Message{Type: t, ID: id, Payload: payload}.Encode()
	if err != nil {
		return err
	}
	for _, pkt := range w.seg.Segment(raw, TCPPacketSize) {
		rec := make([]byte, 2+len(pkt))
		binary.LittleEndian.PutUint16(rec, uint16(len(pkt)))
		copy(rec[2:], pkt)
		if _, err := w.conn.Write(rec); err != nil {
			return nil
		}
	}
	return nil
}
