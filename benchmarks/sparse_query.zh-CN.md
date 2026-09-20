# 稀疏查询覆盖率基准

[English](sparse_query.md) | [简体中文](sparse_query.zh-CN.md)

这是历史版本间微基准。ekit `3fcda56` / TomCat `01f923f` 的结果见[最新 EnTT 对比](ecs_comparison.zh-CN.md)。

2026-09-21 测量，Windows x64、MSVC 19.50.35724、Release（/O2）、C++20。
基线为 `c212e60`，优化版为 `3e0daa8`，两者使用同一份 `sparse_query.cpp`。

10 万实体均有 Common；Rare 按实体索引模 100 分布，覆盖率为 1%、10%、100%。
两个组件均采用稀疏存储。复用的查询包含 Where，并累加两个组件的值作为校验和。
每个进程预热 10 次，再计时 200 次查询；新旧版交替运行三轮，表格取中位数。
创建和预留容量不计时；优化版预留索引容量，基线使用原有创建 API。

| 覆盖率 | 基线候选量 | 优化后候选量 | 基线 µs/查询 | 优化后 µs/查询 |
| --- | ---: | ---: | ---: | ---: |
| 1% | 100,000 | 1,000 | 337.180 | 5.938 |
| 10% | 100,000 | 10,000 | 514.073 | 78.966 |
| 100% | 100,000 | 100,000 | 1970.370 | 583.831 |

所有校验和一致。[原始输出](sparse_query_results.txt)。候选量指成员检查和 Where 之前的循环输入，
不是硬件内存访问计数。100% 覆盖率没有缩小候选量，收益来自绑定和查找优化。
本测试不包含 EnTT、创建/删除吞吐量、一次性查询冷启动或完整引擎负载。
没有迁移到密集存储，也没有更改稀疏增删算法；共享机器上的耗时仍有波动。

## 复现

```sh
cmake -S . -B build_sparse_query -DEKIT_BUILD_BOIDS=OFF -DEKIT_BUILD_QUERY_BENCHMARK=ON
cmake --build build_sparse_query --config Release
ctest --test-dir build_sparse_query -C Release --output-on-failure
./build_sparse_query/Release/ekit_sparse_query_bench
```

复现历史数据时应使用对应版本的头文件。基线使用 `c212e60` 的 include 目录，
并定义 `EKIT_QUERY_BASELINE`，仅关闭基准中的新容量预留与候选计数 API。

`tests/sparse_query.cpp` 覆盖三档覆盖率、串并行结果、必选类型顺序交换、With/Where/Optional/Without、
显式注册错误、注册及新增 archetype 后缓存刷新、列扩容、现有 archetype 间迁移、最小池切换、
交换删除、销毁与实体复用、组件及世界清空，以及追加和预留容量时的引用稳定性。
原有资源组件生命周期测试继续验证资源销毁和稀疏删除。
