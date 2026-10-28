package pagerlink

// The settings.json vectors are the pager registry's exact answers (PROTOCOL.md
// §6.4), so these tests decode every recorded result and event into the wire
// types with DisallowUnknownFields: a field the pager sends and the phone does
// not model is a bug, not something to ignore. The path is the same one
// frame_test.go uses, relative to the package dir.

import (
	"encoding/json"
	"strings"
	"testing"
)

// settingsVectorCase is one settings.json "cases" entry. A case carries either a
// result (with result_json, the compact bytes the pager sends) or an error.
type settingsVectorCase struct {
	Name       string          `json:"name"`
	Method     string          `json:"method"`
	Params     json.RawMessage `json:"params"`
	Result     json.RawMessage `json:"result"`
	ResultJSON string          `json:"result_json"`
	Error      *struct {
		Code string `json:"code"`
		Msg  string `json:"msg"`
	} `json:"error"`
}

// settingsVectorEvent is one settings.json "events" entry: the EVT name and its
// params as compact JSON.
type settingsVectorEvent struct {
	Name  string          `json:"name"`
	Event string          `json:"e"`
	P     json.RawMessage `json:"p"`
	PJSON string          `json:"p_json"`
}

// settingsVectors is the whole file.
type settingsVectors struct {
	Cases  []settingsVectorCase  `json:"cases"`
	Events []settingsVectorEvent `json:"events"`
}

// decodeStrict decodes raw into dst rejecting unknown fields and reports whether
// it succeeded.
func decodeStrict(t *testing.T, name, raw string, dst any) bool {
	t.Helper()
	dec := json.NewDecoder(strings.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		t.Errorf("%s: decode %s: %v", name, raw, err)
		return false
	}
	return true
}

// TestSettingsVectors decodes every result into the type matching its method and
// every event into its wire type, then checks the handful of fields the protocol
// singles out.
func TestSettingsVectors(t *testing.T) {
	var v settingsVectors
	loadVectors(t, "settings.json", &v)
	if len(v.Cases) == 0 || len(v.Events) == 0 {
		t.Fatal("settings.json missing cases/events")
	}

	var (
		schemaFirst  *SettingsSchemaPage
		getSecret    *SettingsValues
		setReboot    *SettingsSetResult
		setUnchanged *SettingsSetResult
	)
	results := 0
	for _, c := range v.Cases {
		if c.Error != nil || len(c.Result) == 0 {
			continue
		}
		if c.ResultJSON == "" {
			t.Errorf("%s: no result_json", c.Name)
			continue
		}
		results++
		switch c.Method {
		case "settings.schema":
			var p SettingsSchemaPage
			if !decodeStrict(t, c.Name, c.ResultJSON, &p) {
				continue
			}
			if c.Name == "schema_first_page" {
				schemaFirst = &p
			}
		case "settings.get":
			var g SettingsValues
			if !decodeStrict(t, c.Name, c.ResultJSON, &g) {
				continue
			}
			if c.Name == "get_with_secret" {
				getSecret = &g
			}
		case "settings.set":
			var s SettingsSetResult
			if !decodeStrict(t, c.Name, c.ResultJSON, &s) {
				continue
			}
			switch c.Name {
			case "set_reboot":
				setReboot = &s
			case "set_unchanged":
				setUnchanged = &s
			}
		default:
			t.Errorf("%s: unknown method %q", c.Name, c.Method)
		}
	}
	if results == 0 {
		t.Fatal("settings.json has no result cases")
	}

	// schema_first_page: the first page carries groups, actions and the start of
	// the item catalog, and nsync.token is the secret item.
	if schemaFirst == nil {
		t.Fatal("schema_first_page: case missing")
	}
	if len(schemaFirst.Items) != 6 {
		t.Errorf("schema_first_page: %d items, want 6", len(schemaFirst.Items))
	}
	if len(schemaFirst.Groups) != 4 {
		t.Errorf("schema_first_page: %d groups, want 4", len(schemaFirst.Groups))
	}
	if len(schemaFirst.Actions) != 1 {
		t.Errorf("schema_first_page: %d actions, want 1", len(schemaFirst.Actions))
	}
	if schemaFirst.RevSchema == "" {
		t.Error("schema_first_page: empty rev_schema")
	}
	if it := findSettingItem(schemaFirst.Items, "nsync.token"); it == nil {
		t.Error("schema_first_page: no nsync.token item")
	} else if !it.Secret {
		t.Error("schema_first_page: nsync.token is not secret")
	}

	// get_with_secret: a secret reads back as {"set":false}, never its value.
	if getSecret == nil {
		t.Fatal("get_with_secret: case missing")
	}
	raw, ok := getSecret.Values["nsync.token"]
	if !ok {
		t.Error("get_with_secret: no nsync.token value")
	} else {
		var ph map[string]json.RawMessage
		if err := json.Unmarshal(raw, &ph); err != nil {
			t.Errorf("get_with_secret: nsync.token %s: %v", raw, err)
		} else if len(ph) != 1 {
			t.Errorf("get_with_secret: nsync.token = %s, want only \"set\"", raw)
		} else {
			var set bool
			if b, has := ph["set"]; !has || json.Unmarshal(b, &set) != nil || set {
				t.Errorf("get_with_secret: nsync.token %s, want {\"set\":false}", raw)
			}
		}
	}
	if got, want := getSecret.Rev, uint32(100); got != want {
		t.Errorf("get_with_secret: rev = %d, want %d", got, want)
	}

	// set_reboot: the change needs a reboot; set_unchanged: nothing was applied.
	if setReboot == nil {
		t.Fatal("set_reboot: case missing")
	}
	if !setReboot.Reboot {
		t.Error("set_reboot: reboot = false, want true")
	}
	if setUnchanged == nil {
		t.Fatal("set_unchanged: case missing")
	}
	if len(setUnchanged.Applied) != 0 {
		t.Errorf("set_unchanged: applied = %v, want empty", setUnchanged.Applied)
	}

	// Events: evt.settings.changed names the ids to refetch, and
	// evt.settings.job carries done/ok plus an action-specific result.
	var changed *SettingsChanged
	var jobScan, jobFailed *SettingsJob
	for _, e := range v.Events {
		switch e.Name {
		case "changed":
			var ch SettingsChanged
			if !decodeStrict(t, e.Name, e.PJSON, &ch) {
				continue
			}
			changed = &ch
		case "job_scan":
			var j SettingsJob
			if !decodeStrict(t, e.Name, e.PJSON, &j) {
				continue
			}
			jobScan = &j
		case "job_failed":
			var j SettingsJob
			if !decodeStrict(t, e.Name, e.PJSON, &j) {
				continue
			}
			jobFailed = &j
		default:
			t.Errorf("event %s: unknown name", e.Name)
		}
	}

	if changed == nil {
		t.Fatal("changed: event missing")
	}
	if len(changed.IDs) != 1 || changed.IDs[0] != "display.brightness" {
		t.Errorf("changed: ids = %v, want [display.brightness]", changed.IDs)
	}
	if changed.Rev != 101 {
		t.Errorf("changed: rev = %d, want 101", changed.Rev)
	}

	if jobScan == nil {
		t.Fatal("job_scan: event missing")
	}
	if !jobScan.OK {
		t.Error("job_scan: ok = false, want true")
	}
	if len(jobScan.Result) == 0 {
		t.Error("job_scan: empty result")
	}

	if jobFailed == nil {
		t.Fatal("job_failed: event missing")
	}
	if jobFailed.OK {
		t.Error("job_failed: ok = true, want false")
	}
}

// findSettingItem returns the item with id, or nil.
func findSettingItem(items []SettingItem, id string) *SettingItem {
	for i := range items {
		if items[i].ID == id {
			return &items[i]
		}
	}
	return nil
}
