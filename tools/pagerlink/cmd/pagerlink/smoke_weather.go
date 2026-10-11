package main

// The weather.push half of the smoke test (IOS_COMPANION_PLAN.md Track C 9):
// `pagerlink smoke` hands the pager a ForecastPayload with weather.push, then
// reads the stored copy back through the emulator-only test.weather.cache hook
// (src/hal/link/handlers_test.cpp) and checks it is byte-identical. A second
// push with one field out of range must be refused whole with invalid_argument
// and leave the cache untouched. Like smokeLink, these steps pass against the
// emulator alone.

import (
	"encoding/json"
	"fmt"
	"reflect"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

// weatherPushSteps runs the two weather.push steps and reports each. c is the
// live client returned by smokeFS.
func weatherPushSteps(c *pagerlink.Client, timeout time.Duration) bool {
	pass := true
	const key = "smoke-wx"

	// readCache fetches the forecast the pager stored under key and decodes it
	// back into a forecastPayload.
	readCache := func() (forecastPayload, error) {
		var got forecastPayload
		res, err := c.Call("test.weather.cache", map[string]any{"key": key}, timeout)
		if err != nil {
			return got, err
		}
		if err := json.Unmarshal(res, &got); err != nil {
			return got, err
		}
		return got, nil
	}

	// r. A valid push must be stored, and the read-back must match it exactly:
	//    same key, timestamp, hourly cells and daily cells.
	want := mockForecast(key, time.Now())
	_, err := c.Call("weather.push", want, timeout)
	ok := err == nil
	if !ok {
		fmt.Printf("       weather.push: %v\n", err)
	}
	var got forecastPayload
	if ok {
		got, err = readCache()
		if err != nil {
			fmt.Printf("       test.weather.cache: %v\n", err)
			ok = false
		}
	}
	if ok && !(got.Key == want.Key && got.Ts == want.Ts &&
		reflect.DeepEqual(got.Hourly, want.Hourly) && reflect.DeepEqual(got.Daily, want.Daily)) {
		ok = false
	}
	if !ok {
		fmt.Printf("       want %+v\n", want)
		fmt.Printf("       got  %+v\n", got)
	}
	pass = report(ok, "phone->pager weather.push 8h/7d + test.weather.cache read-back identical") && pass

	// s. A deep copy of the same payload with an out-of-range temperature must
	//    be refused with invalid_argument, and the stored copy must be
	//    unchanged (the push is rejected whole, never clamped or half-stored).
	bad := want
	bad.Hourly = append([]wxHour(nil), want.Hourly...)
	bad.Daily = append([]wxDay(nil), want.Daily...)
	bad.Hourly[0].Temp = 200

	ok = rpcErrCode(c, timeout, "weather.push", bad, "invalid_argument")
	if ok {
		got, err = readCache()
		if err != nil {
			fmt.Printf("       test.weather.cache: %v\n", err)
			ok = false
		} else if !(got.Ts == want.Ts &&
			reflect.DeepEqual(got.Hourly, want.Hourly) && reflect.DeepEqual(got.Daily, want.Daily)) {
			ok = false
			fmt.Printf("       want %+v\n", want)
			fmt.Printf("       got  %+v\n", got)
		}
	}
	pass = report(ok, "phone->pager weather.push temp 200 -> invalid_argument, cache unchanged") && pass

	return pass
}

// weatherGetSteps runs the two weather.get steps (HUB_REMOVAL_PLAN.md H2.3):
// the pager runs the Weather app's phone step through the emulator-only
// test.weather.fetch hook ({key} -> RES {} at once, then evt.test.result
// {step:"weather",ok,msg,city,hourly_count,daily_count}); the smoke answers the
// pager's weather.get. A valid get must answer with a full forecast the pager
// caches, while a reply for another key must be refused and leave the cache
// untouched. Like the weather.push steps, these pass against the emulator alone.
func weatherGetSteps(c *pagerlink.Client, timeout time.Duration) bool {
	pass := true
	const key = "smoke-wx-get"

	// readCache fetches the forecast the pager stored under key and decodes it
	// back into a forecastPayload.
	readCache := func() (forecastPayload, error) {
		var got forecastPayload
		res, err := c.Call("test.weather.cache", map[string]any{"key": key}, timeout)
		if err != nil {
			return got, err
		}
		if err := json.Unmarshal(res, &got); err != nil {
			return got, err
		}
		return got, nil
	}

	// fetch drives one test.weather.fetch step and returns the evt.test.result
	// the pager emits, waiting up to 20 s for it. ok is false when the call
	// itself fails or no result arrives.
	fetch := func() (testResult, bool) {
		if _, err := c.Call("test.weather.fetch", map[string]any{"key": key}, timeout); err != nil {
			fmt.Printf("       test.weather.fetch: %v\n", err)
			return testResult{}, false
		}
		n, ok := waitResult(c, 20*time.Second)
		if !ok {
			fmt.Printf("       no evt.test.result within 20s\n")
			return testResult{}, false
		}
		var r testResult
		if err := json.Unmarshal(n.Params, &r); err != nil {
			fmt.Printf("       evt.test.result params %q: %v\n", n.Params, err)
			return testResult{}, false
		}
		return r, true
	}

	// now is the timestamp the standard handler builds its forecast with; it is
	// reset to time.Now() when the handler is restored at the end.
	now := time.Now()

	// getHandler is the phone-side weather.get handler the pager's phone step
	// calls: it requires a non-empty key and answers with an in-range forecast
	// for it, built at now.
	getHandler := func(params json.RawMessage) (any, *pagerlink.RPCError) {
		var p struct {
			Key string `json:"key"`
		}
		if err := json.Unmarshal(params, &p); err != nil {
			return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: err.Error()}
		}
		if p.Key == "" {
			return nil, &pagerlink.RPCError{Code: "invalid_argument", Msg: "key required"}
		}
		return mockForecast(p.Key, now), nil
	}

	// t. A valid get must answer with a full 8h/7d forecast for a Sao Paulo
	//    location, and the pager must cache it byte-for-byte.
	c.Handle("weather.get", getHandler)
	r, ok := fetch()
	want := mockForecast(key, now)
	var got forecastPayload
	if ok && !(r.Step == "weather" && r.OK && r.HourlyCount == 8 && r.DailyCount == 7 && r.City == "Sao Paulo") {
		ok = false
	}
	if ok {
		var err error
		got, err = readCache()
		if err != nil {
			fmt.Printf("       test.weather.cache: %v\n", err)
			ok = false
		} else if !(got.Key == key && got.City == "Sao Paulo" && len(got.Hourly) == 8 && len(got.Daily) == 7 &&
			reflect.DeepEqual(got.Hourly, want.Hourly) && reflect.DeepEqual(got.Daily, want.Daily)) {
			ok = false
		}
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
		fmt.Printf("       got  %+v\n", got)
	}
	pass = report(ok, "pager->phone weather.get (phone step): 8h/7d, city Sao Paulo, cached") && pass
	before := got

	// u. A reply for another key must be refused, and the cached copy must be
	//    unchanged (the pager keeps the forecast it already had).
	c.Handle("weather.get", func(json.RawMessage) (any, *pagerlink.RPCError) {
		other := mockForecast("other-city", now.Add(time.Hour))
		other.Hourly[0].Temp = other.Hourly[0].Temp + 5
		return other, nil
	})
	r, ok = fetch()
	if ok && !(r.Step == "weather" && !r.OK) {
		ok = false
	}
	var after forecastPayload
	if ok {
		var err error
		after, err = readCache()
		if err != nil {
			fmt.Printf("       test.weather.cache: %v\n", err)
			ok = false
		} else if !(after.Ts == before.Ts && reflect.DeepEqual(after.Hourly, before.Hourly) &&
			reflect.DeepEqual(after.Daily, before.Daily) && after.City == before.City) {
			ok = false
		}
	}
	if !ok {
		fmt.Printf("       evt.test.result %+v\n", r)
		fmt.Printf("       after %+v\n", after)
	}
	pass = report(ok, "pager->phone weather.get reply for another key -> refused, cache unchanged") && pass

	// Restore the standard handler for the rest of the smoke test.
	now = time.Now()
	c.Handle("weather.get", getHandler)

	return pass
}
