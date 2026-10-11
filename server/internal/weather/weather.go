// Package weather mounts the device-facing weather endpoints. Most are a
// near-pass-through of a public API the device used to call directly, so the
// JSON shape on the wire is unchanged — the device just swaps the host.
// geoSearch is the exception: it trims the response to the fields the device
// actually parses (see trimGeoSearch).
//
// Caching TTLs reflect upstream advice: open-meteo recommends <= 1 req/min,
// city geocoding is effectively static, and IP geo is stable as long as the
// public IP doesn't change.
package weather

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/lilygo/lilyhub/internal/cache"
	"github.com/lilygo/lilyhub/internal/httpx"
)

// ipAPIStatusOK rejects ip-api's 200-with-error responses. ip-api reports
// failure as {"status":"fail",...} at HTTP 200, which the generic proxy would
// otherwise cache as a success for the full 30-minute TTL — pinning a transient
// failure (rate limit, reserved range) and starving the device of a retry.
func ipAPIStatusOK(body []byte) error {
	var s struct {
		Status string `json:"status"`
	}
	if err := json.Unmarshal(body, &s); err != nil {
		return errors.New("unparseable ip-api body")
	}
	if s.Status != "success" {
		return fmt.Errorf("ip-api status %q", s.Status)
	}
	return nil
}

type Handler struct {
	cache  *cache.Cache
	client *http.Client
}

func New(c *cache.Cache) *Handler {
	return &Handler{
		cache: c,
		client: &http.Client{
			Timeout: 8 * time.Second,
		},
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/weather/geo/ip", h.geoIP)
	mux.HandleFunc("GET /api/weather/geo/search", h.geoSearch)
	mux.HandleFunc("GET /api/weather/forecast", h.forecast)
}

// geoIP — pass-through of ip-api.com. Same response shape as the device's
// previous call so the on-device JSON parser is untouched.
func (h *Handler) geoIP(w http.ResponseWriter, r *http.Request) {
	const upstream = "http://ip-api.com/json/?fields=status,lat,lon,city,regionName,country"
	httpx.Proxy(h.cache, h.client, w, r, "geoip", upstream, 30*time.Minute, ipAPIStatusOK, nil)
}

// geoSearch — trimmed proxy of geocoding-api.open-meteo.com/v1/search.
// The `name` query is the user-typed city; we forward `count` if given, else
// default to 10 (matches the device's old behavior).
func (h *Handler) geoSearch(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("name")
	if q == "" {
		http.Error(w, "missing name", http.StatusBadRequest)
		return
	}
	count := r.URL.Query().Get("count")
	if count == "" {
		count = "10"
	}
	upstream := "https://geocoding-api.open-meteo.com/v1/search?name=" +
		url.QueryEscape(q) + "&count=" + url.QueryEscape(count) + "&format=json"
	// Normalize the query for the key so "Paris" / "paris " / " PARIS" share
	// one entry (open-meteo geocoding is case-insensitive), instead of each
	// spelling minting a distinct 24h-pinned entry.
	key := "geosearch:" + strings.ToLower(strings.TrimSpace(q)) + ":" + count
	httpx.Proxy(h.cache, h.client, w, r, key, upstream, 24*time.Hour, nil, trimGeoSearch)
}

// geoSearchResult mirrors the subset of open-meteo's geocoding result object
// the device's two consumers actually read: fetch_geo_city (hub-routed,
// latitude/longitude only) and weather_search_cities (direct-to-open-meteo,
// also reads name/admin1/country for the match list label). Everything else
// open-meteo sends per result — elevation, timezone, feature_code, ids,
// population, postcodes, admin2/3/4 — is decoded here and dropped.
type geoSearchResult struct {
	Name      string  `json:"name"`
	Latitude  float64 `json:"latitude"`
	Longitude float64 `json:"longitude"`
	Country   string  `json:"country,omitempty"`
	Admin1    string  `json:"admin1,omitempty"`
}

// trimGeoSearch re-encodes an open-meteo geocoding response keeping only the
// fields geoSearchResult names. A missing/empty "results" array round-trips
// as an omitted field, which the device's cJSON_IsArray checks already treat
// as "no matches" the same as today's verbatim proxy.
func trimGeoSearch(body []byte) ([]byte, error) {
	var parsed struct {
		Results []geoSearchResult `json:"results,omitempty"`
	}
	if err := json.Unmarshal(body, &parsed); err != nil {
		return nil, err
	}
	return json.Marshal(parsed)
}

// forecast — pass-through of api.open-meteo.com/v1/forecast. We forward the
// raw query string verbatim rather than re-building it: the device may add or
// drop hourly/daily fields without server changes, and the cache key naturally
// follows whatever set of fields was requested.
func (h *Handler) forecast(w http.ResponseWriter, r *http.Request) {
	qs := r.URL.RawQuery
	q := r.URL.Query()
	if qs == "" || q.Get("latitude") == "" || q.Get("longitude") == "" {
		http.Error(w, "missing latitude/longitude", http.StatusBadRequest)
		return
	}
	upstream := "https://api.open-meteo.com/v1/forecast?" + qs
	httpx.Proxy(h.cache, h.client, w, r, forecastCacheKey(q), upstream, 10*time.Minute, nil, nil)
}

// forecastCacheKey builds a stable cache key from the forecast query so param
// reordering and sub-~1km coordinate jitter don't mint distinct entries (which
// would churn the cache and re-hit open-meteo needlessly). Every response-
// affecting param is kept — only latitude/longitude are rounded to 2 decimals
// — so two requests share a cached body only when they'd get the same forecast.
// The upstream URL still uses the raw query, so the fetched data stays exact.
func forecastCacheKey(q url.Values) string {
	canon := url.Values{}
	for k, vs := range q {
		if (k == "latitude" || k == "longitude") && len(vs) > 0 {
			if f, err := strconv.ParseFloat(vs[0], 64); err == nil {
				canon.Set(k, strconv.FormatFloat(f, 'f', 2, 64))
				continue
			}
		}
		canon[k] = vs
	}
	return "forecast:" + canon.Encode()
}
