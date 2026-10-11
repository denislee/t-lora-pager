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
