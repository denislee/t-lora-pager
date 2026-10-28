package pagerlink

// Settings wire types and helpers for the §6.4 methods. The pager owns the
// catalog: settings.schema describes the items that exist on the connected
// board (with that board's ranges), settings.get reads values, settings.set
// writes them all-or-nothing, and settings.action starts a one-shot command
// that may finish later with an evt.settings.job. Secrets never travel pager ->
// phone: a secret item reads back as {"set":bool}.

import (
	"encoding/json"
	"fmt"
	"time"
)

// SettingOption is one choice of an enum item: the value as the pager stores it
// (a string or an integer, so it stays raw) plus the label the app shows.
type SettingOption struct {
	V     json.RawMessage `json:"v"`
	Label string          `json:"label,omitempty"`
}

// SettingItem describes one setting. Min/Max/Step bound an "int" item, Enum
// carries the choices of an "enum" item, and MaxLen bounds a "string"/"secret".
// Apply says when a change takes effect (live, tick, theme, sleep, reboot);
// Secret marks a value the pager never sends, Danger an item the app should
// confirm first, and RO a read-only item.
type SettingItem struct {
	ID     string          `json:"id"`
	Group  string          `json:"group"`
	Title  string          `json:"title"`
	Type   string          `json:"type"`
	Min    *int64          `json:"min,omitempty"`
	Max    *int64          `json:"max,omitempty"`
	Step   *int64          `json:"step,omitempty"`
	Enum   []SettingOption `json:"enum,omitempty"`
	MaxLen *int            `json:"max_len,omitempty"`
	Apply  string          `json:"apply"`
	Secret bool            `json:"secret,omitempty"`
	Danger bool            `json:"danger,omitempty"`
	RO     bool            `json:"ro,omitempty"`
	Hint   string          `json:"hint,omitempty"`
}

// SettingGroup is one section of the catalog.
type SettingGroup struct {
	ID    string `json:"id"`
	Title string `json:"title"`
}

// SettingAction is a one-shot command (e.g. wifi.scan) rather than a value.
type SettingAction struct {
	ID     string `json:"id"`
	Group  string `json:"group"`
	Title  string `json:"title"`
	Danger bool   `json:"danger,omitempty"`
	Hint   string `json:"hint,omitempty"`
}

// SettingsSchemaPage is one page of settings.schema. Groups and Actions are
// present only on the first page; Cursor is set while items remain.
type SettingsSchemaPage struct {
	RevSchema string          `json:"rev_schema"`
	Groups    []SettingGroup  `json:"groups"`
	Actions   []SettingAction `json:"actions"`
	Items     []SettingItem   `json:"items"`
	Cursor    string          `json:"cursor,omitempty"`
}

// SettingsValues is the settings.get result: the current rev, the schema hash it
// belongs to, and each requested value keyed by id.
type SettingsValues struct {
	Rev       uint32                     `json:"rev"`
	RevSchema string                     `json:"rev_schema"`
	Values    map[string]json.RawMessage `json:"values"`
}

// SettingsSetResult is the settings.set result. Applied lists only the ids that
// actually changed; Reboot is true when an applied value needs a reboot.
type SettingsSetResult struct {
	Rev     uint32   `json:"rev"`
	Applied []string `json:"applied"`
	Reboot  bool     `json:"reboot"`
}

// SettingsChanged is the evt.settings.changed payload: a change from either side
// moved rev, and these ids need refetching with settings.get.
type SettingsChanged struct {
	Rev uint32   `json:"rev"`
	IDs []string `json:"ids"`
}

// SettingsJob is the evt.settings.job payload: progress and the final outcome of
// a job settings.action started. Result depends on the action.
type SettingsJob struct {
	Job    string          `json:"job"`
	Done   bool            `json:"done"`
	OK     bool            `json:"ok"`
	Msg    string          `json:"msg"`
	Result json.RawMessage `json:"result,omitempty"`
}

// schemaPageLimit bounds settings.schema pagination, so a pager that never drops
// "cursor" cannot spin the caller forever.
const schemaPageLimit = 64

// SettingsSchema fetches the whole catalog by following settings.schema's cursor
// to the end. Items from every page are merged; groups and actions come from the
// first page, which is the only one that carries them. Every page must report the
// same rev_schema, since that hash covers the whole schema (§6.4).
func (c *Client) SettingsSchema(timeout time.Duration) (SettingsSchemaPage, error) {
	var out SettingsSchemaPage
	var cursor string
	for page := 0; page < schemaPageLimit; page++ {
		var params any
		if cursor != "" {
			params = map[string]any{"cursor": cursor}
		}
		res, err := c.Call("settings.schema", params, timeout)
		if err != nil {
			return SettingsSchemaPage{}, err
		}
		var p SettingsSchemaPage
		if err := json.Unmarshal(res, &p); err != nil {
			return SettingsSchemaPage{}, err
		}
		if page == 0 {
			out.RevSchema = p.RevSchema
			out.Groups = p.Groups
			out.Actions = p.Actions
		} else if p.RevSchema != out.RevSchema {
			return SettingsSchemaPage{}, fmt.Errorf(
				"settings.schema: rev_schema changed while paging: %q != %q",
				p.RevSchema, out.RevSchema)
		}
		out.Items = append(out.Items, p.Items...)
		if p.Cursor == "" {
			return out, nil
		}
		cursor = p.Cursor
	}
	return SettingsSchemaPage{}, fmt.Errorf("settings.schema: more than %d pages", schemaPageLimit)
}
