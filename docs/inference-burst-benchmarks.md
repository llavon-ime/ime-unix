# Mac / Linux burst inference experiments — 2026-09-30

## Executive findings

The measured native Mac is an M3 MacBook Air, 16 GB, with 4 performance and 4
efficiency cores, connected to AC and with low-power mode off. The model is the
installed `llavon-ime-llama-250m-Q4_K_M.gguf`; inference uses the pinned ime-core,
llama.cpp b9030 and ggml 0.11.1. These are single-machine exploratory experiments,
not a universal Mac/Linux hardware benchmark or a model-accuracy evaluation.

Completed **202 valid process configurations / 27,952 measured requests**:
172 macOS core configurations (24,112 requests), 20 macOS real-service raw-key
configurations (1,920 requests), and 10 serialized Linux CPU configurations
(1,920 requests). Failed pilots, unmeasured warmups and the initial contended
Linux sweep are excluded from these totals.

1. **Short interactive requests: CPU/4 threads is a good low-latency candidate.**
   It beats full Metal offload by about 15–28% median in the real raw-key burst
   cases. CPU/6 threads sometimes has a slightly better median; 4 threads has
   more predictable tails. Opening all 8 threads is not a reliable improvement.
2. **Long new-context prefill: Metal wins.** Around 284 ms on CPU/4 threads versus
   around 71 ms on Metal. This rules out a blanket CPU-only recommendation.
3. **Keep the KV prefix cache.** The same long-context incremental workloads
   drop to around 12 ms on both backends. Session/cache lifecycle is a stronger
   lever here than the tested caller QoS changes.
4. **Do not add unconditional synchronous warmup.** It can reduce the following
   prediction's time while increasing total compute and visible synchronous wait.
   Advance warmup is worth testing only when there is a real lead opportunity.
5. **Clock boosting itself is unverified.** macOS QoS does not specify clocks.
   The available Linux VM has dummy-cpufreq, no GPU and unsupported uclamp setters.

## Measurements and controls

The core harness measures synchronous `Session::predict()` after four unmeasured
warmup requests per workload, alternating four different Chinese context suffixes.
Repeating an unchanged one-character query could reuse logits without a decode;
that misleading shortcut is deliberately avoided. Workloads have one, two or
six unresolved syllables, plus a two-syllable workload with a roughly 312-character
shared context prefix. Fresh-session ablations measure full prefill separately.

Configuration order is randomized with seed 20260930; each runs in a separate
process, with repeated rounds. Model/source/binary hashes, raw samples and the
ordered plan are retained. Candidate reading membership and finite probabilities
are checked outside the timed interval. Median is p50; p95 uses nearest rank.

The desktop was shared with existing applications and other work. No competing
process was terminated. Randomized repetitions mitigate but cannot remove
contention/temperature/frequency drift. Intermediate-thread results varied from
earlier phases, so fine-grained cross-phase comparisons are not treated as causal
effects. Same-phase controls were added for a final confirmation.

## Real Engine / service / raw-key results

The probe sends standard-layout raw keys through `raw_key_harness.hpp`, pumps
real service replies, observes the Engine render, presses Enter and checks the
submitted text and cleared composition. A separate status query confirms a
loaded model session received the request, preventing fallback-only observations
from being accepted. All 1,920 measured cases across 20 configurations passed
render/commit checks; previews match across these backend/thread configurations.

Each table row has 64 observations per configuration (32 in each of two rounds).
This is **final tone key → settled Engine render**, including Unix IPC and
host-queue pumping. It excludes physical application window painting. The client
document is synthetic and configuration/training state is isolated in temporary
directories. Service logs verify the requested thread counts and GPU layers.

### Burst typing, median milliseconds

| Characters | CPU/4 threads | CPU/6 threads | CPU/8 threads | Metal/8 threads |
| --- | ---: | ---: | ---: | ---: |
| 1 | 7.65 | 7.60 | 8.06 | 10.64 |
| 2 | 14.39 | 13.97 | 14.92 | 18.11 |
| 6 | 29.30 | 28.58 | 30.49 | 34.35 |

### Wait at each syllable boundary, milliseconds

| Characters | CPU/4 p50 / p95 | CPU/8 p50 / p95 | Metal/8 p50 / p95 |
| --- | ---: | ---: | ---: |
| 1 | 7.70 / 9.80 | 10.63 / 41.64 | 10.67 / 10.71 |
| 2 | 6.39 / 7.15 | 11.56 / 54.80 | 7.23 / 7.58 |
| 6 | 18.71 / 26.49 | 18.57 / 30.81 | 18.45 / 19.00 |

Metal is steadier for the six-character paced tail even though CPU is faster for
many short medians. The first actual model-render request, including lazy model
load, has median 162 ms on CPU and 135 ms on Metal across the configuration runs.
`service_open_ms` only measures endpoint/session opening, not model readiness.

## Core thread / QoS discovery

First sweep: 60 process configurations, 11,520 measured predictions, two rounds,
48 samples per workload/configuration/round. All following values are aggregate
median/p95 milliseconds from this phase, not physical UI response measurements.

| Workload | CPU/4 default | CPU/8 default | Metal/8 default |
| --- | ---: | ---: | ---: |
| One syllable | 4.65 / 5.74 | 5.70 / 8.10 | 6.57 / 6.64 |
| Two syllables | 8.84 / 10.69 | 9.68 / 65.10 | 11.81 / 11.98 |
| Six syllables | 22.02 / 23.07 | 22.21 / 60.47 | 23.75 / 26.48 |
| Long cached context | 11.72 / 13.70 | 13.55 / 16.42 | 12.36 / 12.55 |

The large CPU/8 outliers are intermittent: one two-syllable round has p95 about
138 ms, another about 10.6 ms. They should not be described as an inevitable
fixed cost. Their recurrence in the real-service probe is nevertheless a reason
to prefer a smaller thread count when predictable interactive latency matters.

Default, USER_INITIATED and USER_INTERACTIVE caller QoS were tested. Metal's
two-syllable p50 generally remained around 11.8 ms; CPU changes were not
consistently beneficial across workloads/rounds. QoS was set before model and
worker creation, but each internal worker's actual QoS/core placement was not
independently traced. No CPU/GPU clock rate was measured or forced on macOS.

### Independent same-phase confirmation

16 configurations, 2,560 predictions, two rounds and 40 samples per workload
per round. Four threads retains the short-request advantage; five threads is
not a clear improvement. Six threads lowers two-syllable median slightly but
has a larger p95 than four threads.

| Workload | CPU/4 p50 / p95 | CPU/8 p50 / p95 | Metal/8 p50 / p95 |
| --- | ---: | ---: | ---: |
| One | 4.60 / 5.07 | 5.45 / 7.06 | 6.59 / 6.63 |
| Two | 8.73 / 8.91 | 9.80 / 75.39 | 11.86 / 11.94 |
| Six | 22.77 / 27.27 | 22.57 / 78.33 | 23.85 / 24.59 |

CPU/6 two-syllable p50/p95: 8.58 / 10.24 ms. These observations support testing
CPU/4 as the first short-request configuration while preserving Metal's
long-prefill and stable-tail advantages.

## Cache, partial offload and idle ablations

### Cache: median milliseconds

| Workload/backend | Reused session | Fresh session | Fresh construction cost, separate |
| --- | ---: | ---: | ---: |
| Two, CPU/4 | 9.23 | 13.12 | 5.13 |
| Two, Metal/4 | 11.80 | 13.15 | 6.64 |
| Long, CPU/4 | 12.35 | 284.00 | 6.88 |
| Long, Metal/4 | 12.38 | 70.68 | 7.23 |

Do not add the medians of separate components and call that the exact total
median. Raw per-request component times are available for proper aggregation.

### Partial offload

GPU layers 0/4/8/12/all, CPU threads 2/4/8, two rounds and 24 samples per workload:
30 configurations and 2,880 measured predictions. For two syllables with four
CPU threads, median is CPU-only 8.75 ms, 8 GPU layers 10.74 ms, 12 GPU layers
11.20 ms, full Metal 11.85 ms. Partial offload did not beat CPU-only on this short
workload, and some 8-thread partial configurations had large tail spikes.

### Idle / warmup: two syllables, four threads, USER_INITIATED

24 configurations and 240 observations. Each grouped setting has 20 samples
(10 per round), so these idle-tail estimates have less support than the main
sweeps. A single-token `ready()` uses a separate context; no periodic keep-alive
or busy loop is introduced.

| Idle/backend/condition | Predict p50 | Ready p50 | Combined compute p50 |
| --- | ---: | ---: | ---: |
| 2 s CPU, none | 20.10 | 0 | 20.10 |
| 2 s CPU, synchronous ready | 13.56 | 6.99 | 23.09 |
| 2 s CPU, advance ready | 12.74 | 6.78 | 20.09 |
| 2 s Metal, none | 18.52 | 0 | 18.52 |
| 2 s Metal, synchronous ready | 16.02 | 9.62 | 25.63 |
| 2 s Metal, advance ready | 17.92 | 9.27 | 27.17 |

The advance condition waits **100 ms after ready completes**, so total advance
is ready time + 100 ms. It is not a fixed 100 ms asynchronous deadline. It can
move computation before the final tone, but does not make that work free.
600 ms idle observations also show no universally better warmup policy.

## Candidate stability and scope

Every accepted core request had nonempty candidates of the requested reading and
finite probabilities. The initial sweep had 360 lower-rank variation observations
but **zero within-configuration repeated top1 changes**. Across CPU thread
configurations, one ambiguous synthetic long-context query changes 新香 versus
新鄉. Full rank order is not asserted to be bitwise invariant, and faster
settings are not declared to preserve general model accuracy from these cases.
The 1,920 raw-key fixtures have matching preview and commit text across settings.

## Linux availability and builds

Configured native Linux SSH access was unreachable. A dedicated aarch64 Linux
container/VM was used, with read-only repository access and a private copy of
the identical model. Exact cached vcpkg-patched dependency sources were built
with GCC 14, C++23 and warnings enabled; the harness uses warnings as errors.
Clang 19 had an upstream t5.cpp specialization compile error, so the upstream
source was not patched to conceal it.

The VM reports eight virtual CPUs and `dummy-cpufreq`, with nominal min/current/
max all 2 GHz. This does **not** measure the real Apple CPU clock. It exposes no
`/dev/dri` or NVIDIA GPU. uclamp values 0/512/768/1024 all return Operation not
supported. Native x86 CPU boost, Vulkan/CUDA GPU clocks, NVML and AMDGPU
performance-level efficacy therefore remain unmeasured.

The first Linux sweep briefly overlapped a service rebuild; it is retained as
`linux-sweep-contended` and excluded from baseline recommendations. A serialized
Linux confirmation completed after the final native controls: 10 configurations,
1,920 measured predictions, 96 observations per grouped workload/thread setting.

### Linux VM CPU confirmation, milliseconds

| Workload | 4 threads p50 / p95 | 6 threads p50 / p95 | 8 threads p50 / p95 |
| --- | ---: | ---: | ---: |
| One | 4.55 / 4.90 | 4.57 / 5.61 | 5.62 / 7.06 |
| Two | 9.39 / 9.73 | 8.85 / 9.95 | 10.03 / 14.41 |
| Six | 25.15 / 29.11 | 25.52 / 38.24 | 24.28 / 32.87 |
| Long cached context | 13.36 / 15.26 | 12.93 / 14.03 | 15.15 / 180.04 |

Four or six threads is a sensible candidate in this VM. The six-character
median happens to favor eight threads, so the result is workload-dependent;
the long-context eight-thread tail is considerably worse. These timings cannot
be extrapolated to native x86 machines or interpreted as uclamp speedups.

## Verification

- Native benchmark/probe: C++23, project-wide diagnostics and warnings as errors.
- Linux benchmark: C++23/GCC 14, project-wide diagnostics and warnings as errors;
  the pinned dependency build completed without compiler warnings.
- Real-service raw-key fixtures: 1,920 measured preview/commit cases passed.
- Frozen engine regression runner: 68 suites, zero failures. Six optional
  memory-scanner checks were skipped because the helper was not configured;
  those skips do not validate memory scanning and are unrelated to boost efficacy.
- Rebuilt service CTest: 2/2 tests passed (service protocol/session and native LoRA).
- Python drivers compile and the Linux build script passes `bash -n`.

## Experimental repairs and reproducibility

Failed pilots are retained and excluded from successful counts. Two important
repairs were necessary:

- `Harness::settle_prediction()` is an offline cancellation helper, not a wait.
  The probe now pumps actual model replies and checks service session status.
- Engine files were being edited during this long run. The validated raw-key
  measurements use a frozen engine source copy and rebuilt service binary.
  A missing `<span>` include in the in-progress transport was fixed only in
  the experimental copy. Source/binary hashes are recorded; no production
  input behavior was edited by this experiment.

Tools and commands: [`scripts/inference-bench/README.md`](../scripts/inference-bench/README.md).
The protocol, evolving findings and research log are in the same directory.
Raw JSON/logs live under `build/inference-results/`; the attached evidence archive
contains the valid observations and manifests, not models or dependency binaries.

Official API references:

- [Apple Silicon scheduling / QoS](https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon)
- [Linux utilization clamping](https://docs.kernel.org/scheduler/sched-util-clamp.html)
- [NVIDIA clock controls](https://docs.nvidia.com/deploy/nvidia-smi/index.html)
- [AMDGPU power / clock controls](https://docs.kernel.org/gpu/amdgpu/thermal.html)
