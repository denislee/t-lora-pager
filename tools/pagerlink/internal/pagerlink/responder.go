package pagerlink

// The responder role (PROTOCOL.md §3): the pager can send a REQ to the phone,
// and the phone answers RES or ERR on the same id. Handlers are registered by
// method name with Handle and run on their own goroutine so a slow one never
// stalls the reader. Responses go back through the same segmenting send path
// as requests.
//
// AcceptBytes is the receive side of §4 for a transfer the pager starts: the
// phone allocates the even transfer id, installs its sink before the RES is
// sent (so DATA that races the response is not lost), and the returned wait
// function drives the Receiver/ACK loop.

import (
	"bytes"
	"encoding/json"
	"time"
)

// Handler answers one pager request. params is the request's "p" (or {} when
// absent). A nil result marshals to {}, and a nil *RPCError means success.
type Handler func(params json.RawMessage) (any, *RPCError)

// Handle registers the handler for method, replacing any previous one. Passing
// a nil handler removes the registration, after which requests for method are
// answered ERR unsupported. Safe for concurrent use.
func (c *Client) Handle(method string, h Handler) {
	c.hmu.Lock()
	defer c.hmu.Unlock()
	if h == nil {
		delete(c.handlers, method)
		return
	}
	c.handlers[method] = h
}

// handleReq answers one REQ from the pager. It validates the payload and id
// parity, looks up the handler, and runs it in a new goroutine. The response
// carries the request's id and goes through SendBulk, the same segmentation
// path Call uses.
func (c *Client) handleReq(msg Message) {
	var req struct {
		M string          `json:"m"`
		P json.RawMessage `json:"p"`
	}
	if err := json.Unmarshal(msg.Payload, &req); err != nil || req.M == "" {
		c.sendError(msg.ID, "bad_request", "bad request")
		return
	}
	// The pager allocates odd ids; an even (or zero) id is a parity violation.
	if msg.ID == 0 || msg.ID%2 == 0 {
		c.sendError(msg.ID, "invalid_argument", "bad request id parity")
		return
	}

	c.hmu.Lock()
	h := c.handlers[req.M]
	c.hmu.Unlock()
	if h == nil {
		c.sendError(msg.ID, "unsupported", "unknown method "+req.M)
		return
	}

	params := req.P
	if len(params) == 0 {
		params = json.RawMessage("{}")
	}
	id := msg.ID
	go func() {
		res, rerr := h(params)
		if rerr != nil {
			code := rerr.Code
			if code == "" {
				code = "internal"
			}
			c.sendError(id, code, rerr.Msg)
			return
		}
		if res == nil {
			_ = c.SendBulk(Res, id, []byte("{}"))
			return
		}
		body, err := json.Marshal(res)
		if err != nil {
			c.sendError(id, "internal", err.Error())
			return
		}
		_ = c.SendBulk(Res, id, body)
	}()
}

// sendError answers a request with ERR {code,msg}.
func (c *Client) sendError(id uint16, code, msg string) {
	body, _ := json.Marshal(struct {
		Code string `json:"code"`
		Msg  string `json:"msg"`
	}{Code: code, Msg: msg})
	_ = c.SendBulk(Err, id, body)
}

// Emit sends an EVT (id 0) carrying {"e":event,"p":params}. A nil params is
// sent as {}.
func (c *Client) Emit(event string, params any) error {
	e, err := json.Marshal(event)
	if err != nil {
		return err
	}
	var buf bytes.Buffer
	buf.WriteString(`{"e":`)
	buf.Write(e)
	buf.WriteString(`,"p":`)
	if params == nil {
		buf.WriteString(`{}`)
	} else {
		p, err := json.Marshal(params)
		if err != nil {
			return err
		}
		buf.Write(p)
	}
	buf.WriteByte('}')
	return c.SendBulk(Evt, 0, buf.Bytes())
}

// AcceptBytes prepares to receive size bytes of DATA, checking the whole file
// against crc (CRC-32/ISO-HDLC). It allocates an even transfer id and installs
// its sink immediately, so DATA arriving right after the handler's RES is not
// lost. The returned function runs the Receiver/ACK loop (ACK every 4 messages,
// InitialCredits window) until the transfer completes, times out, or the link
// drops, and closes the transfer on every exit. A zero size returns empty data
// without waiting.
func (c *Client) AcceptBytes(size uint32, crc uint32) (uint16, func(timeout time.Duration) ([]byte, error)) {
	id, ch := c.allocXfer()

	wait := func(timeout time.Duration) ([]byte, error) {
		defer c.closeXfer(id)
		if size == 0 {
			return []byte{}, nil
		}
		return c.receive(id, ch, size, crc, timeout, "accept")
	}
	return id, wait
}
