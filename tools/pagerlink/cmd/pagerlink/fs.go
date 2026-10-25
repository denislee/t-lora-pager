package main

// The fs.* file-service commands and the smoke test's file-service section.
// Most fs.* calls are plain request/response like the sys.* calls in main.go;
// only fs.read and fs.write move bytes, through the bulk-transfer helpers in
// internal/pagerlink/files.go. Paths are validated by the pager's sandbox, so
// these commands pass the user's path through unchanged.

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"hash/crc32"
	"math/rand"
	"os"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

// fsEntry is one fs.list entry.
type fsEntry struct {
	Name  string `json:"name"`
	Dir   bool   `json:"dir"`
	Size  uint32 `json:"size"`
	Mtime uint32 `json:"mtime"`
}

// fsStat is the fs.stat result.
type fsStat struct {
	Dir       bool   `json:"dir"`
	Size      uint32 `json:"size"`
	Mtime     uint32 `json:"mtime"`
	Protected bool   `json:"protected"`
}

// fsList follows fs.list's cursor to the end and returns every entry. The pager
// pages at 64 entries (or fewer when a reply would grow too large) and omits
// "cursor" once the listing is complete.
func fsList(c *pagerlink.Client, timeout time.Duration, vol, path string) ([]fsEntry, error) {
	var all []fsEntry
	var cursor *uint32
	for {
		params := map[string]any{"vol": vol, "path": path}
		if cursor != nil {
			params["cursor"] = *cursor
		}
		res, err := c.Call("fs.list", params, timeout)
		if err != nil {
			return nil, err
		}
		var body struct {
			Entries []fsEntry `json:"entries"`
			Cursor  *uint32   `json:"cursor"`
		}
		if err := json.Unmarshal(res, &body); err != nil {
			return nil, err
		}
		all = append(all, body.Entries...)
		if body.Cursor == nil {
			return all, nil
		}
		cursor = body.Cursor
	}
}

// fsListNames returns the leaf names in dir, following the listing cursor.
func fsListNames(c *pagerlink.Client, timeout time.Duration, vol, path string) ([]string, error) {
	entries, err := fsList(c, timeout, vol, path)
	if err != nil {
		return nil, err
	}
	names := make([]string, len(entries))
	for i, e := range entries {
		names[i] = e.Name
	}
	return names, nil
}

// fsStatOf calls fs.stat and decodes the metadata.
func fsStatOf(c *pagerlink.Client, timeout time.Duration, vol, path string) (fsStat, error) {
	res, err := c.Call("fs.stat", map[string]any{"vol": vol, "path": path}, timeout)
	if err != nil {
		return fsStat{}, err
	}
	var st fsStat
	err = json.Unmarshal(res, &st)
	return st, err
}

// rpcCode returns err's ERR code, or "" for a non-RPC error.
func rpcCode(err error) string {
	var re *pagerlink.RPCError
	if errors.As(err, &re) {
		return re.Code
	}
	return ""
}

// cmdLS lists a directory, one entry per line: "d name" for a directory and
// "- size name" for a file.
func cmdLS(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink ls <vol> <path>")
		return 2
	}
	entries, err := fsList(c, timeout, args[0], args[1])
	if err != nil {
		return fail("fs.list", err)
	}
	for _, e := range entries {
		if e.Dir {
			fmt.Printf("d %s\n", e.Name)
		} else {
			fmt.Printf("- %d %s\n", e.Size, e.Name)
		}
	}
	return 0
}

// cmdStat prints fs.stat's metadata as pretty JSON.
func cmdStat(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink stat <vol> <path>")
		return 2
	}
	return callOne(c, timeout, "fs.stat", map[string]any{"vol": args[0], "path": args[1]})
}

// cmdGet downloads a file and writes it locally. The transfer timeout is at
// least two minutes: a whole file takes far longer than one RPC.
func cmdGet(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink get <vol> <path> <local-file>")
		return 2
	}
	data, _, err := c.ReadFile(args[0], args[1], max(timeout, 120*time.Second))
	if err != nil {
		return fail("fs.read", err)
	}
	if err := os.WriteFile(args[2], data, 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "pagerlink: write %s: %v\n", args[2], err)
		return 1
	}
	fmt.Printf("%d bytes, crc32 0x%08x\n", len(data), crc32.ChecksumIEEE(data))
	return 0
}

// cmdPut uploads a local file; -f allows overwriting an existing one.
func cmdPut(c *pagerlink.Client, timeout time.Duration, args []string) int {
	overwrite := false
	var pos []string
	for _, a := range args {
		if a == "-f" {
			overwrite = true
		} else {
			pos = append(pos, a)
		}
	}
	if len(pos) < 3 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink put <local-file> <vol> <path> [-f]")
		return 2
	}
	data, err := os.ReadFile(pos[0])
	if err != nil {
		fmt.Fprintf(os.Stderr, "pagerlink: read %s: %v\n", pos[0], err)
		return 1
	}
	if err := c.WriteFile(pos[1], pos[2], data, overwrite, timeout); err != nil {
		return fail("fs.write", err)
	}
	fmt.Printf("%d bytes, crc32 0x%08x\n", len(data), crc32.ChecksumIEEE(data))
	return 0
}

// cmdRM deletes a file or directory (recursively, on the pager).
func cmdRM(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink rm <vol> <path>")
		return 2
	}
	return callOne(c, timeout, "fs.delete", map[string]any{"vol": args[0], "path": args[1]})
}

// cmdMkdir creates a directory.
func cmdMkdir(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink mkdir <vol> <path>")
		return 2
	}
	return callOne(c, timeout, "fs.mkdir", map[string]any{"vol": args[0], "path": args[1]})
}

// cmdMV renames a file or directory.
func cmdMV(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink mv <vol> <from> <to>")
		return 2
	}
	return callOne(c, timeout, "fs.rename",
		map[string]any{"vol": args[0], "from": args[1], "to": args[2]})
}

// smokeFS exercises the fs.* file service: directory creation, a 1 MiB
// round-trip, metadata and listing, every negative control the handlers name, a
// rejected CRC, the evt.fs.changed notification, and the guarantee that a
// dropped upload never leaves a .part behind.
//
// The dropped-upload step deliberately closes the link (the emulator serves one
// client at a time) and reconnects, so the returned client may differ from c.
// The caller owns whichever client comes back.
func smokeFS(c *pagerlink.Client, addr string, timeout time.Duration) (*pagerlink.Client, bool) {
	pass := true
	dir := "/smoke-" + strconv.Itoa(os.Getpid())

	// a. A fresh scratch directory: remove any leftover, then create it.
	_, _ = c.Call("fs.delete", map[string]any{"vol": "int", "path": dir}, timeout)
	_, err := c.Call("fs.mkdir", map[string]any{"vol": "int", "path": dir}, timeout)
	if err != nil {
		fmt.Printf("       fs.mkdir: %v\n", err)
	}
	pass = report(err == nil, "fs.mkdir %s", dir) && pass
	if err != nil {
		return c, false
	}

	// b. 1 MiB of deterministic bytes: write, read back, compare bytes and CRC.
	rng := rand.New(rand.NewSource(42))
	big := make([]byte, 1<<20)
	rng.Read(big)
	want := crc32.ChecksumIEEE(big)
	err = c.WriteFile("int", dir+"/big.bin", big, false, timeout)
	var got []byte
	var info pagerlink.FileInfo
	if err == nil {
		got, info, err = c.ReadFile("int", dir+"/big.bin", timeout)
	}
	if err == nil && !bytes.Equal(got, big) {
		err = errors.New("read-back bytes differ")
	}
	if err == nil && info.CRC32 != want {
		err = fmt.Errorf("read-back crc 0x%08x, want 0x%08x", info.CRC32, want)
	}
	if err != nil {
		fmt.Printf("       %v\n", err)
	}
	pass = report(err == nil, "fs 1 MiB round trip (crc 0x%08x)", want) && pass

	// c. fs.stat sees the file and its exact size.
	st, serr := fsStatOf(c, timeout, "int", dir+"/big.bin")
	if serr != nil {
		fmt.Printf("       fs.stat: %v\n", serr)
	}
	pass = report(serr == nil && st.Size == 1<<20 && !st.Dir,
		"fs.stat big.bin size=%d dir=%v", st.Size, st.Dir) && pass

	// d. fs.list finds big.bin and no staging file.
	names, lerr := fsListNames(c, timeout, "int", dir)
	if lerr != nil {
		fmt.Printf("       fs.list: %v\n", lerr)
	}
	isPart := func(n string) bool { return strings.HasSuffix(n, ".part") }
	ok := lerr == nil && slices.Contains(names, "big.bin") && !slices.ContainsFunc(names, isPart)
	pass = report(ok, "fs.list shows big.bin and no .part") && pass

	// e. A second write without overwrite must be refused.
	err = c.WriteFile("int", dir+"/big.bin", []byte("x"), false, timeout)
	if rpcCode(err) != "invalid_argument" {
		fmt.Printf("       fs.write: got %v, want ERR invalid_argument (exists)\n", err)
	}
	pass = report(rpcCode(err) == "invalid_argument",
		"fs.write existing without overwrite -> invalid_argument (exists)") && pass

	// f. Rename, then confirm the old name is gone and the new one is there.
	_, rerr := c.Call("fs.rename",
		map[string]any{"vol": "int", "from": dir + "/big.bin", "to": dir + "/big2.bin"}, timeout)
	_, oerr := c.Call("fs.stat", map[string]any{"vol": "int", "path": dir + "/big.bin"}, timeout)
	_, nerr := c.Call("fs.stat", map[string]any{"vol": "int", "path": dir + "/big2.bin"}, timeout)
	ok = rerr == nil && rpcCode(oerr) == "invalid_argument" && nerr == nil
	if !ok {
		fmt.Printf("       rename=%v old=%v new=%v\n", rerr, oerr, nerr)
	}
	pass = report(ok, "fs.rename big.bin -> big2.bin, old name gone") && pass

	// g. Path sandbox: each of these must be rejected before any I/O.
	pass = smokeSandbox(c, timeout, dir) && pass

	// h. A commit whose CRC does not match must fail and leave nothing behind.
	pass = report(smokeBadCRC(c, timeout, dir),
		"fs.write bad CRC -> invalid_argument, no .part") && pass

	// i. The pager announces filesystem changes on evt.fs.changed.
	_, ok = waitEvent(c, "evt.fs.changed", 3*time.Second)
	pass = report(ok, "evt.fs.changed received") && pass

	// j. A dropped upload must leave neither the file nor a .part behind. This
	// closes the link and reconnects, so c may change.
	src := big[:64*1024]
	id, jerr := c.BeginWrite("int", dir+"/drop.bin", uint32(len(src)),
		crc32.ChecksumIEEE(src), false, timeout)
	if jerr == nil {
		for off := 0; off < 8192; off += pagerlink.MaxDataChunk {
			end := off + pagerlink.MaxDataChunk
			if end > 8192 {
				end = 8192
			}
			if jerr = c.SendData(id, uint32(off), src[off:end]); jerr != nil {
				break
			}
		}
	}
	if jerr == nil {
		time.Sleep(100 * time.Millisecond)
		c.Close()
		nc, derr := redial(addr, timeout, 3*time.Second)
		if derr != nil {
			fmt.Printf("       redial: %v\n", derr)
			return c, false
		}
		c = nc
		names, lerr = fsListNames(c, timeout, "int", dir)
		switch {
		case lerr != nil:
			fmt.Printf("       fs.list: %v\n", lerr)
			jerr = lerr
		case slices.Contains(names, "drop.bin") || slices.Contains(names, "drop.bin.part"):
			jerr = errors.New("drop.bin or drop.bin.part still listed")
		}
	}
	if jerr != nil {
		fmt.Printf("       %v\n", jerr)
	}
	pass = report(jerr == nil, "fs dropped upload leaves no partial file") && pass

	// k. Clean up the scratch directory.
	_, derr := c.Call("fs.delete", map[string]any{"vol": "int", "path": dir}, timeout)
	_, kerr := c.Call("fs.stat", map[string]any{"vol": "int", "path": dir}, timeout)
	ok = derr == nil && rpcCode(kerr) == "invalid_argument"
	if !ok {
		fmt.Printf("       fs.delete=%v fs.stat=%v\n", derr, kerr)
	}
	pass = report(ok, "fs.delete %s removes it", dir) && pass

	return c, pass
}

// smokeSandbox checks that fs.* rejects paths trying to escape the volume root,
// each with ERR invalid_argument and before touching the filesystem.
func smokeSandbox(c *pagerlink.Client, timeout time.Duration, dir string) bool {
	pass := true
	cases := []struct {
		method string
		params map[string]any
	}{
		{"fs.stat", map[string]any{"vol": "int", "path": "/../etc/passwd"}},
		{"fs.list", map[string]any{"vol": "int", "path": ".."}},
		{"fs.read", map[string]any{"vol": "int", "path": "a/../../x", "xfer": 2}},
		{"fs.mkdir", map[string]any{"vol": "int", "path": dir + "/../../escape"}},
	}
	for _, tc := range cases {
		_, err := c.Call(tc.method, tc.params, timeout)
		if rpcCode(err) != "invalid_argument" {
			fmt.Printf("       %s: got %v, want ERR invalid_argument\n", tc.method, err)
		}
		pass = report(rpcCode(err) == "invalid_argument",
			"%s sandbox escape -> invalid_argument", tc.method) && pass
	}
	return pass
}

// smokeBadCRC begins an upload with a deliberately wrong CRC, sends the bytes,
// and confirms the commit is refused and no file or .part survives.
func smokeBadCRC(c *pagerlink.Client, timeout time.Duration, dir string) bool {
	src := make([]byte, 1000)
	for i := range src {
		src[i] = byte(i)
	}
	id, err := c.BeginWrite("int", dir+"/bad.bin", uint32(len(src)), 0xDEADBEEF, false, timeout)
	if err != nil {
		fmt.Printf("       fs.write.begin: %v\n", err)
		return false
	}
	if err := c.SendData(id, 0, src); err != nil {
		fmt.Printf("       fs.write data: %v\n", err)
		return false
	}
	time.Sleep(200 * time.Millisecond)

	_, err = c.Call("fs.write.commit", map[string]any{"xfer": id}, timeout)
	if rpcCode(err) != "invalid_argument" {
		fmt.Printf("       fs.write.commit: got %v, want invalid_argument\n", err)
		return false
	}
	_, err = c.Call("fs.stat", map[string]any{"vol": "int", "path": dir + "/bad.bin"}, timeout)
	if rpcCode(err) != "invalid_argument" {
		fmt.Printf("       fs.stat bad.bin: got %v, want invalid_argument\n", err)
		return false
	}
	names, err := fsListNames(c, timeout, "int", dir)
	if err != nil {
		fmt.Printf("       fs.list: %v\n", err)
		return false
	}
	if slices.Contains(names, "bad.bin.part") {
		fmt.Printf("       fs.list still shows bad.bin.part\n")
		return false
	}
	return true
}

// redial retries Dial until it succeeds or within elapses. The emulator serves
// one client at a time, so a reconnect right after a close needs a moment.
func redial(addr string, timeout, within time.Duration) (*pagerlink.Client, error) {
	deadline := time.Now().Add(within)
	for {
		c, err := pagerlink.Dial(addr, timeout)
		if err == nil {
			return c, nil
		}
		if time.Now().After(deadline) {
			return nil, err
		}
		time.Sleep(100 * time.Millisecond)
	}
}
