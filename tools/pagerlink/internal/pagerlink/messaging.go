package pagerlink

// The PROTOCOL.md §6.5 messaging field limits and the §6.6 rgb565-rle decoder.
// The pager (src/hal/companion_msg.cpp, src/hal/rle565.cpp) enforces the rules;
// protocol/vectors/messaging.json and screen.json pin both. The Go statement of
// the rules and the encoder are test-only oracles in messaging_test.go.

import (
	"encoding/binary"
	"fmt"
)

// Messaging field limits in bytes (PROTOCOL.md §6.5).
const (
	MsgTitleMax    = 64
	MsgBodyMax     = 512
	MsgIDMax       = 32
	MsgSrcMax      = 32
	MsgClipMax     = 4000
	MsgTextSendMax = 2000
)

// DecodeRLE565 decodes an rgb565-rle stream for a w x h image.
func DecodeRLE565(data []byte, w, h int) ([]uint16, error) {
	if len(data)%4 != 0 {
		return nil, fmt.Errorf("length not a multiple of 4")
	}
	total := w * h
	out := make([]uint16, 0, total)
	for i := 0; i < len(data); i += 4 {
		count := int(binary.LittleEndian.Uint16(data[i:]))
		px := binary.LittleEndian.Uint16(data[i+2:])
		if count == 0 {
			return nil, fmt.Errorf("zero-length run")
		}
		if len(out)+count > total {
			return nil, fmt.Errorf("more pixels than w*h")
		}
		for k := 0; k < count; k++ {
			out = append(out, px)
		}
	}
	if len(out) != total {
		return nil, fmt.Errorf("fewer pixels than w*h")
	}
	return out, nil
}
