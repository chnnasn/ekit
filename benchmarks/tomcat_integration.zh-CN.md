# TomCat dev_ekit 接入审计

[English](tomcat_integration.md) | [简体中文](tomcat_integration.zh-CN.md)

**本文是 `01f923f` 之前、针对适配层 `5a89e22` 的历史审计。**
`01f923f` 已新增 const/mutable SceneWorld::ForEach，改造动画/粒子的七处循环，
并移除 TryGet 的重复 Has。该接口直接绑定 ekit 池，不委托 World::Query，固定依赖未改变。
最新结果见[EnTT/SceneWorld 对比](ecs_comparison.zh-CN.md)，
测试环境见 [TomCat_Engine / dev_ekit](https://github.com/chnnasn/TomCat_Engine/tree/dev_ekit)。

2026-09-21 审计时，通过 git show 读取本地 `E:/Github/TomCat_Engine` 的 dev_ekit 分支，
提交 `5a89e22af3f8d3cf52728c0a80dc1153f63de8ad`；未切换或修改该仓库。
当时依赖固定为 `82d4de67f37d5d146bb7287e07116dc7567af996`。
以下源码行号和建议均针对该历史快照。

## 实际调用路径

`TomCat/src/TomCat/Scene/SceneWorld.h` 自行实现 EntityRange/View：

- 89–97 行选择最小必选稀疏池。
- 105–109 行通过 HasAll 推进候选，对每个组件调用 Has，重复检查注册、实体有效性和成员关系。
- 116–117 行通过类型擦除的 EntityAt 函数指针和 World::GetEntity 重建实体句柄。
- 125–130 行的 View::Get 再调用 World::Get，在成员检查后重新查找组件。
- 30–33 行的 TryGet 先调用 Has；这也提供了未注册组件返回空指针的语义。

Scene.cpp 渲染/运行时使用 View 后再 Get，例如 789 行附近的精灵循环。
该快照不暴露 World::Query，因此这些循环不经过其专用执行器。库内 ID 复用仍可能带来收益，
但 Query 微基准的收益不能直接当作引擎循环收益。

## 引用稳定性

Scene.cpp 1121–1156 行将场景组件（包括 Transform）显式注册为稀疏存储。
迁移文档说明创建实体期间会保留组件引用。Advanced2DRegression.cpp 的 TestEkitSceneLifetime
在约 51 行保存 Transform 指针，创建 1,024 个实体后要求地址不变。
这是明确的接入契约；在审计和修改契约前，应保留 Transform 稀疏存储。
删除仍有交换删除导致的引用失效边界。

## 适配层基准

`tomcat_scene_world.cpp` 包含该分支原始 SceneWorld.h，使用两个 8 字节替代组件，
不是引擎真实 GLM Transform 或物理组件布局，因此只测适配层开销。
MSVC 19.50.35724、Windows x64、Release，10 万实体，每版六轮去首轮取中位数。
计时包含 200 次 View + Get 更新，不含创建和拾取 ID 分配；基线先于优化版运行。
两版适配层相同，ekit 分别为 `3e0daa8` 和 `3fcda56`，并非旧固定依赖 `82d4de67` 的对照。

| 覆盖率 | 候选量 | 3e0daa8 ms | 3fcda56 ms | 耗时减少 |
| --- | ---: | ---: | ---: | ---: |
| 1% | 1,000 | 5.866 | 4.667 | 20.4% |
| 10% | 10,000 | 61.444 | 48.687 | 20.8% |
| 100% | 100,000 | 606.138 | 450.653 | 25.7% |

每轮通过 const View/Get 验证预期校验和和实体数，新旧结果一致。
原始数据：[基线](tomcat_scene_baseline.csv)、[优化版](tomcat_scene_optimized.csv)。
共享机器微基准存在波动，未构建完整 TomCat 应用或运行全量原生回归。

## 01f923f 之前记录的建议

1. 更新依赖时同步来源记录，并执行 TomCat 原生回归；当时审计未更新依赖。
2. 为热点新增组件引用回调，并保留 const 语义。当时建议委托 World::Query；最终 `01f923f` 直接绑定池，兼容旧依赖和 const。
3. TryGet 应检查注册后直接调用 World::TryGet，保留未注册和过期句柄返回空指针；此项已在 `01f923f` 完成。
4. 剩余 View 热点可优化适配器绑定或增加上游 const 正确的 range API，不应引入第二套组件存储。

## 构建

将历史分支快照的 SceneWorld.h 导出到临时目录，然后运行：

```sh
cmake -S . -B build_sparse_query -DEKIT_BUILD_BOIDS=OFF -DEKIT_BUILD_QUERY_BENCHMARK=ON -DEKIT_TOMCAT_SCENE_WORLD_DIR=<directory-containing-SceneWorld.h>
cmake --build build_sparse_query --config Release --target ekit_tomcat_scene_bench
./build_sparse_query/Release/ekit_tomcat_scene_bench
```

未提供头文件目录时不启用该目标；适配器头文件不复制进 ekit 的受版本控制源码。
