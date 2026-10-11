package main

// The weather.* wire types and their mock builder (PROTOCOL.md weather.push /
// weather.get, IOS_COMPANION_PLAN.md §5.5/§6.3). The pager's weather.push
// handler (src/hal/link/handlers_weather.cpp) parses this ForecastPayload with
// hal/weather_payload.cpp, range-checks every field and stores it in the shared
// weather cache; weather.get answers with the same shape. The mock builder
// produces a fixed, in-range forecast so the smoke test can push it and read it
// back through test.weather.cache.

import "time"

// wxHour is one hourly forecast cell: the hour of day (0-23), the WMO weather
// code (0-255), the temperature in Celsius (int8 range) and the precipitation
// probability (0-100).
type wxHour struct {
	Hr   int `json:"hr"`
	Code int `json:"code"`
	Temp int `json:"temp"`
	Pop  int `json:"pop"`
}

// wxDay is one daily forecast cell: the date as "YYYY-MM-DD", the WMO weather
// code (0-255), the high and low temperatures in Celsius (int8 range) and the
// precipitation probability (0-100).
type wxDay struct {
	Date string `json:"date"`
	Code int    `json:"code"`
	Hi   int    `json:"hi"`
	Lo   int    `json:"lo"`
	Pop  int    `json:"pop"`
}

// forecastPayload is the weather.push/weather.get body (PROTOCOL.md §6,
// IOS_COMPANION_PLAN.md §6.3): the cache key, the fetch time, up to 8 hourly
// and 7 daily cells, and — when the phone supplied a full location — the city
// plus lat/lon. A key of "auto" means IP-based geolocation.
type forecastPayload struct {
	Key    string   `json:"key"`
	Ts     uint32   `json:"ts"`
	Hourly []wxHour `json:"hourly"`
	Daily  []wxDay  `json:"daily"`
	City   string   `json:"city,omitempty"`
	Lat    *float64 `json:"lat,omitempty"`
	Lon    *float64 `json:"lon,omitempty"`
}

// mockForecast builds the canned in-range forecast the mock phone and the smoke
// test exchange. It carries 8 hourly cells (hr 0,3,…,21) and 7 daily cells
// starting at now's UTC date, a Sao Paulo location, and the given cache key and
// timestamp; every value stays inside the ranges weather_payload_parse accepts.
func mockForecast(key string, now time.Time) forecastPayload {
	hourlyCodes := []int{0, 1, 2, 3, 45, 61, 71, 95}
	hourly := make([]wxHour, 8)
	for i := range hourly {
		hourly[i] = wxHour{Hr: i * 3, Code: hourlyCodes[i], Temp: 18 + i, Pop: i * 10}
	}

	dailyCodes := []int{0, 2, 3, 61, 63, 80, 95}
	daily := make([]wxDay, 7)
	for i := range daily {
		daily[i] = wxDay{
			Date: now.UTC().AddDate(0, 0, i).Format("2006-01-02"),
			Code: dailyCodes[i],
			Hi:   28 - i,
			Lo:   17 - i,
			Pop:  i * 12,
		}
	}

	lat, lon := -23.55, -46.63
	return forecastPayload{
		Key:    key,
		Ts:     uint32(now.Unix()),
		Hourly: hourly,
		Daily:  daily,
		City:   "Sao Paulo",
		Lat:    &lat,
		Lon:    &lon,
	}
}
