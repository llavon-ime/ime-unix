# Request-scoped burst boost — locked protocol, 2026-09-30

Question: does raising QoS only around a prediction burst reduce first-request
latency after idle, when actual CPU compute workers are targeted and restored?

Use the identical installed 250M Q4_K_M model and unmodified pinned ime-core.
Separate process configurations, randomized seed 20260930 and repeated rounds.
CPU/4, CPU/8 and full Metal/4; idle 0/600/2000 ms. Each observation comprises
four two-syllable predictions cycling four Chinese context suffixes to require
actual decode. Four unmeasured initial predictions prepare model/session state.

Controls and treatments:
- none: caller default, disposable worker QoS queried without changes;
- caller: caller USER_INITIATED only during a four-prediction request window;
- workers: caller plus actual new ggml workers USER_INITIATED in the window;
- static: caller USER_INITIATED throughout, actual ggml workers USER_INITIATED;
- optional interactive follow-up: caller/workers USER_INTERACTIVE in the window.

The production core does not attach a persistent ggml pool; CPU graph execution
creates disposable pthread workers. A before-request thread snapshot alone would
miss those compute workers. This executable interposes pthread_create from the
statically linked backend, identifies ggml_graph_compute_secondary_thread via
dladdr, and wraps only those threads. Inside the actual worker, query requested
QoS, optionally raise it, execute its original entry point, restore prior QoS,
and query again before thread exit. Unspecified prior QoS is restored to default
because the setter rejects unspecified; that worker then terminates immediately.
Controls use the identical trampoline/counters so instrumentation is common.

Version 2: only owned caller/actual ggml worker threads are modified. The initial
run tried enumerating other process pthreads and applying QoS overrides. It
trapped on ending a Metal/dispatch-thread override after idle, so that incomplete
run is excluded. Arbitrary runtime-owned threads have no stable lifetime guarantee
from a Mach-port snapshot; this approach is removed entirely. On Metal the
validated experiment raises the caller/submission path, not runtime-owned
dispatch workers or the GPU kernels. There is no direct GPU clock control.

Primary: first prediction plus hint setup p50/p95. Secondary: remaining burst
predictions, total burst incl. restore, setup/restore cost, process/worker CPU
time, observed worker QoS and restoration counts. Baseline requested caller QoS
must return to default at every window end (static intentionally stays high).
Validate candidate reading membership, finite probabilities and identical top1
within and across controls. Fail rather than silently accept missed CPU hooks.

Collect proc_pid_rusage v6 deltas if the kernel permits it: performance-core CPU
time, cycles, instructions, QoS-tier CPU time and energy_nj. These are OS process
accounting/estimates, not calibrated system-rail energy or sampled GPU clock.
Report unsupported or zero fields explicitly rather than inventing power values.

Independent tail confirmation: after the main matrix has fully exited, run CPU/4
after 2000 ms idle with none/workers/interactive, 20 bursts per round and three
randomized rounds. This yields 60 first-request observations per policy. Compare
per-round medians as well as aggregate p50/p95; do not assume adjacent bursts are
independent samples for an IID confidence interval. No concurrent benchmarks.

Second synthesis: independently confirm CPU/4's apparent ~8% no-idle improvement
with none/caller/workers, 120 bursts per round, three randomized rounds. Also
confirm Metal/4's apparent ~5% 2000-ms-idle caller improvement with none/caller,
20 bursts per round, three rounds. These narrow confirmations address specific
unresolved patterns in the discovery matrix rather than repeating the full sweep.

Limits: shared desktop, no core pinning, no physical frequency or joule measurement.
powermetrics is installed but noninteractive sudo is unavailable. CPU time is
not energy or watts. Linux VM has unsupported uclamp and no GPU; native Linux
burst-frequency efficacy remains unavailable, not inferred from macOS QoS.
