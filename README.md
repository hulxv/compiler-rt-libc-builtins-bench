# compiler-rt libc builtins - benchmark

Tracks how compiler-rt's **libc-backed** floating-point builtins
(`COMPILER_RT_USE_LIBC_MATH=ON`) compare against the **legacy** soft-float
implementations, over time, and publishes the trend to GitHub Pages.

## Metrics

All "smaller is better":

| metric                    | determinism                         | role          |
| ------------------------- | ----------------------------------- | ------------- |
| object code size          | exact                               | gated         |
| retired instructions / op | exact, machine-independent (`perf`) | gated         |
| wall-clock ns / op        | noisy on shared runners             | informational |

The deterministic metrics (size, instructions) are the trustworthy signal on
free GitHub-hosted runners. Wall-clock is reported for intuition but should
only be trusted on dedicated hardware.

## How it works

`run-bench` builds compiler-rt's `builtins` target **twice** -
`COMPILER_RT_USE_LIBC_MATH` OFF then ON. sccache is shared across the two
builds, so the LLVM/clang objects are compiled once; only the handful of
builtin sources differ on the second build.

For each enabled builtin it then measures:
- object size from each `libclang_rt.builtins.a`
- per-op retired instructions via `perf stat`, using a two-point subtraction
  (N and 2N iterations) to cancel fixed overhead
- per-op wall-clock from the timing harness (median of 11 runs)

Results are emitted as `results.json` in the
[`github-action-benchmark`](https://github.com/benchmark-action/github-action-benchmark)
`customSmallerIsBetter` format and pushed to the `gh-pages` branch, which
renders the trend charts.

## Local run

```sh
# from inside an llvm-project checkout, or pass the path explicitly:
./run-bench /path/to/llvm-project
cat results.json
```

Env knobs: `ENABLED` (builtins, default `__addtf3 __adddf3`), `WORK` (build
dir), `COUNT_ITERS`, `OUT`.

## Layout

```
harness/bench.cpp     timing + perf-stat count harness (linked vs each archive)
run-bench             build x2, measure size/instr/time, emit results.json
scripts/to-json.py    measurements.tsv -> github-action-benchmark JSON
.github/workflows/    nightly + on-demand CI, publishes to gh-pages
```

## CI

Runs nightly against llvm-project `main`, or on demand
(`workflow_dispatch` with an `llvm_ref` input). Requires GitHub Pages enabled
on the repo. `alert-threshold: 120%` comments on a >20% regression but does
not fail the run (shared-runner perf noise would otherwise cause false reds).

## Adding a builtin

1. Add its `extern "C"` decl and registry entry in `harness/bench.cpp`,
   and to the `ENABLED` list there.
2. Add it to the default `ENABLED` in `run-bench` (or pass via env).
