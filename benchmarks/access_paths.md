# Specialized query and random-access paths

[English](access_paths.md) | [简体中文](access_paths.zh-CN.md)

This is a local version-to-version measurement. The [latest EnTT comparison](ecs_comparison.md)
separately reports ekit `3fcda56` and TomCat adapter `01f923f`.

Measured 2026-09-21, Windows x64, MSVC 19.50.35724, C++20 Release (/O2).
Baseline headers are from `3e0daa8`; optimized headers are from `3fcda56`.
Both binaries compile the exact same `access_paths.cpp` benchmark. These results
compare ekit versions, not EnTT, and are not the external engine integration test.

## Method

100,000 entities, two 8-byte components. Each binary runs six rounds; discard
round 0 and take the median of rounds 1–5. Baseline runs precede optimized runs.
Creation includes Create plus two Add calls, without world/pool reservations.
Each traversal mode performs 200 updates. Random reads perform ten passes of Get
over a fixed shuffled entity order (mt19937 seed 20260921). Churn adds a third
component to every entity and removes it, ten times. Destruction removes all
entities. Registration and the harness entity-vector reservation are outside the
creation timer. Checksums are computed after traversal, outside its timed region.

Retained and freshly constructed queries are measured separately. Dense batch
iteration uses the existing ForEachBatch API and serves as a separate reference.
Every run's checksums match between versions, including all traversal modes.

## Median elapsed time

| Storage | Operation | 3e0daa8 ms | Optimized ms | Change |
| --- | --- | ---: | ---: | ---: |
| Sparse | Create + two components | 9.657 | 9.222 | -4.5% |
| Sparse | Retained ForEach, 200 updates | 106.124 | 48.714 | -54.1% |
| Sparse | Fresh ForEach, 200 updates | 106.822 | 50.373 | -52.8% |
| Sparse | Random Get, ten passes | 18.243 | 14.367 | -21.2% |
| Sparse | Add/remove, ten rounds | 16.124 | 14.002 | -13.2% |
| Sparse | Destroy all | 1.627 | 1.703 | +4.7% |
| Dense | Create + two components | 19.311 | 20.199 | +4.6% |
| Dense | Retained ForEach, 200 updates | 83.723 | 9.955 | -88.1% |
| Dense | Fresh ForEach, 200 updates | 83.128 | 9.846 | -88.2% |
| Dense | ForEachBatch, 200 updates | 6.161 | 5.972 | -3.1% |
| Dense | Random Get, ten passes | 13.277 | 13.195 | -0.6% |
| Dense | Add/remove, ten rounds | 160.343 | 163.814 | +2.2% |
| Dense | Destroy all | 1.274 | 1.320 | +3.6% |

Small changes of a few percent should be treated as inconclusive on this shared
machine. In particular, the measurements do not establish a destruction regression
or improvement. The storage representation, relocation rules and destruction
algorithm are unchanged. The large dense scalar improvement supports removing
the generic per-row path; it does not establish the cause of every difference in
the user's separate engine benchmark. Random-access costs still include entity
validity checks and sparse-index/deque addressing.

Raw data: [baseline](access_paths_baseline.csv), [optimized](access_paths_optimized.csv).

## Coverage regression benchmark

The existing Where + checksum benchmark (`sparse_query.cpp`) was also compiled
against both versions, with identical reservations. Six alternating pairs of
process runs, discard pair 0, median of the remaining five. Each process performs
10 warmups and 200 timed queries. This differs from the update-only benchmark above.

| Coverage | Candidates (both versions) | 3e0daa8 µs/query | Optimized µs/query |
| --- | ---: | ---: | ---: |
| 1% | 1,000 | 5.878 | 2.908 |
| 10% | 10,000 | 73.921 | 31.677 |
| 100% | 100,000 | 572.338 | 270.806 |

All coverage checksums match. [Raw data](access_paths_coverage.txt).
Candidate reduction is unchanged; this round reduces the cost per candidate.

## Implementation and correctness

- Dense scalar queries bind column bases outside the row loop, without redundant
  storage-kind or required-membership checks per component. Bases are refreshed
  on every execution, so reserve/reallocation between executions is safe.
- All-sparse queries dispatch once on the smallest pool's type and read its
  component by pool position. Other pools use sparse lookup. No archetype/row
  lookup is needed for scalar callbacks, predicates or Count. The low-level Visit
  adapter still resolves the location because its callback explicitly requests it.
- Mixed queries retain the generic membership-aware path, including cases where
  only Optional or Without introduces a different storage kind.
- World operations reuse a validated component ID for sparse storage access.
  Registration and entity generation checks are retained; no unchecked public
  accessor was added.

Tests cover dense optional columns, predicates, exclusions, serial/parallel parity,
column reallocation, all-sparse driver switching, optional aliasing of the driver,
duplicate required types, exclusion of a required type, sparse queries with dense
optional components, stale handles, const access, missing components and missing
registration. Existing lifetime, structural mutation-between-queries and coverage
tests remain enabled. Structural mutation inside callbacks remains unsupported.

## Reproduce

```sh
cmake -S . -B build_sparse_query -DEKIT_BUILD_BOIDS=OFF -DEKIT_BUILD_QUERY_BENCHMARK=ON
cmake --build build_sparse_query --config Release
ctest --test-dir build_sparse_query -C Release --output-on-failure
./build_sparse_query/Release/ekit_access_paths_bench
./build_sparse_query/Release/ekit_sparse_query_bench
```

For the baseline, compile the same benchmark sources with the same compiler and
flags but using the `include` directory from `3e0daa8`. Do not define
EKIT_QUERY_BASELINE here: both versions support the reservation and CandidateCount
APIs, so the coverage harness must use identical setup on both sides.
