# Request-scoped burst boost — 2026-09-30

## Result: no reliable first-after-idle win

Completed **96 valid process configurations, 2,244 boost windows and 8,976
predictions**, including three independent focused confirmations. All accepted
predictions have valid readings/probabilities and identical top1 across controls.
Observed **53,280 actual ggml compute-worker creations**; **23,040** raised-worker
instances all verified the requested elevated QoS and restored before thread exit,
with **zero QoS verification/restoration errors**. Pilots and the incomplete
runtime-thread-override attempt are excluded from these totals.

**Do not enable this request-scoped QoS scheme by default for the measured
model/M3.** Raising actual workers is demonstrably possible, but it does not
reliably shorten the first inference after idle. This does not evaluate physical
CPU/GPU clock locking or native Linux DVFS; those remain unmeasured.

### CPU/4 after two seconds idle — independent confirmation

Each policy has 60 windows (20 in each of three randomized rounds). Times include
caller hint setup; worker setter/restoration overhead is inside predict.

| Policy | First p50 / p95 ms | Following predictions p50 ms | Four-predict window p50 ms |
| --- | ---: | ---: | ---: |
| Default, no boost | 19.776 / 25.395 | 10.828 | 52.933 |
| Scoped USER_INITIATED caller + actual workers | 20.456 / 27.558 | 10.507 | 52.916 |
| Scoped USER_INTERACTIVE caller + actual workers | 21.204 / 25.878 | 10.753 | 55.135 |

USER_INITIATED first median is **3.4% slower**, USER_INTERACTIVE **7.2% slower**.
For initiated workers the matched-round median improvements were +2.7%, +0.4%,
and -6.4%; interactive +6.4%, -5.7%, and -17.5%. This variability does not support
a consistent wakeup benefit. The following calls can be slightly quicker, but
the total four-call initiated window is effectively unchanged.

### CPU/4 without idle — larger independent confirmation

360 windows per policy (120 per round, three rounds). The discovery matrix's
apparent ~8% improvement did not survive this confirmation.

| Policy | First p50 / p95 ms | Four-predict window p50 ms |
| --- | ---: | ---: |
| Default, no boost | 8.958 / 10.307 | 35.903 |
| Scoped caller only | 9.003 / 10.472 | 36.382 |
| Scoped caller + actual workers | 9.503 / 10.455 | 38.292 |

Caller-only median differs by about **0.5%**. Worker boost is **6.1% slower**, and
its matched-round first medians are slower in all three rounds (-8.9%, -6.6%,
-4.2% improvement). Kernel accounting already places essentially 100% of compute
CPU time on P cores in all three policies; this is not proof of a maximum clock.

### Full Metal/4 after two seconds idle — independent confirmation

60 windows per policy, three rounds. No ggml CPU compute workers are created
for these full-offload predictions: this measures scoped caller/submission QoS.

| Policy | First p50 / p95 ms | Four-predict window p50 ms |
| --- | ---: | ---: |
| Default | 21.923 / 26.440 | 68.036 |
| Scoped USER_INITIATED caller | 21.818 / 25.071 | 67.187 |

The apparent ~5% discovery median gain shrinks to **0.5% / 0.105 ms**. Matched-round
median improvements are +5.4%, +1.2%, and -0.5%. There is a smaller aggregate p95,
but these results do not establish a reliable GPU burst-frequency speedup.

### Costs and accounting

- Idle CPU/4 initiated caller setup: median **5.1 us**, caller restore **1.1 us**.
  Additional worker setter/restore calls are measured inside predict, so these
  two figures alone are not the entire worker-boost overhead.
- In the two-second CPU/4 confirmation, the kernel P-core CPU-time fraction moves
  from about **81.3% to 86.3%**, without shortening the first-request median.
- Four-request process-attributed energy estimates: baseline **274.0 mJ**,
  initiated workers **272.4 mJ**, interactive workers **262.6 mJ**. These are
  kernel accounting, exclude the preceding idle window and are not measured
  whole-device joules; no energy-efficiency conclusion is drawn from this alone.
- Without idle the worker-boost process energy estimate is lower despite longer
  latency. This underscores that energy and responsiveness are separate goals,
  and that these process estimates cannot be interpreted as physical boost clocks.

## Discovery matrix and confirmation coverage

| Phase | Configurations | Boost windows | Predictions |
| --- | ---: | ---: | ---: |
| CPU/4, CPU/8, Metal/4; 0/600/2000 ms idle; four policies; two rounds | 72 | 864 | 3,456 |
| CPU/4; 2000 ms; default / initiated workers / interactive workers; three rounds | 9 | 180 | 720 |
| CPU/4; 0 ms; default / caller / workers; three rounds | 9 | 1,080 | 4,320 |
| Metal/4; 2000 ms; default / caller; three rounds | 6 | 120 | 480 |

The full matrix also found no consistent first-after-idle win on CPU/8, and no
stable Metal/600-ms improvement. Every initial grouped policy has only 24 windows,
so apparent gains and tail changes were investigated with the focused independent
confirmations above. Full tables and per-round changes are attached in the raw
evidence archive as `analysis.md` and `analysis.json` for each phase.

## Scope

This is a new native-M3 experiment explicitly measuring:

```text
idle → raise caller/actual compute worker QoS → four predictions → restore
```

The earlier `inference-burst-benchmarks.md` measured rapid typing, static caller
QoS and warmup. Its results do not establish request-scoped burst-boost efficacy.
Here the pinned ime-core and installed 250M Q4_K_M model remain the inference
implementation. A separate standalone executable instruments actual CPU workers.

## Important worker discovery

The core does not attach a persistent ggml pool. ggml CPU graph execution creates
and joins disposable pthread compute workers. There may therefore be **no compute
workers to raise in an idle-time thread snapshot**.

The experiment intercepts the statically linked backend's `pthread_create`, uses
`dladdr` to match `ggml_graph_compute_secondary_thread`, then queries/optionally
raises QoS inside that exact worker before invoking its original compute entry.
It restores the previous requested class before thread exit, verifies it, and
records counts. The same wrapper/counters are used in baseline and treatments.
CPU runs abort if matching fails or a QoS change/restoration fails.

Pilot observation: raising the caller to USER_INITIATED alone left all 48 observed
CPU workers at their original class; explicitly raising actual workers verified
48/48 at USER_INITIATED, then 48/48 restored before exit. This demonstrates why
the earlier fixed-caller experiment is not a worker-boost experiment.

Across the four valid phases, **14,400 worker creations in caller-only policies
still report QOS_CLASS_DEFAULT**, whereas explicit worker policies all report the
requested elevated class. Thus this is verified on actual backend workers,
not inferred from the caller's QoS getter.

An initial pilot also enumerated other process pthreads and applied QoS overrides.
That incomplete run hit SIGTRAP while ending a Metal/dispatch-thread override;
arbitrary runtime-owned threads can retire during idle, and Mach-port ownership
does not make the user-space pthread handle lifetime stable. This approach was
removed entirely, and the incomplete run is excluded from conclusions. The final
experiment only modifies owned caller and actual compute-worker threads. For
Metal it tests the caller/submission path, not runtime-owned dispatch threads or
direct GPU clock control.

## Locked comparison

- Hardware: native Apple M3 MacBook Air, 4 P + 4 E CPU cores, 16 GB.
- CPU thread counts: 4 and 8; full Metal offload with 4 CPU threads.
- Idle intervals: 0, 600 and 2000 ms.
- Four predictions per burst, changing Chinese context suffixes to force decode;
  two unresolved syllables per prediction. Four initial untimed predictions.
- `none`: caller default; query original compute-worker QoS.
- `caller`: caller USER_INITIATED only during the burst.
- `workers`: caller + actual new compute workers USER_INITIATED for the burst.
- `static`: caller stays USER_INITIATED between bursts; compute workers raised.
- Optional `interactive`: caller + workers USER_INTERACTIVE during the burst.
- Configuration order randomized with seed 20260930 and repeated full rounds.
- Primary: first predict **including hint setup**, p50/p95.
- Secondary: following predictions, total four-request window including restore,
  setup/restore overhead, process/compute-worker CPU time and kernel accounting.
- Verify nonempty candidates, canonical reading membership, finite probabilities,
  repeated and cross-mode top1. All worker classes/API overrides must be restored.

## Accounting and limitations

The OS permits `proc_pid_rusage` v6 on the experiment's own process. P-core CPU
time, QoS-tier CPU time, cycles/instructions and `energy_nj` deltas are collected.
These are **kernel process accounting/estimates**, not a physical clock trace or
calibrated CPU/GPU/system rail measurement. In particular, Metal process CPU
energy accounting is not the energy consumed by the GPU kernels. CPU-work and
energy counters must be compared within a backend, not used to declare Metal
more energy-efficient than CPU from these counters alone.

`powermetrics` is installed but noninteractive sudo requires a password. No
privileged sampler or system power-setting change was performed. The Linux host
was re-probed and timed out. The available Linux VM again rejects `uclampset`
and exposes no GPU. Native Linux boost efficacy remains unmeasured.

Desktop contention, temperatures and power management can change across phases.
This is a single-machine exploratory experiment; p95 with small idle-sample
counts should not be treated as a precise population tail estimate.

## Reproduction and evidence

See `scripts/inference-bench/README.md` and the pre-run locked
`scripts/inference-bench/burst-protocol.md`. The new target is `burst_bench` and
the driver is `burst-run.py`. Raw manifests/results/logs are preserved under
`build/inference-results/burst-*`; failed builds and pilots are excluded from
main performance conclusions. C++23 and all project diagnostics are enabled,
with warnings as errors.

The analysis independently asserts caller restoration, worker targeting and
every worker's requested class/restoration counts across all 2,244 valid windows.
This is core prediction + QoS-transition validation; the new experiment does
not measure physical application paint latency or modify production key behavior.
