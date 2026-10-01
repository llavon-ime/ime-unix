# Inference burst experiments

Standalone C++23 tools for testing the existing model/core/service. All output
directories are exclusively created, keeping failed pilots and previous runs.
No production defaults or system power settings are changed.

## macOS build

The following reuses dependencies from an already built macOS service. Initialize
the pinned submodules and bootstrap vcpkg before using the usual service build
if those dependencies are missing.

```sh
cmake -S scripts/inference-bench -B build/inference-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_MANIFEST_MODE=OFF \
  -DVCPKG_TARGET_TRIPLET=arm64-osx-llavon \
  -DVCPKG_INSTALLED_DIR="$PWD/ime-unix-service/build/macos/vcpkg_installed" \
  -DLLAVON_IME_WARNINGS_AS_ERRORS=ON \
  -DINFERENCE_BENCH_BUILD_RAWKEY=ON
cmake --build build/inference-bench --target inference_bench rawkey_bench --parallel 4
```

## Core experiments

```sh
python3 scripts/inference-bench/run.py \
  --binary build/inference-bench/inference_bench \
  --model '/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf' \
  --output build/inference-results/my-sweep \
  --mode sweep --samples 48 --rounds 2
```

Modes: `sweep` (threads/QoS), `cache` (fresh versus reused context), `offload`
(partial GPU layers), `idle` (600/2000 ms idle, none/sync/ahead warmup), and
`uclamp` (Linux only, kernel must support the syscall).
The `ahead` condition waits 100 ms after ready completes; both ready cost and
later predict cost are reported. It does not model a fixed asynchronous deadline.

Use `--threads 3,5,7 --priorities initiated` for a targeted sweep. The QoS is set
on the launching thread before worker creation, not verified independently on
every internal backend thread. A disabled logger is used for core measurements;
the real service probe additionally includes IPC and normal service behavior.

```sh
python3 scripts/inference-bench/run.py \
  --output build/inference-results/my-sweep --summarize
```

## Raw-key/service experiment

```sh
python3 scripts/inference-bench/rawkey-run.py \
  --binary build/inference-bench/rawkey_bench \
  --service ime-unix-service/build/macos/llavon-ime-unix-service \
  --model '/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf' \
  --output build/inference-results/my-rawkey --samples 32 --rounds 2
```

This feeds standard-layout keys through `raw_key_harness.hpp`, waits for real
model predictions to settle, checks the rendered preview, presses Enter and
checks the submitted text and cleared composition. Two-character and six-
character sequences are either burst-typed or wait for predictions at each
syllable boundary. Reported latency is final-tone-to-settled-Engine-render,
not physical application window paint time. Synthetic fixtures are used;
no user input history is read.

For a long run in a concurrently edited checkout, freeze an engine source copy
and the service executable first. Set `INFERENCE_BENCH_ENGINE_SOURCE_DIR` to that
copy during configuration, and pass `--engine-source PATH` to `rawkey-run.py`
to record its source hashes. These are plain experimental copies, not Git worktrees.

## Isolated Linux build

`build-linux.sh REPO SCRATCH` builds the exact cached vcpkg-patched source versions
used in this checkout, without writing to REPO or downloading dependencies.
It needs CMake, Ninja, a C++23 compiler and nlohmann-json headers. For the
Debian trixie container used here, GCC 14 successfully builds the pinned upstream
llama.cpp version; Clang 19 has an upstream t5.cpp specialization error.

```sh
CC=gcc-14 CXX=g++-14 bash /repo/scripts/inference-bench/build-linux.sh /repo /work/inference-bench
python3 /repo/scripts/inference-bench/run.py \
  --binary /work/inference-bench/bench/inference_bench \
  --model /work/model.gguf --output /work/results --mode sweep --samples 48 --rounds 2
```

Linux container/VM timings are not native x86 measurements. Inspect the real
kernel/driver and GPU availability before experimenting with uclamp, CPU
performance profiles or GPU clocks. The Docker VM used here exposes
`dummy-cpufreq`, has no GPU, and rejects utilization clamp changes.

## Request-scoped burst boost (macOS)

This separate experiment implements the actual raise → predict → restore
window, instead of equating rapid typing or permanently high caller QoS with
burst boosting. Build the `burst_bench` target in the same CMake project:

```sh
cmake --build build/inference-burst --target burst_bench --parallel 4
python3 scripts/inference-bench/burst-run.py \
  --binary build/inference-burst/burst_bench \
  --model '/Library/Application Support/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf' \
  --output build/inference-results/my-request-scoped-burst \
  --samples 12 --rounds 2
```

Each sample idles, raises scoped hints, makes four two-syllable predictions,
and restores hints. Modes: `none`, `caller`, `workers`, `static`, and optionally
`interactive`. Defaults compare CPU/4, CPU/8 and Metal/4 after 0/600/2000 ms idle.
For focused repeats use `--targets cpu:4 --idle 2000 --modes none,workers,interactive`.

The pinned core uses **disposable** ggml CPU workers, created during each graph
execution. Snapshotting threads before predict would miss them. The isolated
executable interposes `pthread_create`, matches the actual
`ggml_graph_compute_secondary_thread` symbol, and raises/queries/restores QoS
inside its original compute worker. It fails if a CPU experiment misses the
hook or restoration. The identical trampoline instruments the controls too.
Only the owned caller and matched actual ggml compute workers are modified;
runtime-owned Metal/dispatch threads are not enumerated or overridden. An early
override experiment trapped on a thread lifetime/override release and is excluded.

`pthread_get_qos_class_np` verifies requested QoS on the compute worker itself.
On full Metal the experiment raises the caller/submission path, not GPU clocks
or runtime-owned dispatch worker QoS.

Per-burst kernel `proc_pid_rusage` v6 accounting records P-core CPU time and
process-attributed `energy_nj` when supported. These counters are diagnostic
estimates: they are not calibrated total-system energy, do not establish a
particular clock frequency, and cannot substitute for GPU/system rail sampling.
The locked protocol is `burst-protocol.md`. CPU and caller QoS restoration,
candidate readings, probabilities and repeated/cross-mode top1 are checked.
