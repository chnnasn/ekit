# Sparse query coverage benchmark

Measured 2026-09-21 on Windows x64 with MSVC 19.50.35724, Release (/O2), C++20.
Baseline: repository commit `c212e60`, compiled against its original headers.
Optimized: this change, using the same `sparse_query.cpp` benchmark.

100,000 entities have Common; Rare occurs on 1%, 10%, or 100% of entities,
distributed by entity index modulo 100. Both components remain sparse.
The retained query uses a Where predicate and accumulates both component values
into a printed checksum. Each process warms up 10 queries, then times 200 queries.
The table reports the median of three alternating baseline/optimized process runs.
Construction and capacity reservation are outside the timed region; the optimized
world reserves index capacity, while the baseline uses its original creation API.

| Coverage | Baseline candidates | Optimized candidates | Baseline µs/query | Optimized µs/query |
| --- | ---: | ---: | ---: | ---: |
| 1% | 100,000 | 1,000 | 337.180 | 5.938 |
| 10% | 100,000 | 10,000 | 514.073 | 78.966 |
| 100% | 100,000 | 100,000 | 1970.370 | 583.831 |

All checksums agree. Raw output: [sparse_query_results.txt](sparse_query_results.txt).
Candidate counts describe the loop's input before Where and membership filters,
not hardware memory-access counters. The 100% case cannot benefit from candidate
reduction; it benefits from the binding/lookup changes. This microbenchmark does
not measure EnTT, creation/removal throughput, cold one-shot query setup, or an
engine workload. No dense-storage migration or sparse add/remove algorithm change
is included. Shared-machine timing remains subject to noise.

Build and run:

```sh
cmake -S . -B build_sparse_query -DEKIT_BUILD_BOIDS=OFF -DEKIT_BUILD_QUERY_BENCHMARK=ON
cmake --build build_sparse_query --config Release
ctest --test-dir build_sparse_query -C Release --output-on-failure
./build_sparse_query/Release/ekit_sparse_query_bench
```

To reproduce the baseline, compile this same benchmark with `EKIT_QUERY_BASELINE`
defined and the original `include` directory from `c212e60`. This disables only
the new reservation and candidate-count APIs in the harness.

Regression coverage in `tests/sparse_query.cpp` verifies all three coverages,
serial/parallel results, reversed required-component order, With/Where/Optional/
Without, explicit registration errors, cache refresh after registration and new
archetypes, column reallocation, moves between existing archetypes, changing the
smallest pool, swap-and-pop, destruction and entity reuse, component clear and
world clear, and append/reserve reference stability. Existing owning-component
lifetime tests continue to verify resource destruction and sparse removal.
