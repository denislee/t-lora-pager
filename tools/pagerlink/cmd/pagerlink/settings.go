package main

// The settings.* commands and the smoke test's settings section (PROTOCOL.md
// §6.4). A setting is addressed by id and rendered generically, because the
// pager owns the catalog: `schema` walks the whole settings.schema cursor,
// `get`/`set` are plain request/response, and `action` may start a job that
// finishes later with evt.settings.job, so that command waits for it.
//
// The smoke half walks a real pager's catalog end to end: pagination, the secret
// placeholder, an applied write with if_rev, all-or-nothing validation, stale
// revisions, read-only and unknown ids, the unauthorized secret write, a change
// made on the pager side (via the emulator-only test.settings.local hook), the
// refused unknown time zone, and a settings.action job.

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"slices"
	"strings"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

// settingsUsage is the one-line form of the subcommands, shared by the error
// paths here and the top-level usage text.
const settingsUsage = "pagerlink settings schema|get [id...]|set [-if-rev N] id=value...|action <id> [json]"

// cmdSettings dispatches one settings.* subcommand.
func cmdSettings(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: "+settingsUsage)
		return 2
	}
	sub, rest := args[0], args[1:]
	switch sub {
	case "schema":
		schema, err := c.SettingsSchema(timeout)
		if err != nil {
			return fail("settings.schema", err)
		}
		raw, err := json.Marshal(schema)
		if err != nil {
			return fail("settings.schema", err)
		}
		printJSON("settings.schema", raw)
		return 0
	case "get":
		var params any
		if len(rest) > 0 {
			params = map[string]any{"ids": rest}
		}
		return callOne(c, timeout, "settings.get", params)
	case "set":
		return cmdSettingsSet(c, timeout, rest)
	case "action":
		return cmdSettingsAction(c, timeout, rest)
	}
	fmt.Fprintf(os.Stderr, "pagerlink: unknown settings subcommand %q\n", sub)
	fmt.Fprintln(os.Stderr, "usage: "+settingsUsage)
	return 2
}

// cmdSettingsSet writes one or more id=value pairs. A value is parsed as JSON
// when it parses (so 8, true, "text" and [] all work) and taken as the literal
// string otherwise, which keeps `set display.sleep_mode=deep` natural.
func cmdSettingsSet(c *pagerlink.Client, timeout time.Duration, args []string) int {
	fs := flag.NewFlagSet("settings set", flag.ContinueOnError)
	ifRev := fs.Int64("if-rev", -1, "require this settings revision (default: any)")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	rest := fs.Args()
	if len(rest) == 0 {
		fmt.Fprintln(os.Stderr, "usage: "+settingsUsage)
		return 2
	}

	values := make(map[string]json.RawMessage, len(rest))
	for _, pair := range rest {
		id, text, ok := strings.Cut(pair, "=")
		if !ok || id == "" {
			fmt.Fprintf(os.Stderr, "pagerlink: %q: want id=value\n", pair)
			return 2
		}
		values[id] = settingsValue(text)
	}
	params := map[string]any{"values": values}
	if *ifRev >= 0 {
		params["if_rev"] = *ifRev
	}
	return callOne(c, timeout, "settings.set", params)
}

// settingsValue turns one command-line value into JSON: the text itself when it
// is valid JSON, else the text as a JSON string.
func settingsValue(text string) json.RawMessage {
	var v any
	if err := json.Unmarshal([]byte(text), &v); err != nil {
		v = text
	}
	raw, err := json.Marshal(v)
	if err != nil { // unreachable: marshal of string/number/bool/map/slice
		return json.RawMessage(text)
	}
	return raw
}

// cmdSettingsAction runs one action and, when it answers {"job"}, waits for the
// evt.settings.job that finishes it and prints its params. Exit 1 when the job
// fails or never reports, so a script can tell an action that worked.
func cmdSettingsAction(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: "+settingsUsage)
		return 2
	}
	params := map[string]any{"id": args[0]}
	if len(args) > 1 {
		var p json.RawMessage
		if err := json.Unmarshal([]byte(args[1]), &p); err != nil {
			fmt.Fprintf(os.Stderr, "pagerlink: invalid params JSON: %v\n", err)
			return 2
		}
		params["p"] = p
	}

	res, err := c.Call("settings.action", params, timeout)
	if err != nil {
		return fail("settings.action", err)
	}
	printJSON("settings.action", res)

	var body struct {
		Job string `json:"job"`
	}
	if err := json.Unmarshal(res, &body); err != nil || body.Job == "" {
		return 0 // a synchronous action (or one the pager confirms itself)
	}
	n, job, ok := waitSettingsJob(c, body.Job, 120*time.Second)
	if !ok {
		fmt.Fprintf(os.Stderr, "pagerlink: evt.settings.job for %q: no event in time\n", body.Job)
		return 1
	}
	printJSON("evt.settings.job", n.Params)
	if !job.OK {
		return 1
	}
	return 0
}

// waitSettingsJob drains c.Events() until an evt.settings.job for job arrives or
// within elapses, returning the notification, the decoded job and whether one
// arrived.
func waitSettingsJob(c *pagerlink.Client, job string, within time.Duration) (pagerlink.Notification, pagerlink.SettingsJob, bool) {
	deadline := time.After(within)
	for {
		select {
		case n := <-c.Events():
			if n.Name != "evt.settings.job" {
				continue
			}
			var j pagerlink.SettingsJob
			if err := json.Unmarshal(n.Params, &j); err != nil {
				continue
			}
			if j.Job == job {
				return n, j, true
			}
		case <-deadline:
			return pagerlink.Notification{}, pagerlink.SettingsJob{}, false
		case <-c.Done():
			return pagerlink.Notification{}, pagerlink.SettingsJob{}, false
		}
	}
}

// waitSettingsChanged drains c.Events() until an evt.settings.changed that lists
// id arrives or within elapses.
func waitSettingsChanged(c *pagerlink.Client, id string, within time.Duration) (pagerlink.SettingsChanged, bool) {
	deadline := time.After(within)
	for {
		select {
		case n := <-c.Events():
			if n.Name != "evt.settings.changed" {
				continue
			}
			var ch pagerlink.SettingsChanged
			if err := json.Unmarshal(n.Params, &ch); err != nil {
				continue
			}
			if slices.Contains(ch.IDs, id) {
				return ch, true
			}
		case <-deadline:
			return pagerlink.SettingsChanged{}, false
		case <-c.Done():
			return pagerlink.SettingsChanged{}, false
		}
	}
}

// drainEvents discards every notification already queued on c.Events(), so a
// later wait sees only what the step it started produces.
func drainEvents(c *pagerlink.Client) {
	for {
		select {
		case <-c.Events():
		default:
			return
		}
	}
}

// rpcErr performs one call and returns the *RPCError it failed with, or nil when
// it returned a result or failed some other way (which it prints).
func rpcErr(c *pagerlink.Client, timeout time.Duration, method string, params any) *pagerlink.RPCError {
	_, err := c.Call(method, params, timeout)
	var re *pagerlink.RPCError
	if errors.As(err, &re) {
		return re
	}
	if err != nil {
		fmt.Printf("       %s: %v\n", method, err)
	}
	return nil
}

// readSettings calls settings.get for every item (or the given ids) and decodes
// the result.
func readSettings(c *pagerlink.Client, timeout time.Duration, ids ...string) (pagerlink.SettingsValues, error) {
	var params any
	if len(ids) > 0 {
		params = map[string]any{"ids": ids}
	}
	res, err := c.Call("settings.get", params, timeout)
	if err != nil {
		return pagerlink.SettingsValues{}, err
	}
	var v pagerlink.SettingsValues
	err = json.Unmarshal(res, &v)
	return v, err
}

// readSetting reads one value with settings.get and decodes it into dst.
func readSetting(c *pagerlink.Client, timeout time.Duration, id string, dst any) error {
	v, err := readSettings(c, timeout, id)
	if err != nil {
		return err
	}
	raw, ok := v.Values[id]
	if !ok {
		return fmt.Errorf("%s: no value", id)
	}
	return json.Unmarshal(raw, dst)
}

// secretPlaceholderOK reports whether raw is {"set":<bool>} — an object with
// exactly that key — and says why not when it is not.
func secretPlaceholderOK(id string, raw json.RawMessage) bool {
	var ph map[string]json.RawMessage
	if err := json.Unmarshal(raw, &ph); err != nil {
		fmt.Printf("       %s: secret placeholder %s: %v\n", id, raw, err)
		return false
	}
	b, has := ph["set"]
	var set bool
	if len(ph) != 1 || !has || json.Unmarshal(b, &set) != nil {
		fmt.Printf("       %s: secret placeholder %s is not {\"set\":bool}\n", id, raw)
		return false
	}
	return true
}

// smokeSettings drives the twelve settings steps (a-l) and reports each. It
// leaves display.brightness as it found it.
func smokeSettings(c *pagerlink.Client, timeout time.Duration) bool {
	pass := true

	// a. The whole catalog, across every page: enough items to prove paging,
	//    unique ids, two items every board has, and ordered int ranges.
	schema, schemaErr := c.SettingsSchema(timeout)
	if schemaErr != nil {
		fmt.Printf("       settings.schema: %v\n", schemaErr)
	}
	byID := make(map[string]pagerlink.SettingItem, len(schema.Items))
	idsUnique, rangesOK := true, true
	for _, it := range schema.Items {
		if _, dup := byID[it.ID]; dup {
			fmt.Printf("       duplicate id %q\n", it.ID)
			idsUnique = false
		}
		byID[it.ID] = it
		if it.Type != "int" {
			continue
		}
		if it.Min == nil || it.Max == nil {
			fmt.Printf("       %s: int item without min/max\n", it.ID)
			rangesOK = false
		} else if *it.Min > *it.Max {
			fmt.Printf("       %s: min %d > max %d\n", it.ID, *it.Min, *it.Max)
			rangesOK = false
		}
	}
	_, hasBrightness := byID["display.brightness"]
	_, hasBT := byID["conn.bt"]
	pass = report(schemaErr == nil && len(schema.Items) >= 40 && idsUnique && hasBrightness && hasBT && rangesOK,
		"settings.schema pages to the end: %d items, unique ids, display.brightness + conn.bt, int ranges ordered",
		len(schema.Items)) && pass

	// b. Reading everything must return a secret as {"set":bool}, never a value.
	all, allErr := readSettings(c, timeout)
	if allErr != nil {
		fmt.Printf("       settings.get: %v\n", allErr)
	}
	secretOK := allErr == nil
	if secretOK {
		for _, it := range schema.Items {
			if it.Secret && !secretPlaceholderOK(it.ID, all.Values[it.ID]) {
				secretOK = false
			}
		}
	}
	pass = report(secretOK, "settings.get: every secret item reads {\"set\":bool}") && pass

	// c. An int write with if_rev must be applied and read back.
	item, hasItem := byID["display.brightness"]
	var cur, v2 int64
	setOK := allErr == nil && hasItem && item.Min != nil && item.Max != nil
	if setOK {
		if err := json.Unmarshal(all.Values["display.brightness"], &cur); err != nil {
			fmt.Printf("       display.brightness: current value %s: %v\n",
				all.Values["display.brightness"], err)
			setOK = false
		}
	}
	if setOK {
		v2 = *item.Min
		if cur == *item.Min {
			v2 = *item.Min + 1
		}
		if v2 > *item.Max {
			v2 = *item.Max
		}
		var applied pagerlink.SettingsSetResult
		res, err := c.Call("settings.set", map[string]any{
			"values": map[string]any{"display.brightness": v2},
			"if_rev": all.Rev,
		}, timeout)
		if err != nil {
			fmt.Printf("       settings.set display.brightness: %v\n", err)
			setOK = false
		} else if err := json.Unmarshal(res, &applied); err != nil {
			fmt.Printf("       settings.set result %s: %v\n", res, err)
			setOK = false
		} else if !slices.Contains(applied.Applied, "display.brightness") {
			fmt.Printf("       settings.set applied %v, want display.brightness\n", applied.Applied)
			setOK = false
		}
	}
	if setOK {
		var back int64
		if err := readSetting(c, timeout, "display.brightness", &back); err != nil {
			fmt.Printf("       settings.get display.brightness: %v\n", err)
			setOK = false
		} else if back != v2 {
			fmt.Printf("       display.brightness reads %d, want %d\n", back, v2)
			setOK = false
		}
	}
	pass = report(setOK, "settings.set display.brightness %d -> %d (if_rev %d): applied, read back",
		cur, v2, all.Rev) && pass

	// d. All or nothing: one bad value fails the call naming it, and the good
	//    value in the same request is not written.
	atomicOK := setOK
	if setOK {
		re := rpcErr(c, timeout, "settings.set", map[string]any{
			"values": map[string]any{"display.brightness": cur, "display.timeout_s": 9999},
		})
		if re == nil || re.Code != "invalid_argument" || !strings.HasPrefix(re.Msg, "display.timeout_s") {
			fmt.Printf("       settings.set one-bad value -> %v, want invalid_argument display.timeout_s…\n", re)
			atomicOK = false
		}
	}
	if atomicOK {
		var back int64
		if err := readSetting(c, timeout, "display.brightness", &back); err != nil {
			fmt.Printf("       settings.get display.brightness: %v\n", err)
			atomicOK = false
		} else if back != v2 {
			fmt.Printf("       display.brightness reads %d after the refused set, want %d\n", back, v2)
			atomicOK = false
		}
	}
	pass = report(atomicOK,
		"settings.set all-or-nothing: display.timeout_s 9999 -> invalid_argument, brightness still %d", v2) && pass

	// e. A stale if_rev is refused with "changed" and nothing is written.
	staleOK := setOK
	if setOK {
		now, err := readSettings(c, timeout, "display.brightness")
		if err != nil {
			fmt.Printf("       settings.get display.brightness: %v\n", err)
			staleOK = false
		} else {
			re := rpcErr(c, timeout, "settings.set", map[string]any{
				"values": map[string]any{"display.brightness": v2},
				"if_rev": now.Rev - 1,
			})
			if re == nil || re.Code != "invalid_argument" || re.Msg != "changed" {
				fmt.Printf("       settings.set if_rev %d -> %v, want invalid_argument \"changed\"\n",
					now.Rev-1, re)
				staleOK = false
			}
		}
	}
	pass = report(staleOK, "settings.set if_rev rev-1 -> invalid_argument \"changed\"") && pass

	// f. An unknown id and a read-only item are both invalid_argument.
	unknownRe := rpcErr(c, timeout, "settings.set",
		map[string]any{"values": map[string]any{"nope.x": 1}})
	roRe := rpcErr(c, timeout, "settings.set",
		map[string]any{"values": map[string]any{"wifi.saved": []string{}}})
	idsOK := true
	if unknownRe == nil || unknownRe.Code != "invalid_argument" {
		fmt.Printf("       settings.set nope.x -> %v, want invalid_argument\n", unknownRe)
		idsOK = false
	}
	if roRe == nil || roRe.Code != "invalid_argument" {
		fmt.Printf("       settings.set wifi.saved -> %v, want invalid_argument\n", roRe)
		idsOK = false
	}
	pass = report(idsOK, "settings.set nope.x and wifi.saved -> invalid_argument") && pass

	// g. A non-empty secret needs notes encryption, which the emulator lacks, so
	//    the write is unauthorized and the value stays unset.
	secretRe := rpcErr(c, timeout, "settings.set",
		map[string]any{"values": map[string]any{"nsync.token": "smoke-not-a-token"}})
	secretSetOK := secretRe != nil && secretRe.Code == "unauthorized"
	if !secretSetOK {
		fmt.Printf("       settings.set nsync.token -> %v, want unauthorized\n", secretRe)
	}
	if secretSetOK {
		var ph struct {
			Set bool `json:"set"`
		}
		if err := readSetting(c, timeout, "nsync.token", &ph); err != nil {
			fmt.Printf("       settings.get nsync.token: %v\n", err)
			secretSetOK = false
		} else if ph.Set {
			fmt.Printf("       nsync.token reads set=true after a refused write\n")
			secretSetOK = false
		}
	}
	pass = report(secretSetOK,
		"settings.set nsync.token -> unauthorized (no notes encryption), still {\"set\":false}") && pass

	// h. A change made on the pager side must surface as evt.settings.changed.
	var showMem bool
	readErr := readSetting(c, timeout, "statusbar.show_mem", &showMem)
	if readErr != nil {
		fmt.Printf("       settings.get statusbar.show_mem: %v\n", readErr)
	}
	pagerOK := readErr == nil
	if pagerOK {
		drainEvents(c)
		if _, err := c.Call("test.settings.local",
			map[string]any{"id": "statusbar.show_mem", "value": !showMem}, timeout); err != nil {
			fmt.Printf("       test.settings.local: %v\n", err)
			pagerOK = false
		} else if ch, ok := waitSettingsChanged(c, "statusbar.show_mem", 3*time.Second); !ok {
			fmt.Printf("       no evt.settings.changed for statusbar.show_mem within 3s\n")
			pagerOK = false
		} else {
			fmt.Printf("       evt.settings.changed rev %d ids %v\n", ch.Rev, ch.IDs)
		}
		// Put the pager's own value back whether or not the event arrived.
		if _, err := c.Call("test.settings.local",
			map[string]any{"id": "statusbar.show_mem", "value": showMem}, timeout); err != nil {
			fmt.Printf("       test.settings.local restore: %v\n", err)
			pagerOK = false
		}
	}
	pass = report(pagerOK, "test.settings.local statusbar.show_mem -> evt.settings.changed, restored") && pass

	// i. A time zone the pager has no rule for is refused.
	tzRe := rpcErr(c, timeout, "sys.time.set",
		map[string]any{"epoch": time.Now().Unix(), "tz": "Mars/Olympus_Mons"})
	tzOK := tzRe != nil && tzRe.Code == "invalid_argument"
	if !tzOK {
		fmt.Printf("       sys.time.set Mars/Olympus_Mons -> %v, want invalid_argument\n", tzRe)
	}
	pass = report(tzOK, "sys.time.set tz Mars/Olympus_Mons -> invalid_argument") && pass

	// j. An action that answers {"job"} finishes with evt.settings.job.
	jobOK := false
	var jobID string
	res, err := c.Call("settings.action", map[string]any{"id": "wifi.test"}, timeout)
	if err != nil {
		fmt.Printf("       settings.action wifi.test: %v\n", err)
	} else {
		var body struct {
			Job string `json:"job"`
		}
		if err := json.Unmarshal(res, &body); err != nil || body.Job == "" {
			fmt.Printf("       settings.action wifi.test result %s, want {\"job\"}\n", res)
		} else {
			jobID = body.Job
			_, j, ok := waitSettingsJob(c, jobID, 10*time.Second)
			jobOK = ok && j.Done
			if !ok {
				fmt.Printf("       no evt.settings.job for %q within 10s\n", jobID)
			}
		}
	}
	pass = report(jobOK, "settings.action wifi.test -> job %q, evt.settings.job done", jobID) && pass

	// k. An unknown action is invalid_argument.
	actionRe := rpcErr(c, timeout, "settings.action", map[string]any{"id": "nope.y"})
	actionOK := actionRe != nil && actionRe.Code == "invalid_argument"
	if !actionOK {
		fmt.Printf("       settings.action nope.y -> %v, want invalid_argument\n", actionRe)
	}
	pass = report(actionOK, "settings.action nope.y -> invalid_argument") && pass

	// l. Put display.brightness back the way it was.
	restoreOK := setOK
	if setOK {
		res, err := c.Call("settings.set",
			map[string]any{"values": map[string]any{"display.brightness": cur}}, timeout)
		restoreOK = err == nil
		if err != nil {
			fmt.Printf("       settings.set restore: %v\n", err)
		} else {
			printJSON("       settings.set restore", res)
		}
	}
	pass = report(restoreOK, "settings.set display.brightness restored to %d", cur) && pass

	return pass
}
