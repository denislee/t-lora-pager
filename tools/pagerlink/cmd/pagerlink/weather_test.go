package main

// Tests for mockForecast, the canned weather.push/weather.get payload. They
// pin the invariants weather_payload_parse (src/hal/weather_payload.cpp)
// enforces, so an out-of-range mock can never be pushed in the smoke test.

import (
	"encoding/json"
	"reflect"
	"regexp"
	"strings"
	"testing"
	"time"
)

// TestMockForecast checks the shape and ranges of mockForecast and that it
// survives a JSON marshal/unmarshal round trip unchanged.
func TestMockForecast(t *testing.T) {
	now := time.Now()
	const key = "auto"
	want := mockForecast(key, now)

	if want.Key != key {
		t.Errorf("Key = %q, want %q", want.Key, key)
	}
	if want.Ts != uint32(now.Unix()) {
		t.Errorf("Ts = %d, want %d", want.Ts, uint32(now.Unix()))
	}

	if len(want.Hourly) != 8 {
		t.Fatalf("len(Hourly) = %d, want 8", len(want.Hourly))
	}
	for i, h := range want.Hourly {
		if h.Hr < 0 || h.Hr > 23 {
			t.Errorf("Hourly[%d].Hr = %d, want 0..23", i, h.Hr)
		}
		if h.Code < 0 || h.Code > 255 {
			t.Errorf("Hourly[%d].Code = %d, want 0..255", i, h.Code)
		}
		if h.Temp < -128 || h.Temp > 127 {
			t.Errorf("Hourly[%d].Temp = %d, want -128..127", i, h.Temp)
		}
		if h.Pop < 0 || h.Pop > 100 {
			t.Errorf("Hourly[%d].Pop = %d, want 0..100", i, h.Pop)
		}
	}

	if len(want.Daily) != 7 {
		t.Fatalf("len(Daily) = %d, want 7", len(want.Daily))
	}
	dateRE := regexp.MustCompile(`^\d{4}-\d{2}-\d{2}$`)
	for i, d := range want.Daily {
		if !dateRE.MatchString(d.Date) {
			t.Errorf("Daily[%d].Date = %q, want YYYY-MM-DD", i, d.Date)
		}
		if d.Code < 0 || d.Code > 255 {
			t.Errorf("Daily[%d].Code = %d, want 0..255", i, d.Code)
		}
		if d.Hi < -128 || d.Hi > 127 {
			t.Errorf("Daily[%d].Hi = %d, want -128..127", i, d.Hi)
		}
		if d.Lo < -128 || d.Lo > 127 {
			t.Errorf("Daily[%d].Lo = %d, want -128..127", i, d.Lo)
		}
		if d.Pop < 0 || d.Pop > 100 {
			t.Errorf("Daily[%d].Pop = %d, want 0..100", i, d.Pop)
		}
	}

	if want.City != "Sao Paulo" {
		t.Errorf("City = %q, want %q", want.City, "Sao Paulo")
	}
	if want.Lat == nil || *want.Lat != -23.55 {
		t.Errorf("Lat = %v, want -23.55", want.Lat)
	}
	if want.Lon == nil || *want.Lon != -46.63 {
		t.Errorf("Lon = %v, want -46.63", want.Lon)
	}

	raw, err := json.Marshal(want)
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	for _, k := range []string{`"city"`, `"lat"`, `"lon"`} {
		if !strings.Contains(string(raw), k) {
			t.Errorf("marshalled JSON missing %s: %s", k, raw)
		}
	}

	var got forecastPayload
	if err := json.Unmarshal(raw, &got); err != nil {
		t.Fatalf("Unmarshal: %v", err)
	}
	if !reflect.DeepEqual(want, got) {
		t.Errorf("round trip mismatch:\n want %+v\n  got %+v", want, got)
	}
}

// TestMockForecastNoGeoOmitted checks that an empty city and nil lat/lon are
// dropped from the JSON, so a geo-less forecast carries no city/lat/lon keys.
func TestMockForecastNoGeoOmitted(t *testing.T) {
	fc := forecastPayload{
		Key:    "auto",
		Ts:     1,
		Hourly: []wxHour{{Hr: 0, Code: 0, Temp: 18, Pop: 0}},
		Daily:  []wxDay{{Date: "2026-10-11", Code: 0, Hi: 28, Lo: 17, Pop: 0}},
	}
	raw, err := json.Marshal(fc)
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	for _, k := range []string{`"city"`, `"lat"`, `"lon"`} {
		if strings.Contains(string(raw), k) {
			t.Errorf("marshalled JSON unexpectedly has %s: %s", k, raw)
		}
	}
}
