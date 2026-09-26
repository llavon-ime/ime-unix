# llavon-ime-memscan

A deliberately small helper that finds a text in the memory of the user's own
processes and returns the text in front of it. It is the last context source
of the input method for applications that expose neither client surrounding
text nor AT-SPI (for example Chromium, CEF, terminals, Tk).

The input method passes the natural composition it is showing. The private
resident mode returns bounded candidate locations; the engine only trusts a
location after it follows two natural composition changes with the same text
in front. No marker is inserted into the displayed composition. Clients that
keep preedit outside their document buffer may never yield usable context.

The helper is intentionally *not* a generic memory reader:

* the anchor has to be valid UTF-8 with 1..64 code points and no control
  characters (the input method passes its composition);
* only PIDs owned by the calling user are accepted (the caller names them);
* only a bounded window (at most 64 KiB each side, default 4 KiB before) in
  front of the anchor is returned, trimmed to the trailing run of plausible
  document text so heap metadata never reaches the model;
* scanning is bounded in bytes and wall-clock time.

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
`hint_pid`/`hint_address`; the previous match is checked before a full scan.

Output is one JSON object on stdout:

```json
{"found":true,"pid":1234,"encoding":"utf16le","address":"0x5555a1b2c3d4",
 "before":"hello magic ","after":"seed line\n",
 "before_bytes":12,"after_bytes":11,"scanned_bytes":4194304,"elapsed_ms":7}
```

Exit codes: `0` found, `1` not found, `2` usage/anchor error, `3` permission
denied or foreign PID, `4` timeout/byte budget. A permission failure is
reported as `{"found":false,"error":"denied"}`, which the engine surfaces as
"memory context unavailable" instead of pretending the anchor was absent.

`--hint <pid>:<start>-<end>` points the scanner at a region that contained the
anchor during the previous probe (the engine caches it), so repeat probes stay
fast.

Setting `MEMSCAN_DEBUG=1` prints the raw window bytes of a hit to stderr; it is
meant for development only.

## Permission

Reading another process needs the same UID plus ptrace permission. Yama
(`kernel.yama.ptrace_scope=1`) refuses non-descendant access, so the helper
needs either `CAP_SYS_PTRACE`

```sh
sudo setcap cap_sys_ptrace+ep /usr/libexec/llavon-ime/llavon-ime-memscan
```

or `kernel.yama.ptrace_scope=0`. Descendant processes (the test suite) need
neither.
