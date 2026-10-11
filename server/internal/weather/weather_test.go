package weather

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/lilygo/lilyhub/internal/cache"
)

func TestIPAPIStatusOK(t *testing.T) {
	cases := []struct {
		name string
		body string
		ok   bool
	}{
		{"success", `{"status":"success","city":"Kyiv","lat":50.4}`, true},
		{"fail", `{"status":"fail","message":"private range"}`, false},
		{"missing status", `{"city":"Kyiv"}`, false},
		{"garbage", `not json at all`, false},
		{"empty", ``, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := ipAPIStatusOK([]byte(tc.body))
			if tc.ok && err != nil {
				t.Fatalf("want accept, got error: %v", err)
			}
			if !tc.ok && err == nil {
				t.Fatal("want reject, got nil error (would be cached as success)")
			}
		})
	}
}

// A realistic open-meteo geocoding result, trimmed of everything the two
// device parsers (fetch_geo_city, weather_search_cities in ui_weather.cpp)
// don't read.
func TestTrimGeoSearchDropsUnusedFields(t *testing.T) {
	in := `{
		"results": [{
			"id": 2988507, "name": "Paris", "latitude": 48.85341, "longitude": 2.3488,
			"elevation": 42.0, "feature_code": "PPLC", "country_code": "FR",
			"admin1_id": 3012874, "admin1": "Île-de-France", "country": "France",
			"country_id": 3017382, "population": 2138551, "timezone": "Europe/Paris",
			"postcodes": ["75000","75001"]
		}],
		"generationtime_ms": 0.5
	}`
	out, err := trimGeoSearch([]byte(in))
	if err != nil {
		t.Fatalf("trimGeoSearch: %v", err)
	}

	var parsed struct {
		Results []geoSearchResult `json:"results"`
	}
	if err := json.Unmarshal(out, &parsed); err != nil {
		t.Fatalf("output not valid JSON: %v", err)
	}
	if len(parsed.Results) != 1 {
		t.Fatalf("got %d results, want 1", len(parsed.Results))
	}
	got := parsed.Results[0]
	want := geoSearchResult{Name: "Paris", Latitude: 48.85341, Longitude: 2.3488, Country: "France", Admin1: "Île-de-France"}
	if got != want {
		t.Fatalf("got %+v, want %+v", got, want)
	}

	// Dropped fields must not survive as unrecognized-but-present keys.
	for _, dead := range []string{"elevation", "feature_code", "country_code", "admin1_id", "country_id", "population", "timezone", "postcodes", "generationtime_ms", "\"id\""} {
		if strings.Contains(string(out), dead) {
			t.Fatalf("trimmed output still contains %q: %s", dead, out)
		}
	}
}

func TestTrimGeoSearchEmptyResults(t *testing.T) {
	out, err := trimGeoSearch([]byte(`{"generationtime_ms": 0.1}`))
	if err != nil {
		t.Fatalf("trimGeoSearch: %v", err)
	}
	var parsed struct {
		Results []geoSearchResult `json:"results"`
	}
	if err := json.Unmarshal(out, &parsed); err != nil {
		t.Fatalf("output not valid JSON: %v", err)
	}
	if len(parsed.Results) != 0 {
		t.Fatalf("got %d results, want 0", len(parsed.Results))
	}
}

func TestTrimGeoSearchInvalidJSON(t *testing.T) {
	if _, err := trimGeoSearch([]byte(`not json`)); err == nil {
		t.Fatal("want error for invalid upstream JSON, got nil")
	}
}

// Routes are GET-only Go 1.22 patterns; before, weather accepted any method.
// Validation failures are answered locally, without reaching upstream.
func TestRoutesMethodAndValidation(t *testing.T) {
	mux := http.NewServeMux()
	New(cache.New()).Register(mux)
	cases := []struct {
		method, target string
		want           int
	}{
		{http.MethodPost, "/api/weather/geo/ip", http.StatusMethodNotAllowed},
		{http.MethodPost, "/api/weather/forecast?latitude=1&longitude=2", http.StatusMethodNotAllowed},
		{http.MethodGet, "/api/weather/geo/search", http.StatusBadRequest},
		{http.MethodGet, "/api/weather/forecast?latitude=1", http.StatusBadRequest},
	}
	for _, c := range cases {
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, httptest.NewRequest(c.method, c.target, nil))
		if rec.Code != c.want {
			t.Errorf("%s %s: code=%d, want %d", c.method, c.target, rec.Code, c.want)
		}
	}
}
