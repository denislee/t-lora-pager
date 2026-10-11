package pagerlink

// File transfer helpers over the bulk-transfer layer (PROTOCOL.md §4, §6).
// fs.read is a download the pager sends, so the phone allocates the (even)
// transfer id and ACKs the DATA; fs.write.begin starts an upload the pager
// receives, so the pager allocates the (odd) transfer id and the phone drives a
// Sender off the pager's ACKs. fs.write.commit is the operation's own RPC and
// carries the 60 s deadline §3.3 names.

import (
	"bytes"
	"encoding/json"
	"errors"
	"hash/crc32"
	"time"
)

// FileInfo is the fs.read / fs.stat metadata for one file. Protected reports
// whether the on-disk bytes are Salted__ ciphertext.
type FileInfo struct {
	Size      uint32
	CRC32     uint32
	Protected bool
}

// ReadFile downloads path from vol and returns its bytes and metadata. size is
// taken from the fs.read RES; every byte is checked against the reported CRC-32.
//
// timeout bounds both the fs.read RPC and each idle wait for a DATA message.
func (c *Client) ReadFile(vol, path string, timeout time.Duration) ([]byte, FileInfo, error) {
	// Allocate the sink before the request: the first DATA can follow the RES
	// immediately, before Call has even returned.
	id, ch := c.allocXfer()
	defer c.closeXfer(id)

	res, err := c.Call("fs.read",
		map[string]any{"vol": vol, "path": path, "xfer": id}, timeout)
	if err != nil {
		return nil, FileInfo{}, err
	}

	var body struct {
		Size      uint32 `json:"size"`
		CRC32     uint32 `json:"crc32"`
		Protected bool   `json:"protected"`
	}
	if err := json.Unmarshal(res, &body); err != nil {
		return nil, FileInfo{}, err
	}
	info := FileInfo{Size: body.Size, CRC32: body.CRC32, Protected: body.Protected}

	var buf bytes.Buffer
	r := NewReceiver(info.Size, InitialCredits, 4, 0,
		func(_ uint32, b []byte) error {
			buf.Write(b)
			return nil
		},
		func(payload []byte) error {
			return c.SendBulk(Ack, id, payload)
		},
	)

	for r.State() == XferActive {
		select {
		case msg := <-ch:
			if msg.Type == Data {
				r.OnData(msg.Payload)
			}
			// ACK messages do not belong on a download; ignore them.
		case <-time.After(timeout):
			return nil, info, errors.New("fs.read: timeout")
		case <-c.done:
			return nil, info, ErrDisconnected
		}
	}
	if r.State() == XferFailed {
		return nil, info, errors.New("fs.read: transfer failed")
	}

	data := buf.Bytes()
	if crc32.ChecksumIEEE(data) != info.CRC32 {
		return nil, info, errors.New("crc mismatch")
	}
	return data, info, nil
}

// BeginWrite starts an upload and returns the transfer id the pager allocated.
// It is a thin wrapper over fs.write.begin for callers that drive the DATA
// themselves (or abandon the transfer) instead of using WriteFile.
func (c *Client) BeginWrite(vol, path string, size, crc uint32, overwrite bool,
	timeout time.Duration) (uint16, error) {
	res, err := c.Call("fs.write.begin", map[string]any{
		"vol":       vol,
		"path":      path,
		"size":      size,
		"crc32":     crc,
		"overwrite": overwrite,
	}, timeout)
	if err != nil {
		return 0, err
	}
	var body struct {
		Xfer uint16 `json:"xfer"`
	}
	if err := json.Unmarshal(res, &body); err != nil {
		return 0, err
	}
	return body.Xfer, nil
}

// SendData sends one DATA message for an upload started with BeginWrite. b is
// the chunk at offset; it must not exceed MaxDataChunk.
func (c *Client) SendData(xfer uint16, offset uint32, b []byte) error {
	return c.SendBulk(Data, xfer, EncodeData(offset, b))
}

// WriteFile uploads data to path on vol, overwriting when overwrite is set. It
// starts the transfer, sends the bytes under the pager's credit window, and
// commits. On any failure before the commit it best-effort aborts the transfer.
//
// timeout bounds fs.write.begin and each idle wait for an ACK; the commit RPC
// gets the 60 s deadline §3.3 names.
func (c *Client) WriteFile(vol, path string, data []byte, overwrite bool, timeout time.Duration) error {
	crc := crc32.ChecksumIEEE(data)
	id, err := c.BeginWrite(vol, path, uint32(len(data)), crc, overwrite, timeout)
	if err != nil {
		return err
	}
	ch := c.openXfer(id)
	defer c.closeXfer(id)

	s := NewSender(uint32(len(data)), MaxDataChunk, 0,
		func(off uint32, n int) ([]byte, error) {
			return data[off : off+uint32(n)], nil
		},
		func(payload []byte) error {
			return c.SendBulk(Data, id, payload)
		},
	)

	var ferr error
	if s.Pump() == XferFailed {
		ferr = errors.New("fs.write: transfer failed")
	}
	for ferr == nil && s.State() == XferActive {
		select {
		case msg := <-ch:
			if msg.Type != Ack {
				continue
			}
			s.OnAck(msg.Payload)
			if s.Pump() == XferFailed {
				ferr = errors.New("fs.write: transfer failed")
			}
		case <-time.After(timeout):
			ferr = errors.New("fs.write: timeout")
		case <-c.done:
			ferr = ErrDisconnected
		}
	}
	if ferr != nil {
		c.abortWrite(id, timeout)
		return ferr
	}

	_, err = c.Call("fs.write.commit", map[string]any{"xfer": id}, 60*time.Second)
	return err
}

// abortWrite drops a half-finished upload, best effort: a failure here must not
// mask the error that triggered it.
func (c *Client) abortWrite(id uint16, timeout time.Duration) {
	_, _ = c.Call("fs.write.abort", map[string]any{"xfer": id}, timeout)
}
