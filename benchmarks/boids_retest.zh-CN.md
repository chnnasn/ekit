# Boids 当前版本重测

[English](boids_retest.md) | [简体中文](boids_retest.zh-CN.md)

2026-09-21 实测，版本 `055e1c9`（核心优化 `4d2d014`）。这次重新编译并运行当前源码，没有运行旧版或 EnTT，不是跨版本提速或 EnTT 对比。

Intel Core i7-14650HX，Windows 11 x64 10.0.26200，MSVC 19.50.35724 Release /O2。800×600 世界、种子 20260810，20 步预热 + 120 步计时，六轮舍弃首轮，取其余五轮中位数。所有 24 个逻辑 CPU 可调度，不固定单核；不同程序串行运行，每轮交替先后顺序。3 线程为后续单独补跑的六轮，可能受机器状态变化影响；CSV 保留每组最小/最大值供判断波动。

主图使用 1、2、3、4 线程，24 线程为额外扩展点。加速比按各路径自身单线程中位数计算，并非原始 parallel 输出中的 orig 单线程基准。

## 每步耗时

### 系统调度器

| boids | 1 (ms) | 2 (ms) | 3 (ms) | 4 (ms) | 24 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 200 | 0.1408 | 0.1184 | 0.0778 | 0.0770 | 0.0775 |
| 500 | 0.7452 | 0.4979 | 0.3268 | 0.3275 | 0.3260 |
| 1000 | 2.3783 | 1.4703 | 1.0227 | 1.0142 | 1.0323 |
| 2000 | 7.7274 | 4.5938 | 3.1555 | 3.1947 | 3.1698 |
| 5000 | 31.2551 | 18.2715 | 12.3555 | 12.5305 | 12.4350 |
| 10000 | 102.1213 | 59.6865 | 67.7146 | 40.3539 | 40.3134 |

10,000 boids：4 线程相对本路径单线程加速 **2.53×**；24 线程为 **2.53×**。

### 查询数据并行 + 并行网格

| boids | 1 (ms) | 2 (ms) | 3 (ms) | 4 (ms) | 24 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 200 | 0.1437 | 0.1634 | 0.1227 | 0.1424 | 0.1417 |
| 500 | 0.7483 | 0.4981 | 0.3941 | 0.3193 | 0.7458 |
| 1000 | 2.3630 | 1.3972 | 0.9906 | 0.7614 | 2.3535 |
| 2000 | 7.6955 | 4.0827 | 2.8418 | 2.2126 | 1.3341 |
| 5000 | 30.4961 | 15.8150 | 19.7172 | 8.2029 | 3.1622 |
| 10000 | 100.8996 | 51.0233 | 57.9652 | 25.6267 | 8.4852 |

10,000 boids：4 线程相对本路径单线程加速 **3.94×**；24 线程为 **11.89×**。

![规模曲线](chart_boids_current_cost.zh.png)

![线程扩展](chart_boids_current_speedup.zh.png)

## 解释与验证范围

- 10,000 boids 的 3 线程中位数慢于 2 线程，且补测波动明显：调度器 40.52–72.35 ms，数据并行 35.33–59.15 ms。保留原始结果，不将其解释为 3 线程必然更慢。
- 调度器路径并行执行不同规则系统；数据并行路径按依赖顺序执行系统，但在每个系统和网格重建内部并行。两者不能共用一个扩展上限结论。
- Boids 显式注册密集组件；本轮分页稀疏存储的收益不能直接套用。固定世界下，邻居扫描成本仍会随密度增加。
- 程序只计模拟步，不含创建、渲染、GLFW 或编辑器。GIF 中旧 FPS 不代表本次结果。
- 两个 Release 基准编译通过。所有 parallel 程序运行均未出现 DIFF：并行路径及 orig/24 的最终位置 FNV 校验和与 orig/1 相同。这不是全部组件状态或所有场景的严格相等证明。
- 历史数据保留，不根据跨日期差异宣称版本提速；共享机器与异构核心调度会造成波动。

## 原始数据与复现

[CSV](boids_retest.csv) · [Environment / source hashes](boids_retest_environment.json) · [Raw runs](boids_retest_raw/)

```powershell
cmake -S . -B build_boids_retest -DEKIT_BUILD_BOIDS=ON -DEKIT_BUILD_BOIDS_LIVE=OFF -DEKIT_BUILD_ENTT_COMPARE=OFF
cmake --build build_boids_retest --config Release --target ekit_boids_bench ekit_boids_bench_parallel
python benchmarks/run_boids_retest.py build_boids_retest/examples/boids/Release --threads 1,2,4,24
python benchmarks/run_boids_retest.py build_boids_retest/examples/boids/Release --threads 3 --tag _3
python benchmarks/analyze_boids_retest.py
python benchmarks/write_boids_report.py
```

分析与绘图需要 Python 和 matplotlib。历史报告见[基准索引](README.zh-CN.md)。
