# Burst inference experiments — 2026-09-30

## Scope and locked evaluation

Use the pinned production ime-core and installed 250M Q4_K_M model. Do not
change the running input method, user configuration, OS power settings or
submodule commits. All changes are standalone experimental tooling.

Primary metrics: per-workload synchronous predict p50/p95 milliseconds. Report
model loading, session construction and warmup costs separately. An ahead warmup
waits 100 ms after ready completes (advance = ready time + 100 ms); it is not a free improvement in total compute or an
end-to-end keyboard latency measurement. Verify candidate reading membership,
   finite probabilities, repeated ranking stability, and top-1/rank differences
between configurations. Never benchmark an unchanged one-character query in a
tight loop: that can return cached logits without doing a decode. Cycle four
different Chinese context suffixes. Long-context workloads include a shared
prefix; fresh sessions separately measure full prefill.

Randomize configuration order with seed 20260930, run separate processes to
isolate scheduling and backend state, repeat complete rounds, keep raw samples,
and persist the ordered plan and model checksum before execution. GPU pipelines
may remain cached by the driver: startup here is not guaranteed disk-cold or
shader-cache-cold. No claim of physical core pinning or explicit worker QoS
verification merely from setting the launching thread's QoS.

## Hypotheses

1. **Threads**: small quantized models can lose performance from synchronization
   and asymmetric-core contention. Sweep 1/2/4/6/8 threads on CPU and Metal.
2. **QoS**: default/user-initiated/user-interactive scheduling may change latency,
   especially CPU tails. Set the launching thread before model/worker creation.
3. **Idle wake**: ready() after 600/2000 ms idle may lower the subsequent predict
   time, but synchronous warmup may increase total compute. Compare none,
   synchronous ready, and ready followed by a 100 ms advance window.
4. **Cache**: reuse versus fresh sessions distinguishes incremental decoding
   from full prefill. Repeat at 2/4/8 threads.
5. **Offload**: partial offload may lose to both CPU-only and full offload due to
   synchronization. Sweep 0/4/8/12/all layers if the first round justifies it.
6. **Linux uclamp**: use 0/512/768/1024 hints under SCHED_OTHER. Verify the kernel
   accepted the value. A Docker Linux VM cannot validate native x86 CPU DVFS or
   GPU clocks; report API/CPU observations as VM-only.

## Available hardware

Native macOS: Apple M3 MacBook Air, 4 performance + 4 efficiency cores, 16 GB,
low-power mode disabled. Shared desktop workloads are not killed; randomized
repeats mitigate but do not eliminate interference/thermal drift.

Linux: existing aarch64 Docker VM. Configured native Linux SSH host was probed
but unreachable. No GPU-frequency benefit may be inferred from this VM.

## Research loop

Run a thread/QoS discovery sweep, synthesize the pattern, then targeted idle,
cache and offload experiments. Repeat promising comparisons on independent
rounds. Conclude with measured recommendations, negative results, limitations
and commands to reproduce. This is a finite benchmark session, with no scheduled
jobs or automatic commits.
