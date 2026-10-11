package httpx

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"strconv"
)

// DecodeJSON decodes r's body into v, reading at most limit bytes and
// rejecting unknown fields. On failure it has already written a 400
// "bad json: ..." reply; the error is returned so the caller can log it and
// must simply return.
func DecodeJSON(w http.ResponseWriter, r *http.Request, limit int64, v any) error {
	dec := json.NewDecoder(io.LimitReader(r.Body, limit))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return err
	}
	return nil
}

// WriteJSON marshals v up-front so it can set Content-Length explicitly, and
// forces Connection: close. The Arduino HTTPClient on ESP32 has been observed
// to hang in getString() when relying solely on chunked-EOF or implicit
// content-length signalling — giving it both a length and a
// close-after-response makes the read deterministic. Every JSON reply the
// device reads must go through here.
func WriteJSON(w http.ResponseWriter, v any) {
	body, err := json.Marshal(v)
	if err != nil {
		log.Printf("httpx: WriteJSON marshal error: %v", err)
		http.Error(w, "internal", http.StatusInternalServerError)
		return
	}
	body = append(body, '\n')
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.Header().Set("Connection", "close")
	if _, err := w.Write(body); err != nil {
		log.Printf("httpx: WriteJSON write error: %v", err)
	}
}
