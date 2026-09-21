# Random access and creation optimization

[English](random_create.md) | [简体中文](random_create.zh-CN.md)

This local study compares the headers at `43e18e1` (same implementation as
`3fcda56`) with the subsequent working-tree optimization. It is not a new EnTT
comparison and does not replace the [published integration retest](ecs_comparison.md).

## Changes and tradeoffs

- Replace the component `std::deque` with allocator-backed pages. Each page holds
  a power-of-two number of elements targeting 4 KiB, or one element for larger
  types. On the tested MSVC STL, an 8-byte deque element has only two elements per
  block; the new page holds 512. Indexing no longer uses deque's wrapping map.
- Append and reserve keep live component addresses unchanged. Reserve allocates
  raw storage without constructing T. Removal still uses swap-and-pop and destroys
  the removed/moved-last object as before. Owning resources are released promptly.
- Empty pages are retained for reuse after removal. ClearComponent, ClearAll and
  storage destruction destroy live components and release component pages. This
  retains peak capacity and adds up to one partially used page per pool; it is an
  explicit memory/throughput tradeoff, not universally lower memory use.
- Destroyed slots store the next generation with index zero. IsAlive still checks
  a valid, in-range, fully matching handle, but no longer reads the separate alive
  byte array. Null, stale and guessed next-generation handles remain invalid until
  the slot is actually reused. Enumeration still uses the alive array.
- Cache the empty archetype ID instead of looking it up on every Create.

## Measurement method

Windows x64, MSVC 19.50.35724 Release, 100,000 entities, two 8-byte components.
Both binaries compile identical benchmark sources. `run_random_create.ps1` pins
its child processes to logical CPU 0 (affinity mask 1), alternates baseline/new
order each round, runs six rounds, and discards round 0. Tables use medians of the
remaining five. Dense and sparse workloads use separate processes to avoid heap
state inherited from the preceding workload. Shared-machine noise still applies.

Early unrestricted measurements were unstable, including on dense operations;
the data below was collected after isolating processes and applying affinity.
Absolute times should not be compared with previous unpinned reports. All access
and setup checksums match between versions; coverage candidate counts also match.

## End-to-end operations

`access_paths.cpp`, milliseconds. Creation interleaves Create and two Add calls;
component pools are not reserved. Random reads use a fixed shuffled order for ten
passes. Churn adds/removes a third component ten times. Queries perform 200 updates.

| Storage | Operation | Baseline ms | New ms |
| --- | --- | ---: | ---: |
| Sparse | Create + two components | 10.8015 | 7.2465 |
| Sparse | Retained ForEach, 200 updates | 56.4447 | 37.2220 |
| Sparse | Fresh ForEach, 200 updates | 55.4720 | 35.9398 |
| Sparse | Random Get, ten passes | 24.5535 | 10.7215 |
| Sparse | Add/remove, ten rounds | 15.8115 | 10.9786 |
| Sparse | Destroy all | 2.5893 | 1.4869 |
| Dense | Create + two components | 21.6508 | 20.1904 |
| Dense | Retained ForEach, 200 updates | 10.9662 | 10.9150 |
| Dense | Fresh ForEach, 200 updates | 10.9627 | 9.8728 |
| Dense | ForEachBatch, 200 updates | 5.7977 | 6.1099 |
| Dense | Random Get, ten passes | 18.8834 | 15.3622 |
| Dense | Add/remove, ten rounds | 186.1050 | 185.7160 |
| Dense | Destroy all | 1.3116 | 1.6976 |

Sparse random reads take about 56% less time, creation 33% less, churn 31% less,
and destruction 43% less in this workload. Dense destruction is slower in this
sample (+29%, about 0.39 ms); the result is not an across-the-board improvement.
Dense traversal/churn is broadly similar, and short operations remain sensitive
to scheduling and code/heap layout. No whole-engine frame-time claim is made.
Raw data: [baseline](paged_access_baseline.csv), [new](paged_access_optimized.csv).

## Creation and access breakdown

`random_create.cpp` first creates all entities, then adds each component in a
separate pass. Its allocation pattern differs from the interleaved test above.
Pool Get bypasses World registration/handle validation; ComponentAt also bypasses
the sparse index. Their times cannot be subtracted to isolate individual costs
because compiler and cache behavior differ. All random reads execute ten passes.

| Reserved | Operation | Baseline ms | New ms |
| --- | --- | ---: | ---: |
| No | Create entities only | 4.3755 | 3.5476 |
| No | Add first component | 3.4269 | 2.1887 |
| No | Add second component | 3.0644 | 1.6540 |
| No | Total setup, including reserve timer | 11.5425 | 7.5518 |
| No | Random IsAlive | 4.6340 | 2.9190 |
| No | Random World::Get | 23.1601 | 12.4598 |
| No | Random pool Get | 8.0105 | 4.7857 |
| No | Random ComponentAt | 2.7856 | 1.6584 |
| Yes | Reserve itself | 0.0392 | 0.5661 |
| Yes | Create entities only | 2.1075 | 1.4465 |
| Yes | Add first component | 2.9095 | 0.9971 |
| Yes | Add second component | 2.9762 | 1.0045 |
| Yes | Total setup, including reserve | 7.9562 | 3.9672 |
| Yes | Random IsAlive | 3.8264 | 3.3320 |
| Yes | Random World::Get | 23.4973 | 19.0463 |
| Yes | Random pool Get | 7.8492 | 4.4382 |
| Yes | Random ComponentAt | 2.8433 | 1.7055 |

Reserve now allocates component pages too, so it costs more up front; the total
includes that cost. The total median is the median of each round's summed times,
not the sum of column medians. Raw data: [baseline](random_create_baseline.csv),
[new](random_create_optimized.csv).

## Query coverage regression

The existing Where/checksum benchmark uses six alternating process pairs on the
same logical CPU; discard pair 0. Each process warms up ten queries and times 200.

| Coverage | Candidates, both versions | Baseline µs/query | New µs/query |
| --- | ---: | ---: | ---: |
| 1% | 1,000 | 2.8805 | 2.3235 |
| 10% | 10,000 | 33.9990 | 22.6135 |
| 100% | 100,000 | 299.7120 | 204.0760 |

[Raw coverage output](paged_coverage.txt). Matching candidates and checksums show
that the optimization does not change query membership in these workloads.

## Validation and reproduction

Release CTest, MSVC AddressSanitizer (RelWithDebInfo with /EHsc), and the TomCat
dev_ekit SceneWorldRegression against these headers all pass. New tests cover
page growth/reservation, large and aligned components, failed constructors at
page boundaries, failed copies, copy/move ownership, immediate resource release,
Clear/reuse, and rejecting predicted-generation handles while slots are dead.
The full TomCat application was not rebuilt, and its vendored dependency was not
modified. No batch creation API or storage-kind migration was introduced.

```powershell
cmake -S . -B build_sparse_query -DEKIT_BUILD_BOIDS=OFF -DEKIT_BUILD_QUERY_BENCHMARK=ON
cmake --build build_sparse_query --config Release
ctest --test-dir build_sparse_query -C Release --output-on-failure
./benchmarks/run_random_create.ps1 -BaselineDirectory <baseline-bin-directory> -OptimizedDirectory ./build_sparse_query/Release
```

Build the baseline from the same benchmark sources using `43e18e1` headers and
identical compiler flags. The runner expects random_create, access_paths and
sparse_query benchmark executables in both directories and restores its shell's
original affinity afterwards. Optional benchmark arguments select one round
(0–5); access_paths also accepts `sparse` or `dense` as the second argument.
