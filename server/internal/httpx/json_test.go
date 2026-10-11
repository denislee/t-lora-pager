package httpx

import (
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
)

func TestDecodeJSONOK(t *testing.T) {
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/", strings.NewReader(`{"a":1}`))

	var v struct {
		A int `json:"a"`
	}
	if err := DecodeJSON(rec, req, 1024, &v); err != nil {
		t.Fatalf("DecodeJSON: unexpected error: %v", err)
	}
	if v.A != 1 {
		t.Fatalf("A = %d, want 1", v.A)
	}
	if rec.Code != http.StatusOK {
		t.Fatalf("recorder code = %d, want %d (nothing should be written)", rec.Code, http.StatusOK)
	}
	if rec.Body.Len() != 0 {
		t.Fatalf("recorder body = %q, want empty", rec.Body.String())
	}
}

func TestDecodeJSONRejectsUnknownField(t *testing.T) {
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/", strings.NewReader(`{"a":1,"b":2}`))

	var v struct {
		A int `json:"a"`
	}
	err := DecodeJSON(rec, req, 1024, &v)
	if err == nil {
		t.Fatal("DecodeJSON: expected error for unknown field, got nil")
	}
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("recorder code = %d, want %d", rec.Code, http.StatusBadRequest)
	}
	if !strings.HasPrefix(rec.Body.String(), "bad json:") {
		t.Fatalf("recorder body = %q, want it to start with %q", rec.Body.String(), "bad json:")
	}
}

func TestDecodeJSONRespectsLimit(t *testing.T) {
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/", strings.NewReader(`{"a":1}`))

	var v struct {
		A int `json:"a"`
	}
	err := DecodeJSON(rec, req, 3, &v)
	if err == nil {
		t.Fatal("DecodeJSON: expected error when body exceeds limit, got nil")
	}
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("recorder code = %d, want %d", rec.Code, http.StatusBadRequest)
	}
}

func TestWriteJSONSetsLengthAndClose(t *testing.T) {
	rec := httptest.NewRecorder()
	WriteJSON(rec, map[string]int{"x": 1})

	if got := rec.Header().Get("Content-Type"); got != "application/json" {
		t.Fatalf("Content-Type = %q, want %q", got, "application/json")
	}
	if got := rec.Header().Get("Connection"); got != "close" {
		t.Fatalf("Connection = %q, want %q", got, "close")
	}
	if got := rec.Header().Get("Content-Length"); got != strconv.Itoa(rec.Body.Len()) {
		t.Fatalf("Content-Length = %q, want %q", got, strconv.Itoa(rec.Body.Len()))
	}
	if rec.Code != http.StatusOK {
		t.Fatalf("recorder code = %d, want %d", rec.Code, http.StatusOK)
	}
	if got, want := rec.Body.String(), "{\"x\":1}\n"; got != want {
		t.Fatalf("body = %q, want %q", got, want)
	}
}

func TestWriteJSONMarshalError(t *testing.T) {
	rec := httptest.NewRecorder()
	WriteJSON(rec, make(chan int))

	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("recorder code = %d, want %d", rec.Code, http.StatusInternalServerError)
	}
}
