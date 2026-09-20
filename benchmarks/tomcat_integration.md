# TomCat dev_ekit integration audit

[English](tomcat_integration.md) | [简体中文](tomcat_integration.zh-CN.md)

**Historical audit of adapter `5a89e22`, before `01f923f`.** The latter adds
const/mutable SceneWorld::ForEach, migrates seven animation/particle loops and
removes TryGet's duplicate Has check. It binds ekit pools directly, rather than
delegating to World::Query. The pinned dependency remains unchanged. See the
[latest EnTT/SceneWorld comparison](ecs_comparison.md) and
[test environment](https://github.com/chnnasn/TomCat_Engine/tree/dev_ekit).
The source line numbers and recommendations below describe the older snapshot.

Reviewed 2026-09-21. The requested `E:/Github/TomCat/_Engine` directory was absent;
the local repository is `E:/Github/TomCat_Engine`, branch `dev_ekit`, commit
`5a89e22af3f8d3cf52728c0a80dc1153f63de8ad`. Files were read with `git show` without
checking out or changing that repository. Its vendored ekit provenance still pins
`82d4de67f37d5d146bb7287e07116dc7567af996`, not `3e0daa8` or the current changes.

## Actual integration path

`TomCat/src/TomCat/Scene/SceneWorld.h` contains its own EntityRange/View:

- Lines 89–97 select the smallest required sparse component pool.
- Lines 105–109 advance through candidates using HasAll, which calls Has for each
  component. These wrappers check registration, entity validity and membership.
- Lines 116–117 reconstruct the entity handle through a type-erased EntityAt
  function pointer and World::GetEntity.
- Lines 125–130 implement View::Get through World::Get, performing component
  lookup again after the membership checks.
- Lines 30–33 implement TryGet as Has followed by World::TryGet. The extra Has is
  also what gives this wrapper its unregistered-component-to-null behavior.

Scene.cpp rendering/runtime loops use View followed by Get, for example the sprite
loop near line 789. SceneWorld does not expose World::Query. Therefore the new
specialized Query executor is not reached by these loops. Reusing validated IDs
inside World accessors can still benefit this integration after updating the library.
Query benchmark gains must not be advertised as engine-loop gains.

## Reference stability

Scene.cpp lines 1121–1156 explicitly register scene components, including Transform,
as sparse. `docs/EKIT_MIGRATION.md` states that callers retain references while
creating entities. `Tests/Advanced2DRegression/src/Advanced2DRegression.cpp`,
TestEkitSceneLifetime near line 51, retains a Transform pointer, creates another
1,024 entities and requires the original address to remain unchanged.

This is an explicit integration contract, not just a hypothetical risk. Keep
Transform sparse unless the reference contract and its consumers are deliberately
changed and audited. Removal still has the existing swap-and-pop limitations.

## Adapter benchmark

`tomcat_scene_world.cpp` includes the unmodified SceneWorld.h exported from that
branch. It uses two minimal 8-byte surrogate components named Transform/Rigidbody,
not TomCat's actual GLM transform or physics component layouts. This isolates
adapter overhead; it is not a full scene/render/physics benchmark.

Same compiler as the access-path tests: MSVC 19.50.35724, Windows x64, Release.
100,000 entities; six rounds per binary, discard the first, median of five.
The timer covers 200 View + Get updates; creation and picking-ID allocation are
outside it. Both binaries use the same adapter source. Baseline is ekit `3e0daa8`,
optimized is ekit `3fcda56`. This comparison does not measure the old
vendored `82d4de67` revision. Baseline executes before optimized.

| Coverage | Candidates | 3e0daa8 ms | Current ms | Time reduction |
| --- | ---: | ---: | ---: | ---: |
| 1% | 1,000 | 5.866 | 4.667 | 20.4% |
| 10% | 10,000 | 61.444 | 48.687 | 20.8% |
| 100% | 100,000 | 606.138 | 450.653 | 25.7% |

Each run independently checks the expected update sum and matching entity count
through the const View/Get interface. Baseline and optimized checksums and candidate
counts also agree. Raw data: [baseline](tomcat_scene_baseline.csv),
[optimized](tomcat_scene_optimized.csv). Shared-machine microbenchmark limitations
apply; the complete TomCat application and native regression suite were not built.

## Recommendations recorded before 01f923f

1. Update the vendored dependency and provenance together, then run TomCat's native
   regressions. This audit did not modify TomCat or update its dependency pin.
2. Add a component-reference callback iteration path to SceneWorld for mutable hot
   loops, backed by World::Query::ForEach. Retain const View support and verify its
   semantics separately; a direct replacement with the mutable Query API would
   lose const correctness.
3. Reduce TryGet's duplicate lookup by checking registration, then calling
   World::TryGet directly. Preserve null returns for unregistered types and dead
   handles; blindly deleting Has would change the unregistered-type behavior.
4. If range-based View remains hot, optimize its membership/component binding as
   an adapter concern, or add an appropriately const-correct upstream range API.
   Do not implement a second component store in the adapter.

## Build the adapter benchmark

Export the branch's SceneWorld.h into a temporary directory, then configure:

```sh
cmake -S . -B build_sparse_query -DEKIT_BUILD_BOIDS=OFF -DEKIT_BUILD_QUERY_BENCHMARK=ON -DEKIT_TOMCAT_SCENE_WORLD_DIR=<directory-containing-SceneWorld.h>
cmake --build build_sparse_query --config Release --target ekit_tomcat_scene_bench
./build_sparse_query/Release/ekit_tomcat_scene_bench
```

The optional target is disabled unless the header directory is supplied. The
adapter header is not copied into the tracked ekit source tree.
