# Burst inference findings

## Current understanding

The 250M quantized model's incremental workload is small enough that GPU
submission/synchronization overhead can outweigh Metal compute acceleration.
On the native M3, CPU with 4–6 threads has lower median short-request latency
than full Metal offload. Metal is more stable at the tail than an 8-thread CPU
configuration. Increasing caller QoS is not a consistently beneficial speedup
in the initial randomized sweep.

The fresh-context ablation changes this conclusion: a long context takes around
284 ms on CPU/4 threads versus around 71 ms on Metal. Retaining its common-prefix
KV cache reduces subsequent requests to around 12 ms on either backend. Backend
selection should therefore account for workload shape; a blanket CPU-only or
GPU-only recommendation would hide this tradeoff.

## Lessons and constraints

- Exact candidate rank ordering is not numerically invariant; preserve rank
  differences as observations rather than suppressing them.
- Initial sweep: repeated top1 never changes within a configuration. Across CPU
  thread configurations, one ambiguous synthetic long-context case changes
  新香 versus 新鄉. No general model accuracy claim is made.
- Repeating an unchanged single-character request can avoid actual model decode;
  cycle different context suffixes to avoid that misleading microbenchmark.
- Core timings exclude table validation, JSON serialization, service IPC and
  app painting. Raw-key/service measurements are required before claiming an
  input responsiveness improvement.
- A warmup can lower later latency while increasing total compute. Report both.
- Caller QoS is a scheduling hint, not evidence of a particular CPU/GPU clock.
- The available Linux VM exposes dummy-cpufreq at a nominal constant frequency,
  no GPU, and unsupported uclamp setters. It can test Linux CPU inference, not
  physical x86 DVFS or GPU clock-control effectiveness.

## Confirmed follow-ups

- Independent same-phase confirmation: CPU/4 two-syllable p50/p95 8.73/8.91 ms,
  CPU/6 8.58/10.24 ms, CPU/8 9.80/75.39 ms, Metal/8 11.86/11.94 ms.
  Five threads is not a clear improvement; four is a good short-request candidate.
- Partial offload did not beat CPU/4 on two-syllable requests.
- Synchronous ready can increase total compute. Advance CPU warmup after 2 s idle
  lowers later p50 from 20.10 to 12.74 ms but adds about 6.78 ms of earlier work.
- The CPU short-request advantage survives real Engine/raw-key/service IPC:
  burst medians for CPU/4 versus Metal/8 are 7.65/10.64 ms (one), 14.39/18.11 ms
  (two), 29.30/34.35 ms (six). All 1,920 measured preview/commit checks pass.
- Linux VM serialized confirmation likewise favors 4–6 threads for short
  predictions. No physical boost efficacy can be established in this VM.

Completed 202 valid process configurations and 27,952 measured requests.
Full tables, limits, regression results and reproduction are documented in
`docs/inference-burst-benchmarks.md`.
