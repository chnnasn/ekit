# ekit 基准测试

[English](README.md) | [简体中文](README.zh-CN.md)

ekit Boids 基准与 ekit vs EnTT 对比的原始数据、测试条件与分析。生成于 **2026-08-14**。

## 代码版本

- Git 提交：`227ea33`（`227ea33c7c629b07da2e858dc6376d17fba0dc52`）
  "Polish C#-like ergonomic API, dual storage, stream processing and docs"
- 基准源码：`examples/boids/bench.cpp`（ekit Boids）、
  `examples/boids/compare_bench.cpp` + `examples/boids/entt_impl.hpp`
  （ekit vs EnTT）、`examples/boids/boids.hpp`（模拟核心）
## 测试条件

| 项目 | 值 |
| --- | --- |
| CPU | Intel Core i7-14650HX（16 核 / 24 线程） |
| 操作系统 | Windows 10/11 x64 |
| 编译器 | MSVC 19.50（VS 2026），`/O2`，C++20，Release x64 |
| 世界 | 800 x 600 |
| 种子 | 20260810 |
| 算法 | 分离 / 对齐 / 聚合 / 边界四条规则，均匀空间网格（单元 = 邻居半径 48），**网格单元按实体 id 排序**，保证邻居累加顺序确定 |

`ekit_boids_bench`（单库基准）：

| | |
| --- | --- |
| boid 数量 | 200, 500, 1000, 2000, 5000, 10000 |
| 线程数 | 1, 2, 4, 24 |
| 计时步数 | 120（+20 预热） |

`ekit_entt_compare`（EnTT v4 vs ekit，相同算法）：

| | |
| --- | --- |
| boid 数量 | 200, 1000, 5000, 10000 |
| 线程数 | 1, 2, 3, 4 |
| 计时步数 | 30（+10 预热） |
| 校验 | 两边产生位级一致的状态（`state identical: YES`） |

> 说明：均为单次测量，且运行在共享机器上，期望波动 ±10-30%。请比较相对比值而非绝对值。

## 文件

| 文件 | 说明 |
| --- | --- |
| `ekit_boids_bench_raw.txt` | ekit Boids 基准的原始控制台输出 |
| `ekit_boids_bench.csv` | 相同数据，机器可读 |
| `entt_vs_ekit_raw.txt` | ekit vs EnTT 对比的原始控制台输出 |
| `chart_cost_vs_boids.png` | 每步耗时 vs boid 数（对数坐标） |
| `chart_speedup_vs_threads.png` | 加速比 vs 线程数 |
| `chart_throughput.png` | 吞吐量（k boids/s）vs boid 数 |
| `analyze.py` | 解析原始数据并重新生成 CSV/图表的脚本 |

## 分析

### boid 数量与每步耗时

4 线程实测（ms/步）：

| 从 | 到 | boid 倍数 | 耗时倍数 | 指数 |
| --- | --- | --- | --- | --- |
| 200 | 500 | 2.5x | 4.82x | 1.72 |
| 500 | 1000 | 2.0x | 2.91x | 1.54 |
| 1000 | 2000 | 2.0x | 3.14x | 1.65 |
| 2000 | 5000 | 2.5x | 3.86x | 1.47 |
| 5000 | 10000 | 2.0x | 3.26x | **1.71** |

耗时按指数约 1.5-1.7 增长（介于线性与平方之间），且指数**随密度上升趋向 2**。原因：世界尺寸固定，boid 翻倍 → 密度翻倍 → 每只 boid 的邻居数翻倍；近邻搜索为 O(n x 邻居数)，均匀密度极限下即 O(n^2)。吞吐量从 200 只时的约 265 万 boids/s（4 线程）跌至 10000 只时的约 23.8 万 boids/s。

### 不同线程数下的并行扩展

| boid 数 | t2 | t4 | t24 |
| --- | --- | --- | --- |
| 200 | 1.26x | 1.97x | 1.92x |
| 1000 | 1.64x | 2.39x | 2.39x |
| 10000 | 1.69x | 2.50x | 2.51x |

在这些测量中，超过 **4 线程**后加速比变化较小。依赖图在阶段 1 中包含 4 条可并行的规则系统，而空间网格重建与二系统阶段二链为串行部分。观测到的约 2.0-2.5x 加速比与这些串行工作及四条规则之间的负载差异一致（对齐和聚合扫描的邻居多于分离）。

### ekit vs EnTT（相同算法，EnTT v4）

- 单线程：ekit 比 EnTT 慢约 20%（`ekit/entt` 在 1000+ 只 boid 时约 1.20）。
- 4 线程：ekit 调度器慢约 1.7-1.9 倍（`ekit/entt` 在 5000/10000 只时约 1.75-1.91），而控制变量的数据并行路径 `ekit-dp` 将差距缩小到约 1.1-1.13 倍。两者均产生位级一致的状态。
## 复现

```powershell
cmake -S . -B build -DENTT_ROOT=E:/Github/entt   # EnTT 克隆在仓库外
cmake --build build --config Release --target ekit_boids_bench ekit_entt_compare
.\build\examples\boids\Release\ekit_boids_bench.exe     *> benchmarks\ekit_boids_bench_raw.txt
.\build\examples\boids\Release\ekit_entt_compare.exe    *> benchmarks\entt_vs_ekit_raw.txt
python benchmarks\analyze.py
```
