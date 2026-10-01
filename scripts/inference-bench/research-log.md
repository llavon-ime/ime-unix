# Experiment timeline

## Initial discovery

- Detected native M3 MacBook Air (4 P + 4 E cores), 16 GB; low-power mode off.
- Native Linux SSH access timed out. Existing Docker provides aarch64 Linux,
  without GPU exposure. Its uclampset syscall returns Operation not supported;
  its exposed CPU governor is performance. No native DVFS conclusion is possible.
- Created standalone tools and locked `protocol.md` before the first run.
- First pilot stopped on a full candidate rank-stability assertion. Investigation
  showed that exact ordering of low-probability candidates is too strict for
  inference numeric variations. Retained failed pilot artifacts. Changed the
  harness to record full rank and top1 differences, while still rejecting empty,
  invalid-probability and wrong-reading results. The completed rerun has zero
  repeated top1 changes; it does have lower-rank variations.

## First outer-loop synthesis

- Completed 60 randomized process configurations and 11,520 measured predictions.
- CPU 4/6 threads is fastest by median for one/two-character incremental requests.
- CPU 8 threads exhibits large p95 outliers, particularly on six-character work.
- Metal median and tail barely depend on the caller QoS or CPU thread count.
- QoS-only improvement is not yet supported; CPU/GPU and thread configuration
  are stronger levers on this model/hardware.
- Long-context query variant 1 yields 新香 or 新鄉 depending on CPU thread count.
  Record as a configuration-dependent ambiguous prediction, not a quality pass.
- Follow up with fresh-session prefill, partial GPU offload, idle wakeup and
  real service/Engine render latency to avoid extrapolating from core microbench.

## Linux build observations

- Isolated a new container; repository is mounted read-only. No running app
  container or installed input method was reconfigured.
- Cached exact vcpkg-patched ggml 0.11.1 and llama.cpp b9030 source trees are reused.
- Ninja is required in the minimal container; C++ module scanning disabled
  because the benchmark does not use modules and clang-scan-deps is absent.
- Clang 19 failed compiling the pinned upstream t5.cpp specializations. Use
  GCC 14 in the isolated container rather than editing the pinned model code.

## Follow-up synthesis

- Completed fresh/reused context, partial offload, intermediate threads and idle
  experiments: 156 total macOS core configurations, 21,552 measured predictions.
- Partial offload did not beat CPU/4 threads on the two-character workload.
- The intermediate-thread phase was slower overall than the first sweep. Do
  not compare fine-grained numbers across these phases without same-phase controls.
- After 2 seconds idle, CPU ahead warmup reduced later predict p50 from about
  20.1 to 12.7 ms, but consumed another about 6.8 ms of warmup. Metal synchronous
  warmup reduced subsequent predict time but increased total compute. No blanket
  synchronous warmup recommendation is supported.
- The ahead protocol waits 100 ms **after** ready completes. Its actual advance
  includes ready time plus 100 ms; it is not a fixed 100 ms asynchronous deadline.

## Raw-key measurement repair

- The first IPC pilot used an older service binary. Rebuilt the current service
  and froze it for subsequent experiments.
- Engine sources changed concurrently during this long session. A rebuild found
  a missing `<span>` include in the in-progress service transport. Copied a
  point-in-time engine snapshot into build/, added the include **only in that
  snapshot**, and built it with the same strict diagnostics. Production sources
  were not edited by this experiment.
- Crucial harness discovery: `Harness::settle_prediction()` invalidates pending
  predictions for offline tests; it does not wait for model responses. Corrected
  the benchmark to pump the actual host queue until prediction.pending clears,
  then query service status to verify a loaded model session received the request.
  Failed pilots are excluded from latency conclusions and retained as evidence.
- Isolated all synthetic commits/configuration/state and model/thread/layer
  environment overrides inside each private benchmark process.

## Final confirmation and closure

- Completed 20 validated real-service process configurations with 1,920 measured
  raw-key cases. Every preview/Enter commit matched across configurations, and
  service logs confirm the requested layers and CPU threads.
- Repeated native controls in one randomized phase: CPU/4 two-syllable p50/p95
  8.73/8.91 ms, CPU/8 9.80/75.39 ms, Metal/8 11.86/11.94 ms. CPU/6 median is
  slightly better at 8.58 ms but p95 is 10.24 ms. Five threads has no clear win.
- Ran a serialized Linux CPU sweep: 10 configs, 1,920 predictions; retain earlier
  overlapping sweep as contended evidence and exclude it from valid totals.
- Frozen-engine raw-key regressions: 68 suites, zero failures; six optional
  scanner-helper checks skipped. Rebuilt service CTest: 2/2 passed.
- Valid totals: 202 process configurations / 27,952 measured requests. Failed
  pilots and unmeasured warmups excluded. No production behavior/power settings
  changed. Research session is complete; there are no scheduled research jobs.

## Request-scoped boost follow-up

- User clarified that burst boost, rather than rapid typing/static caller QoS,
  needed a direct raise → inference → restore experiment.
- Found disposable ggml pools: idle-time thread enumeration misses the actual
  CPU compute workers. Added standalone pthread_create interposition matching
  ggml_graph_compute_secondary_thread, and queried/raised/restored inside that
  exact worker. Controls use the same wrapper/counters.
- An early attempt to override other runtime-owned pthreads trapped while ending
  a Metal/dispatch-thread override. Removed all arbitrary thread enumeration/
  overrides. Preserved and excluded that incomplete run. The final code modifies
  only owned caller and original compute workers; Metal is caller/submission QoS.
- Kernel proc_pid_rusage v6 accounting is available without sudo; collected P-core
  CPU time, cycles, instructions and process energy estimates. Privileged
  powermetrics is unavailable. Energy accounting is not system/GPU rail power.
- Completed 72-config randomized matrix, then three focused confirmations of
  idle CPU, warm CPU and idle Metal patterns. Valid totals: 96 configurations,
  2,244 windows, 8,976 predictions. 53,280 compute worker creations observed;
  23,040 raised workers verified and restored, zero QoS errors and matching top1.
- Crucial propagation observation: caller-only policies leave all 14,400 observed
  compute workers at default QoS. Actual worker boost is a different intervention.
- CPU/4 idle 2 s confirmation: baseline 19.776/25.395 ms p50/p95 versus initiated
  workers 20.456/27.558 ms, interactive 21.204/25.878 ms. No consistent improvement.
- Warm CPU discovery ~8% gain failed a 360-window-per-policy confirmation:
  baseline/caller/workers first medians 8.958/9.003/9.503 ms. Metal's apparent ~5%
  idle gain shrank to 0.5% in a 60-window-per-policy confirmation.
- Conclusion: this scoped QoS policy is not justified as a default latency
  optimization for the measured model/M3. Native Linux and physical frequency
  boosting remain unmeasured. The follow-up is complete, with no scheduled jobs.
