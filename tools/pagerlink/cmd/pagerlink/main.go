// Command pagerlink is a small operator CLI for the PagerLink v1 TCP transport
// (protocol/PROTOCOL.md §5). It talks to a pager — usually the emulator on
// 127.0.0.1:7600 — so a human can send a Phase-1 RPC by hand, see the JSON
// result, and confirm the error paths behave.
//
// The `smoke` subcommand is the useful one: it walks the four system methods a
// pager must implement, checks each required error code, proves the link
// recovers after a deliberate sequence fault, exercises the fs.* file service
// end to end (fs.go), and then runs the pager-initiated steps — calls, bulk
// DATA, chat, notes sync, Telegram and weather via the phone — through the
// emulator-only test hooks (smoke_link.go, smoke_weather.go), so a full pass
// needs the emulator. The ls/stat/get/put/rm/mkdir/mv commands are hand tools
// for the file service.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/lilygo/pagerlink/internal/pagerlink"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:7600", "pagerlink TCP address")
	timeout := flag.Duration("timeout", 5*time.Second, "per-request timeout")
	flag.Usage = usage
	flag.Parse()

	args := flag.Args()
	if len(args) == 0 {
		usage()
		os.Exit(2)
	}
	cmd, rest := args[0], args[1:]
	run, ok := commands[cmd]
	if !ok {
		fmt.Fprintf(os.Stderr, "pagerlink: unknown command %q\n", cmd)
		usage()
		os.Exit(2)
	}

	c, err := pagerlink.Dial(*addr, *timeout)
	if err != nil {
		fmt.Fprintf(os.Stderr, "pagerlink: dial %s: %v\n", *addr, err)
		os.Exit(1)
	}

	code := run(c, *addr, *timeout, rest)
	c.Close()
	os.Exit(code)
}

// command runs one subcommand on a dialed client and returns the exit code.
type command func(c *pagerlink.Client, addr string, timeout time.Duration, args []string) int

// commands is the single table main validates against and dispatches from.
var commands = map[string]command{
	"hello": func(c *pagerlink.Client, _ string, timeout time.Duration, _ []string) int {
		return callOne(c, timeout, "sys.hello",
			map[string]any{"proto": 1, "app": "pagerlink-cli", "caps": []string{"sys"}})
	},
	"info": func(c *pagerlink.Client, _ string, timeout time.Duration, _ []string) int {
		return callOne(c, timeout, "sys.info", nil)
	},
	"ping": noAddr(func(c *pagerlink.Client, timeout time.Duration, _ []string) int { return cmdPing(c, timeout) }),
	"time": noAddr(cmdTime),
	"call": noAddr(cmdCall),
	"smoke": func(c *pagerlink.Client, addr string, timeout time.Duration, _ []string) int {
		return cmdSmoke(c, addr, timeout)
	},
	"serve": cmdServe,
	"ls":    noAddr(cmdLS),
	"stat":  noAddr(cmdStat),
	"get":   noAddr(cmdGet),
	"put":   noAddr(cmdPut),
	"rm":    noAddr(cmdRM),
	"mkdir": noAddr(cmdMkdir),
	"mv":    noAddr(cmdMV),
}

// noAddr adapts a subcommand that does not need the dial address.
func noAddr(f func(*pagerlink.Client, time.Duration, []string) int) command {
	return func(c *pagerlink.Client, _ string, timeout time.Duration, args []string) int {
		return f(c, timeout, args)
	}
}

func usage() {
	fmt.Fprint(os.Stderr, `usage: pagerlink [-addr 127.0.0.1:7600] [-timeout 5s] <cmd>

Commands:
  hello                          sys.hello with proto 1
  info                           sys.info
  ping                           sys.ping and the measured RTT
  time [-tz ZONE]                sys.time.set with the current epoch
  call <method> [json-params]    arbitrary RPC
  smoke                          end-to-end check: sys, fs and pager->phone (emulator)
  serve [-responses f.json] [-notes-out dir]  mock phone for emulator work
  ls <vol> <path>                list a directory (fs.list, paged)
  stat <vol> <path>              show file metadata (fs.stat)
  get <vol> <path> <local-file>  download a file (fs.read)
  put <local-file> <vol> <path> [-f]  upload a file (fs.write; -f overwrites)
  rm <vol> <path>                delete a file or directory (fs.delete)
  mkdir <vol> <path>             create a directory (fs.mkdir)
  mv <vol> <from> <to>           rename a file or directory (fs.rename)
`)
}

// callOne sends one request, prints the pretty JSON result, and returns the
// process exit code.
func callOne(c *pagerlink.Client, timeout time.Duration, method string, params any) int {
	res, err := c.Call(method, params, timeout)
	if err != nil {
		return fail(method, err)
	}
	printJSON(method, res)
	return 0
}

func cmdPing(c *pagerlink.Client, timeout time.Duration) int {
	start := time.Now()
	res, err := c.Call("sys.ping", map[string]int64{"t": start.UnixMilli()}, timeout)
	if err != nil {
		return fail("sys.ping", err)
	}
	printJSON("sys.ping", res)
	fmt.Printf("rtt: %s\n", time.Since(start).Round(time.Microsecond))
	return 0
}

func cmdTime(c *pagerlink.Client, timeout time.Duration, args []string) int {
	fs := flag.NewFlagSet("time", flag.ContinueOnError)
	tz := fs.String("tz", defaultTZ(), "IANA time zone")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	return callOne(c, timeout, "sys.time.set",
		map[string]any{"epoch": time.Now().Unix(), "tz": *tz})
}

func cmdCall(c *pagerlink.Client, timeout time.Duration, args []string) int {
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: pagerlink call <method> [json-params]")
		return 2
	}
	method := args[0]
	var params any
	if len(args) > 1 {
		if err := json.Unmarshal([]byte(args[1]), &params); err != nil {
			fmt.Fprintf(os.Stderr, "pagerlink: invalid params JSON: %v\n", err)
			return 2
		}
	}
	return callOne(c, timeout, method, params)
}

// cmdSmoke is the CI-friendly end-to-end check. Every step prints PASS or FAIL;
// the process exits 1 if any step failed. smokeFS may reconnect after a
// deliberate drop, so close the replacement client it hands back.
func cmdSmoke(c *pagerlink.Client, addr string, timeout time.Duration) int {
	pass := true

	// Positive path: the Phase-1 methods the pager must implement.
	pass = report(callOK(c, timeout, "sys.hello",
		map[string]any{"proto": 1, "app": "pagerlink-cli", "caps": []string{"sys"}}),
		"sys.hello") && pass
	pass = report(callOK(c, timeout, "sys.info", nil), "sys.info") && pass
	pass = report(pingOK(c, timeout), "sys.ping") && pass
	pass = report(callOK(c, timeout, "sys.time.set",
		map[string]any{"epoch": time.Now().Unix(), "tz": "America/Sao_Paulo"}),
		"sys.time.set America/Sao_Paulo") && pass

	// Negative controls: each must fail with the named ERR code.
	pass = report(rpcErrCode(c, timeout, "nope.unknown", nil, "unsupported"),
		"unknown method -> unsupported") && pass
	pass = report(rpcErrCode(c, timeout, "sys.time.set",
		map[string]any{"epoch": 5, "tz": "UTC"}, "invalid_argument"),
		"sys.time.set epoch=5 -> invalid_argument") && pass
	pass = report(rpcErrCode(c, timeout, "sys.time.set",
		map[string]any{"epoch": 1800000000, "tz": "bad tz!"}, "invalid_argument"),
		"sys.time.set bad tz -> invalid_argument") && pass
	pass = report(rpcErrCode(c, timeout, "sys.hello",
		map[string]any{"proto": 2}, "unsupported"),
		"sys.hello proto=2 -> unsupported") && pass

	// Recovery: inject a packet with the wrong seq, then confirm a normal ping
	// still works. The peer's reassembler resets on the gap and resyncs at the
	// next START, so the link must come back without any reconnect.
	pass = report(injectBadSeq(c), "inject wrong-seq packet") && pass
	pass = report(pingOK(c, timeout), "sys.ping after reset") && pass

	// File service: directories, bulk transfer, sandboxing, CRC integrity,
	// events, and crash recovery. The dropped-upload step reconnects, so the
	// pager-initiated steps below and the final close both use the replacement
	// client smokeFS hands back.
	next, fsOK := smokeFS(c, addr, timeout)
	pass = fsOK && pass
	pass = smokeLink(next, timeout) && pass
	if next != c {
		next.Close()
	}

	if !pass {
		return 1
	}
	fmt.Println("smoke: all steps passed")
	return 0
}

// callOK performs one request and prints its result. A nil error is a pass.
func callOK(c *pagerlink.Client, timeout time.Duration, method string, params any) bool {
	res, err := c.Call(method, params, timeout)
	if err != nil {
		fmt.Printf("       %s: %v\n", method, err)
		return false
	}
	printJSON("       "+method, res)
	return true
}

// pingOK sends sys.ping and reports the RTT.
func pingOK(c *pagerlink.Client, timeout time.Duration) bool {
	start := time.Now()
	res, err := c.Call("sys.ping", map[string]int64{"t": start.UnixMilli()}, timeout)
	if err != nil {
		fmt.Printf("       sys.ping: %v\n", err)
		return false
	}
	printJSON(fmt.Sprintf("       sys.ping (rtt %s)", time.Since(start).Round(time.Microsecond)), res)
	return true
}

// rpcErrCode asserts the call fails with the given ERR code.
func rpcErrCode(c *pagerlink.Client, timeout time.Duration, method string, params any, code string) bool {
	_, err := c.Call(method, params, timeout)
	var re *pagerlink.RPCError
	if !errors.As(err, &re) {
		fmt.Printf("       %s: got %v, want ERR %s\n", method, err, code)
		return false
	}
	if re.Code != code {
		fmt.Printf("       %s: got ERR %s, want %s\n", method, re.Code, code)
		return false
	}
	fmt.Printf("       %s -> ERR %s\n", method, re.Code)
	return true
}

// injectBadSeq sends one record holding a START|END packet whose seq (40) is
// almost certainly out of step, carrying a valid ping message.
func injectBadSeq(c *pagerlink.Client) bool {
	raw, err := pagerlink.Message{
		Type:    pagerlink.Req,
		ID:      2,
		Payload: []byte(`{"m":"sys.ping","p":{"t":1}}`),
	}.Encode()
	if err != nil {
		fmt.Printf("       encode: %v\n", err)
		return false
	}
	pkt := make([]byte, 0, 1+len(raw))
	pkt = append(pkt, pagerlink.StartFlag|pagerlink.EndFlag|40)
	pkt = append(pkt, raw...)
	if err := c.SendRaw(pkt); err != nil {
		fmt.Printf("       SendRaw: %v\n", err)
		return false
	}
	fmt.Printf("        sent %d-byte packet with seq 40\n", len(pkt))
	return true
}

func report(ok bool, format string, args ...any) bool {
	status := "PASS"
	if !ok {
		status = "FAIL"
	}
	fmt.Printf("[%s] %s\n", status, fmt.Sprintf(format, args...))
	return ok
}

func fail(method string, err error) int {
	fmt.Fprintf(os.Stderr, "pagerlink: %s: %v\n", method, err)
	return 1
}

// printJSON indents a JSON result on one labelled block; a non-JSON payload is
// echoed raw rather than swallowed.
func printJSON(label string, raw json.RawMessage) {
	var buf bytes.Buffer
	if err := json.Indent(&buf, raw, "", "  "); err != nil {
		fmt.Printf("%s: %s\n", label, raw)
		return
	}
	fmt.Printf("%s: %s\n", label, buf.String())
}

// defaultTZ mirrors the device: honour $TZ when set, else UTC.
func defaultTZ() string {
	if tz := os.Getenv("TZ"); tz != "" {
		return tz
	}
	return "UTC"
}
