// This file implements the §5 TCP transport: one client at a time, each packet
// preceded by a u16 little-endian length. Everything here is synchronous and
// stdlib-only; the Client is the phone side of the link, so it allocates even
// request ids and surfaces pager EVT messages on Events().
package pagerlink

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

var (
	// ErrTimeout is returned by Call when a request gets no RES/ERR in time.
	ErrTimeout = errors.New("timeout")
	// ErrDisconnected is returned by Call when the link drops or is closed
	// while a request is outstanding.
	ErrDisconnected = errors.New("disconnected")
)

// RPCError is the decoded ERR payload (§3.4): a stable code plus human text.
// An unknown code is treated as "internal", per the spec.
type RPCError struct {
	Code string
	Msg  string
}

func (e *RPCError) Error() string { return e.Code + ": " + e.Msg }

// Notification is a decoded EVT message (§3.1) from the pager, delivered on
// Events(). It is separate from Event (the reassembler's lower-level record)
// because the two carry different things and the frame-level type is part of
// the codec's contract.
type Notification struct {
	Name   string          `json:"e"`
	Params json.RawMessage `json:"p"`
}

// Stats is a snapshot of the Client's error counters. Resets counts every
// reassembly reset (empty, seq_gap, unexpected_start, unexpected_continue,
// oversize); Malformed counts reassembled messages dropped by Decode and EVT
// payloads that would not parse as JSON. Both are cheap enough to poll.
type Stats struct {
	Resets    uint64
	Malformed uint64
}

// result is what the reader delivers to a waiting Call.
type result struct {
	payload json.RawMessage
	err     error
}

// Client is a PagerLink connection to a pager (or the emulator) over the TCP
// record transport. Create one with Dial. A background goroutine reads records,
// reassembles messages, resolves outstanding Calls, and fans EVT messages out
// on Events().
type Client struct {
	conn net.Conn

	wmu sync.Mutex // serializes writes and the send-side sequence
	seg Segmenter
	re  Reassembler // receive side; touched only by the reader goroutine

	mu      sync.Mutex
	nextID  uint16
	pending map[uint16]chan result

	// Bulk-transfer sinks. xferMu guards xfers and nextXfer; each channel is
	// written only by the reader goroutine (non-blocking) and read by whoever
	// owns that transfer.
	xferMu   sync.Mutex
	xfers    map[uint16]chan Message
	nextXfer uint16

	events chan Notification
	done   chan struct{}

	closeOnce sync.Once
	resets    atomic.Uint64
	malformed atomic.Uint64
}

// Dial opens a TCP connection to addr and starts the reader. timeout bounds the
// connect only; per-request deadlines are passed to Call.
func Dial(addr string, timeout time.Duration) (*Client, error) {
	conn, err := net.DialTimeout("tcp", addr, timeout)
	if err != nil {
		return nil, err
	}
	return newClient(conn), nil
}

// newClient wires up a Client around an established conn and starts its reader.
// It is separate from Dial so tests can drive it over a net.Pipe.
func newClient(conn net.Conn) *Client {
	c := &Client{
		conn:     conn,
		nextID:   2, // the phone side uses even ids, starting at 2
		pending:  make(map[uint16]chan result),
		xfers:    make(map[uint16]chan Message),
		nextXfer: 2, // transfer ids share the phone's even parity
		events:   make(chan Notification, 32),
		done:     make(chan struct{}),
	}
	go c.readLoop()
	return c
}

// Events returns the channel of EVT messages from the pager. It is buffered at
// 32; if the consumer falls behind, further events are dropped rather than
// blocking the reader (the link must keep servicing outstanding Calls).
func (c *Client) Events() <-chan Notification { return c.events }

// Stats returns a snapshot of the link's error counters.
func (c *Client) Stats() Stats {
	return Stats{Resets: c.resets.Load(), Malformed: c.malformed.Load()}
}

// Call sends a REQ for method with JSON params and waits up to timeout for the
// matching RES (whose payload is returned) or ERR (returned as *RPCError).
// Passing nil params omits "p", which the peer reads as {}. The returned
// RawMessage is a copy owned by the caller.
func (c *Client) Call(method string, params any, timeout time.Duration) (json.RawMessage, error) {
	payload, err := marshalRequest(method, params)
	if err != nil {
		return nil, err
	}

	id, ch := c.register()

	raw, err := Message{Type: Req, ID: id, Payload: payload}.Encode()
	if err != nil {
		c.unregister(id)
		return nil, err
	}

	c.wmu.Lock()
	err = c.send(raw)
	c.wmu.Unlock()
	if err != nil {
		c.unregister(id)
		return nil, err
	}

	select {
	case r := <-ch:
		return r.payload, r.err
	case <-time.After(timeout):
		c.unregister(id)
		return nil, ErrTimeout
	case <-c.done:
		c.unregister(id)
		return nil, ErrDisconnected
	}
}

// SendRaw writes packet as one record, verbatim and without touching the send
// sequence. It exists for negative-control tests (injecting a bad seq) and is
// not part of normal operation.
func (c *Client) SendRaw(packet []byte) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	return c.writeRecord(packet)
}

// SendBulk encodes one DATA or ACK message and sends it through the same
// segmentation path as Call's REQ. payload is the bulk payload, not a whole
// message: EncodeData / EncodeAck are the callers' job.
func (c *Client) SendBulk(t MsgType, id uint16, payload []byte) error {
	raw, err := Message{Type: t, ID: id, Payload: payload}.Encode()
	if err != nil {
		return err
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	return c.send(raw)
}

// openXfer installs a bulk sink for id and returns it. It replaces any sink
// already registered for that id.
func (c *Client) openXfer(id uint16) chan Message {
	ch := make(chan Message, 64)
	c.xferMu.Lock()
	c.xfers[id] = ch
	c.xferMu.Unlock()
	return ch
}

// allocXfer reserves the next free even transfer id and installs its sink. Ids
// advance like request ids (wrap 65534 -> 2) and any id still in xfers is
// skipped, so two live transfers never collide.
func (c *Client) allocXfer() (uint16, chan Message) {
	c.xferMu.Lock()
	defer c.xferMu.Unlock()
	id := c.nextXfer
	for {
		if _, busy := c.xfers[id]; !busy {
			break
		}
		id = nextEven(id)
	}
	c.nextXfer = nextEven(id)
	ch := make(chan Message, 64)
	c.xfers[id] = ch
	return id, ch
}

// closeXfer drops the bulk sink for id. Messages already buffered for it are
// discarded.
func (c *Client) closeXfer(id uint16) {
	c.xferMu.Lock()
	delete(c.xfers, id)
	c.xferMu.Unlock()
}

// Close shuts the link down, fails every outstanding Call with
// ErrDisconnected, and closes the underlying connection.
func (c *Client) Close() error {
	c.shutdown(ErrDisconnected)
	return c.conn.Close()
}

// marshalRequest builds {"m":method[,"p":params]}. params == nil omits "p"; any
// other value (including a json.RawMessage) is marshalled as-is.
func marshalRequest(method string, params any) ([]byte, error) {
	var buf bytes.Buffer
	buf.WriteString(`{"m":`)
	mb, err := json.Marshal(method)
	if err != nil {
		return nil, err
	}
	buf.Write(mb)
	if params != nil {
		pb, err := json.Marshal(params)
		if err != nil {
			return nil, err
		}
		buf.WriteString(`,"p":`)
		buf.Write(pb)
	}
	buf.WriteByte('}')
	return buf.Bytes(), nil
}

// register reserves the next free even id and installs its result channel.
func (c *Client) register() (uint16, chan result) {
	c.mu.Lock()
	defer c.mu.Unlock()
	id := c.nextID
	for {
		if _, busy := c.pending[id]; !busy {
			break
		}
		id = nextEven(id)
	}
	c.nextID = nextEven(id)
	ch := make(chan result, 1)
	c.pending[id] = ch
	return id, ch
}

func (c *Client) unregister(id uint16) {
	c.mu.Lock()
	delete(c.pending, id)
	c.mu.Unlock()
}

// nextEven advances to the next even request id, wrapping 65534 -> 2 (§3.2).
func nextEven(id uint16) uint16 {
	id += 2
	if id == 0 || id > 65534 {
		return 2
	}
	return id
}

// send segments raw and writes every packet. Caller holds wmu.
func (c *Client) send(raw []byte) error {
	for _, pkt := range c.seg.Segment(raw, TCPPacketSize) {
		if err := c.writeRecord(pkt); err != nil {
			return err
		}
	}
	return nil
}

// writeRecord writes one u16 length + packet record. Caller holds wmu.
func (c *Client) writeRecord(pkt []byte) error {
	if len(pkt) == 0 || len(pkt) > 0xFFFF {
		return fmt.Errorf("pagerlink: packet length %d out of range", len(pkt))
	}
	rec := make([]byte, 2+len(pkt))
	binary.LittleEndian.PutUint16(rec, uint16(len(pkt)))
	copy(rec[2:], pkt)
	_, err := c.conn.Write(rec)
	return err
}

// readLoop reads records until the connection fails, reassembling and
// dispatching messages as they arrive.
func (c *Client) readLoop() {
	br := bufio.NewReader(c.conn)
	var hdr [2]byte
	for {
		if _, err := io.ReadFull(br, hdr[:]); err != nil {
			c.shutdown(err)
			_ = c.conn.Close()
			return
		}
		pkt := make([]byte, int(binary.LittleEndian.Uint16(hdr[:])))
		if _, err := io.ReadFull(br, pkt); err != nil {
			c.shutdown(err)
			_ = c.conn.Close()
			return
		}
		for _, ev := range c.re.Feed(pkt) {
			if ev.Reset != "" {
				c.resets.Add(1)
				continue
			}
			c.dispatch(ev.Message)
		}
	}
}

// dispatch routes one reassembled message to the waiting Call or Events.
func (c *Client) dispatch(raw []byte) {
	msg, err := Decode(raw)
	if err != nil {
		c.malformed.Add(1)
		return
	}
	switch msg.Type {
	case Res:
		c.deliver(msg.ID, result{payload: json.RawMessage(msg.Payload)})
	case Err:
		var body struct {
			Code string `json:"code"`
			Msg  string `json:"msg"`
		}
		if err := json.Unmarshal(msg.Payload, &body); err != nil {
			c.malformed.Add(1)
			return
		}
		if body.Code == "" {
			body.Code = "internal" // unknown/absent code, per §3.4
		}
		c.deliver(msg.ID, result{err: &RPCError{Code: body.Code, Msg: body.Msg}})
	case Evt:
		var n Notification
		if err := json.Unmarshal(msg.Payload, &n); err != nil {
			c.malformed.Add(1)
			return
		}
		select {
		case c.events <- n:
		default: // consumer is behind; drop rather than stall the reader
		}
	case Data, Ack:
		// A bulk message for a transfer this side owns. Deliver it to that
		// transfer's sink; a full sink drops the message rather than stalling
		// the reader, and an unknown id is ignored.
		c.xferMu.Lock()
		ch, ok := c.xfers[msg.ID]
		c.xferMu.Unlock()
		if !ok {
			return
		}
		select {
		case ch <- msg:
		default:
		}
	}
	// REQ is for the pager side; a client ignores it.
}

// deliver resolves the Call waiting on id, if any. A late or duplicate id has
// no waiter and is dropped (§3.2).
func (c *Client) deliver(id uint16, r result) {
	c.mu.Lock()
	ch, ok := c.pending[id]
	if ok {
		delete(c.pending, id)
	}
	c.mu.Unlock()
	if ok {
		ch <- r
	}
}

// shutdown closes done once and fails every outstanding Call. err is nil only
// on a clean local Close, which passes ErrDisconnected explicitly.
func (c *Client) shutdown(err error) {
	c.closeOnce.Do(func() {
		close(c.done)
		c.mu.Lock()
		pending := c.pending
		c.pending = make(map[uint16]chan result)
		c.mu.Unlock()
		for _, ch := range pending {
			select {
			case ch <- result{err: err}:
			default:
			}
		}
	})
}
