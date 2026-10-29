package pagerlink

// Golden-vector tests for the client-side messaging rules (PROTOCOL.md §6.5)
// and the rgb565-rle codec (§6.6). Vector files are produced by
// protocol/tools/mkvectors.py and are the oracle, so the expected values come
// straight from them.

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"reflect"
	"testing"
)

type messagingVectorError struct {
	Code string `json:"code"`
	Msg  string `json:"msg"`
}

type messagingVectorCase struct {
	Name       string                `json:"name"`
	Method     string                `json:"method"`
	ParamsJSON string                `json:"params_json"`
	Ok         json.RawMessage       `json:"ok"`
	Error      *messagingVectorError `json:"error"`
}

// messaging.json: every valid case is accepted ("") and every error case
// returns exactly the recorded invalid_argument msg.
func TestMessagingVectors(t *testing.T) {
	var v struct {
		Cases []messagingVectorCase `json:"cases"`
	}
	loadVectors(t, "messaging.json", &v)
	if len(v.Cases) <= 20 {
		t.Fatalf("messaging.json has %d cases, want > 20", len(v.Cases))
	}

	for _, c := range v.Cases {
		got := CheckMessaging(c.Method, json.RawMessage(c.ParamsJSON))
		if c.Error != nil {
			if got != c.Error.Msg {
				t.Errorf("%s: CheckMessaging = %q, want %q", c.Name, got, c.Error.Msg)
			}
			continue
		}
		if got != "" {
			t.Errorf("%s: CheckMessaging = %q, want \"\"", c.Name, got)
		}
	}
}

// screen.json: every image round-trips through EncodeRLE565/DecodeRLE565 and
// every malformed stream is rejected.
func TestScreenVectors(t *testing.T) {
	var v struct {
		Images []struct {
			Name      string `json:"name"`
			W         int    `json:"w"`
			H         int    `json:"h"`
			RLEHex    string `json:"rle_hex"`
			PixelsHex string `json:"pixels_hex"`
			Fill      *int   `json:"fill"`
		} `json:"images"`
		Malformed []struct {
			Name   string `json:"name"`
			W      int    `json:"w"`
			H      int    `json:"h"`
			RLEHex string `json:"rle_hex"`
		} `json:"malformed"`
	}
	loadVectors(t, "screen.json", &v)
	if len(v.Images) == 0 || len(v.Malformed) == 0 {
		t.Fatal("screen.json missing images/malformed cases")
	}

	for _, c := range v.Images {
		var px []uint16
		if c.Fill != nil {
			px = make([]uint16, c.W*c.H)
			for i := range px {
				px[i] = uint16(*c.Fill)
			}
		} else {
			raw := mustHex(t, c.PixelsHex)
			if len(raw)%2 != 0 {
				t.Errorf("%s: pixels_hex has odd length", c.Name)
				continue
			}
			px = make([]uint16, len(raw)/2)
			for i := range px {
				px[i] = binary.LittleEndian.Uint16(raw[i*2:])
			}
		}

		if got := hex.EncodeToString(EncodeRLE565(px)); got != c.RLEHex {
			t.Errorf("%s: EncodeRLE565 = %s, want %s", c.Name, got, c.RLEHex)
		}

		got, err := DecodeRLE565(mustHex(t, c.RLEHex), c.W, c.H)
		if err != nil {
			t.Errorf("%s: DecodeRLE565: %v", c.Name, err)
			continue
		}
		if !reflect.DeepEqual(got, px) {
			t.Errorf("%s: DecodeRLE565 = %d pixels, want %d", c.Name, len(got), len(px))
		}
	}

	for _, c := range v.Malformed {
		if _, err := DecodeRLE565(mustHex(t, c.RLEHex), c.W, c.H); err == nil {
			t.Errorf("%s: DecodeRLE565 accepted %s, want error", c.Name, c.RLEHex)
		}
	}
}
