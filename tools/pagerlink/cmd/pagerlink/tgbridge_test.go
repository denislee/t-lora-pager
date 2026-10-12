package main

import (
	"encoding/json"
	"testing"
)

// tgParams: no bridge fields means the phone's own login; a half-configured
// bridge is invalid; the "bad" token stands in for a bridge 401.
func TestTgParamsBridgeOptional(t *testing.T) {
	cases := []struct {
		params string
		code   string // "" = accepted
	}{
		{`{"limit":20}`, ""},
		{`{"bridge":"https://b","token":"t","limit":20}`, ""},
		{`{"bridge":"https://b","limit":20}`, "invalid_argument"},
		{`{"token":"t","limit":20}`, "invalid_argument"},
		{`{"bridge":"https://b","token":"bad","limit":20}`, "unauthorized"},
	}
	for _, c := range cases {
		_, perr := tgParams(json.RawMessage(c.params))
		got := ""
		if perr != nil {
			got = perr.Code
		}
		if got != c.code {
			t.Errorf("tgParams(%s) = %q, want %q", c.params, got, c.code)
		}
	}
}
