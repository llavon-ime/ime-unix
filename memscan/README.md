# llavon-ime-memscan

A deliberately small helper that finds a text in the memory of the user's own
processes and returns the text in front of it. It is the last context source
of the input method for applications that expose neither client surrounding
text nor AT-SPI. Whether a particular client yields context depends on how it
stores its document and preedit.

The input method passes the natural composition it is showing. The private
resident mode returns bounded candidate locations; the engine only trusts a
location after it follows two natural composition changes with the same text
in front. No marker is inserted into the displayed composition. Clients that
keep preedit outside their document buffer may never yield usable context.

The helper is intentionally *not* a generic memory reader:

* the anchor has to be valid UTF-8 with 1..64 code points and no control
  characters (the input method passes its composition);
* only PIDs owned by the calling user are accepted (the caller names them);
* only a bounded window (at most 64 KiB each side, default 4 KiB before in
  one-shot mode, 256 bytes before in resident mode) is decoded; implausible
  heap bytes are trimmed, and a candidate is not published without temporal
  validation;
* scanning is bounded in bytes and wall-clock time.

Text is looked for as UTF-8, UTF-16LE, UTF-32LE, and as the cell grids
terminal emulators use: 8-byte cells (VTE, st), 12-byte cells (kitty, foot),
16-byte cells (Konsole), and 24-byte cells (Alacritty). Each cell starts with
a UTF-32 code point followed by attributes; wide characters use a repeated
code point, a spacer above the Unicode range, or a blank continuation cell,
and the match walks the following cells so attribute bytes never have to
match. A hit whose following text continues the same word may be a prefix of
a static string rather than the caret, so it is kept as a continuation
candidate and only trusted when the text after it stays the same across
composition states; a static string's tail shrinks as the composition grows.
This is what lets the probe find a caret in the middle of existing text.
The engine's raw-key suite `memory formats validate through raw keys`
validates every format end-to-end when `LLAVON_IME_TEST_MEMSCAN` names a
built helper.

## Build and test

```sh
cmake -S memscan -B build/memscan -DCMAKE_BUILD_TYPE=Release
cmake --build build/memscan --parallel
ctest --test-dir build/memscan --output-on-failure
```

## Usage

```sh
llavon-ime-memscan --anchor "<composition text>" --pid 1234 [--pid 5678]
                   [--before 4096] [--after 512]
                   [--hint 1234:0x55550000-0x55560000]
                   [--timeout-ms 3000] [--max-bytes 536870912]
```

The one-shot CLI still returns the best matching window. The IME's private
`--serve` mode reads one JSON request per line from stdin and replies with a
bounded list of candidate matches. It is a child process owned by the IME,
not a system service. Requests name explicit PIDs and may include a cached
`hint_pid`/`hint_address`/`hint_before` only after a location is confirmed.
A nearby but different copy does not prevent the fallback full scan. A
`{"prime": true, "pids": [...]}` request only resets the soft-dirty baseline
so the first real scan is a changed-pages scan instead of a full one; the
input method sends it when a client gets focus. The first scan after a prime
does not trust an empty dirty set, because the client may have written its
first composition before the baseline was reset; that one request keeps the
full region list.

Output is one JSON object on stdout:

```json
{"found":true,"pid":1234,"encoding":"utf16le","address":"0x5555a1b2c3d4",
 "before":"hello magic ","after":"seed line\n",
 "before_bytes":12,"after_bytes":11,"scanned_bytes":4194304,"truncated":false}
```

Exit codes: `0` found, `1` not found, `2` usage/anchor error, `3` permission
denied or foreign PID, `4` timeout/byte budget. A permission failure is
reported as `{"found":false,"error":"denied"}`, which the engine surfaces as
"memory context unavailable" instead of pretending the anchor was absent.

`--hint <pid>:<start>-<end>` points the one-shot scanner at a region to try
first. The resident mode only uses a previously confirmed location as a hint.

Building with `-DLLAVON_IME_DEBUG=ON` (the same switch the engine and the
fcitx5 addon use, set by `LLAVON_IME_DEBUG=1 scripts/build-linux.sh`) enables
`[CTX]`/`[MEMCTX]` lines on fcitx5 stderr, raw bytes around scanner hits, and
soft-dirty scan timings; those bytes may include private document text.

## Permission

Reading another process needs the same UID plus ptrace permission. Yama
(`kernel.yama.ptrace_scope=1`) refuses non-descendant access, so the helper
needs either `CAP_SYS_PTRACE`

```sh
sudo setcap cap_sys_ptrace+ep /usr/libexec/llavon-ime/llavon-ime-memscan
```

or `kernel.yama.ptrace_scope=0`. Descendant processes (the test suite) need
neither.
