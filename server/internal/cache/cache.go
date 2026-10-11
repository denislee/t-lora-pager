// Package cache is a small in-memory TTL cache shared by upstream-proxying
// handlers. It is goroutine-safe and intentionally minimal: Get/Set/Sweep,
// plus a size cap (maxEntries) enforced on Set by dropping expired entries
// first, then the entry nearest expiry. No LRU tracking, so Get stays a
// read lock.
package cache

import (
	"sync"
	"time"
)

// maxEntries bounds how many responses the cache holds. Without it a client
// minting unique keys (e.g. junk query params on the weather forecast
// endpoint, whose key derives from the query string) grows the map without
// limit and can OOM the hub on a small box like a Pi.
const maxEntries = 512

type entry struct {
	value     []byte
	expiresAt time.Time
}

type Cache struct {
	mu    sync.RWMutex
	items map[string]entry
}

func New() *Cache {
	return &Cache{items: make(map[string]entry)}
}

func (c *Cache) Get(key string) ([]byte, bool) {
	c.mu.RLock()
	e, ok := c.items[key]
	c.mu.RUnlock()
	if !ok || time.Now().After(e.expiresAt) {
		return nil, false
	}
	return e.value, true
}

func (c *Cache) Set(key string, value []byte, ttl time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	// Overwriting an existing key doesn't grow the map, so only guard the cap
	// when inserting a genuinely new key.
	if _, exists := c.items[key]; !exists && len(c.items) >= maxEntries {
		c.evictLocked()
	}
	c.items[key] = entry{value: value, expiresAt: time.Now().Add(ttl)}
}

// evictLocked frees room for one new entry. It drops every expired entry first
// (always correct, usually enough); only if none were expired does it evict the
// entry nearest expiry — the closest approximation to "oldest" that doesn't
// require tracking access order, which would force a write lock on every Get.
// Caller holds c.mu.
func (c *Cache) evictLocked() {
	now := time.Now()
	removed := false
	for k, e := range c.items {
		if now.After(e.expiresAt) {
			delete(c.items, k)
			removed = true
		}
	}
	if removed {
		return
	}
	var oldestKey string
	var oldestExp time.Time
	first := true
	for k, e := range c.items {
		if first || e.expiresAt.Before(oldestExp) {
			oldestKey, oldestExp, first = k, e.expiresAt, false
		}
	}
	if !first {
		delete(c.items, oldestKey)
	}
}

func (c *Cache) Sweep() {
	now := time.Now()
	var expired []string

	c.mu.RLock()
	for k, e := range c.items {
		if now.After(e.expiresAt) {
			expired = append(expired, k)
		}
	}
	c.mu.RUnlock()

	if len(expired) > 0 {
		c.mu.Lock()
		for _, k := range expired {
			// Double check in case it was updated while lock was released
			if e, ok := c.items[k]; ok && now.After(e.expiresAt) {
				delete(c.items, k)
			}
		}
		c.mu.Unlock()
	}
}
