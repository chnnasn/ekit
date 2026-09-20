# ekit benchmark report

[English](ecs_comparison.md) | [简体中文](ecs_comparison.zh-CN.md)

This report records the supplied retest results; it is separate from the local
version-to-version microbenchmarks. Per-round raw output for this retest is not
included in this repository.

### ekit vs EnTT: native ECS and engine integration

The native ECS test uses **ekit `3fcda56`**; the engine adapter is **`01f923f` on
`dev_ekit`**. The comparison uses **EnTT 3.15.0**, as used by TomCat's `main` branch.
Test environment and integration code:
[TomCat_Engine / dev_ekit](https://github.com/chnnasn/TomCat_Engine/tree/dev_ekit).

Hardware/software: **Intel Core i7-14650HX, Windows x64, MSVC Release**.
Tests run **single-threaded for six rounds, discarding the first and reporting
the median of the remaining five**.

Native ECS results for **100,000 entities**, in milliseconds; lower is better:

| Operation | EnTT | ekit sparse | ekit relative time |
| --- | ---: | ---: | --- |
| Create entities and add two components | 4.07 | 8.80 | 2.17× |
| Two-component traversal/update, 200 passes | 35.85 | 49.19 | 37% more |
| Random reads, 10 passes | 3.72 | 13.97 | 3.75× |
| Component add/remove, 10 rounds | 24.93 | 12.86 | 48% less |
| Destroy all entities | 5.80 | 1.69 | 71% less |

Across three successive benchmark reports, sparse traversal time relative to EnTT narrowed from
**7.28× → 3.26× → 1.37×**. Query optimization has substantially improved traversal;
random reads and entity creation still trail EnTT.

**Dense storage** completes the same 200 traversal passes in **13.74 ms**, about
**62% less time** than EnTT's 35.85 ms. Its component add/remove time is **6.55×**
EnTT's, however. It suits frequently traversed data with infrequent structural
changes; these results do not make dense storage universally preferable.

The new **`SceneWorld::ForEach`** adapter was separately tested using the dependency
actually pinned by `dev_ekit`, rather than assuming it uses the native test's ekit
revision:

| Second-component coverage | EnTT | SceneWorld | Relative time |
| --- | ---: | ---: | --- |
| 1% | 0.455 ms | 0.575 ms | About 26% more |
| 10% | 3.991 ms | 5.786 ms | About 45% more |
| 100% | 35.687 ms | 41.959 ms | About 18% more |

All adapter measurements use **100,000 entities and 200 updates**, excluding
creation and editor picking management.

In this workload, sparse component churn and entity destruction outperform the
EnTT comparison, sparse traversal is much closer, and dense traversal outperforms
EnTT for two simple data components. The next priorities are random access and
creation overhead while preserving reference validity, lifetimes and query
semantics. **These results do not generalize directly to complex components,
multithreaded systems or whole-engine frame rates.**
