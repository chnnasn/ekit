# ekit Boids：基于 ekit 的群集行为案例

[English](README.md) | [简体中文](README.zh-CN.md)

这是一个基于 **ekit** 实现的 Craig Reynolds Boids 群集算法案例。它演示了**显式组件注册、流式查询、声明式 `Reads/Writes` 系统依赖和自动并行调度**，并使用空间哈希网格进行邻居查询。

![boids](boids.gif)

## 10,000 boid 录制（无需 GLFW）

下面的动画 GIF 由**无头模式**（`ekit_boids`）渲染。只有交互式实时查看器需要 GLFW，因此无需安装 GLFW 也能查看模拟效果。每段录制均使用 10,000 只 boid、90 帧、30 fps、800x600 世界（缩小至 400x300）和固定种子。

模拟是**确定性的**：在种子和 boid 数量相同时，不同线程数会生成完全相同的帧。因此四段动画除了每帧标注的**运行时 FPS** 外，画面内容一致。800x600、10,000 只 boid、90 帧的结果如下：

| 线程数 | 运行时 FPS |
| --- | --- |
| 1 | ~12 fps |
| 2 | ~22 fps |
| 3 | ~28 fps |
| 4 | ~29 fps |

每帧标注格式为 `threads=N | ~X fps | frame i / 90`。

| threads = 1 | threads = 2 |
| --- | --- |
| ![10k boids，1 线程](boids_t1.gif) | ![10k boids，2 线程](boids_t2.gif) |
| threads = 3 | threads = 4 |
| ![10k boids，3 线程](boids_t3.gif) | ![10k boids，4 线程](boids_t4.gif) |

复现命令：

```powershell
cmake --build build --config Release --target ekit_boids
.\build\examples\boids\Release\ekit_boids.exe --boids 10000 --frames 90 --threads 1 --out frames_t1
powershell -ExecutionPolicy Bypass -File examples/boids/render.ps1 -Dir frames_t1 -Fps 30 -Out boids_t1.gif -Scale 0.5 -Label -Title "threads=1 | ~12 fps"
```

## ekit vs EnTT（相同算法）

同一套 boid 算法（规则计算、空间网格和阶段顺序均相同）分别基于 [EnTT](https://github.com/skypjack/entt)（v4，外部克隆，**不包含在本仓库中**）和 ekit 实现。校验和验证表明两种实现产生的状态**位级一致**，因此下面的计时只比较 ECS 层。

```powershell
# 首次在仓库外克隆 EnTT
git clone --depth 1 https://github.com/skypjack/entt.git E:\Github\entt
cmake -S . -B build -DENTT_ROOT=E:/Github/entt
cmake --build build --config Release --target ekit_entt_compare
.\build\examples\boids\Release\ekit_entt_compare.exe   # 4 组，左侧 EnTT / 右侧 ekit
```

对比包含三列：

- `ekit` — 依赖图调度器：整系统并行，每个系统单线程。
- `ekit-dp` — 数据并行查询路径。它与 EnTT 侧使用**相同**的动态分块和**相同**的存储驱动式组件访问（dense 数组驱动 + 每个组件稀疏查找、组件集合一致），因此这一列才是在隔离 ECS 层本身。

本机测量结果（Apple Silicon、clang、-O2，120 个计时步 + 20 个预热步，世界大小 800x600，种子 20260810；比值小于 1 表示 ekit 更快）：

```
threads = 1
boids    entt ms/step   ekit ms/step   ekit/entt  ekit-dp ms/step  ekit-dp/entt
200      0.0885         0.0856         0.967      0.0752          0.850
1000     1.5510         1.3593         0.876      1.3138          0.847
5000     20.7945        15.9294        0.766      15.2530         0.734
10000    70.1903        54.5702        0.777      52.1881         0.744

threads = 2
200      0.0917         0.0650         0.708      0.0902          0.983
1000     0.8197         0.8588         1.048      0.7278          0.888
5000     11.0227        9.9391         0.902      8.5136          0.772
10000    38.5296        34.7373        0.902      29.1513         0.757

threads = 3
200      0.0971         0.0615         0.634      0.0983          1.013
1000     0.6598         0.6413         0.972      0.5998          0.909
5000     8.2491         7.4971         0.909      6.0826          0.737
10000    26.3406        24.8070        0.942      19.7936         0.751

threads = 4
200      0.0932         0.0623         0.668      0.0837          0.898
1000     0.5460         0.6415         1.175      0.4941          0.905
5000     6.7052         7.4500         1.111      4.6617          0.695
10000    20.9687        25.8639        1.233      17.4223         0.831
```

结论：

- **控制变量对比（`ekit-dp`）**：在算法、分块、存储访问和组件集合完全一致的情况下，ekit 的 ECS 层在 5000/10000 只 boid、1-4 线程时比 EnTT v4 快约 17-31%（`ekit-dp/entt` 约 0.69-0.78），1000 只时快约 5-15%；200 只时两者基本相当。
- **调度器对比（`ekit`）**：原调度器在 1 线程时更快，但在 4 线程的密集负载下落后于 EnTT——这是并行方式的差异（整系统并行 vs 数据并行），而不是存储差异。
- `ekit` 与 `ekit-dp` 都与 EnTT 产生**位级一致**的状态。
- 空间网格中的每个单元都按实体 ID 排序，使邻居累加顺序一致，并在两侧保持位级确定性。

## 算法

每一帧中，每只 boid 都会根据邻居计算以下规则并调整运动方向：

1. **分离（Separation）**：避开距离过近的邻居
2. **对齐（Alignment）**：与邻居的平均朝向保持一致
3. **聚合（Cohesion）**：向邻居的质量中心移动
4. **边界（Bounds）**：靠近边缘时转向世界中心

每条规则分别写入自己的**累加器组件**（`Separation / Alignment / Cohesion / BoundsSteer`）；合并系统将这些结果应用到速度，积分系统再据此更新位置。

## ekit 功能对应

| ekit 功能 | 在本案例中的用法 |
|---|---|
| `EKIT_COMPONENT(T)` + `RegisterComponents<Ts...>()` | 显式注册 `Position / Velocity / BoidTag / Separation / Alignment / Cohesion / BoundsSteer` |
| 流式查询 `Query<Ts...>()` | 每个系统都通过 `Query<...>().ForEach(...)` 遍历 boid |
| `With<T>()` / 标记组件 | 每个查询都使用 `BoidTag` 作为标记 |
| `TryGet<T>(entity)` | 在邻居查询中安全访问邻居的 `Position / Velocity` |
| 系统 `Reads / Writes` | 每个系统都在类中声明 `using Reads/Writes = ekit::TypeList<...>` |
| 调度器依赖分析与并行执行 | 四个规则系统写入**互不重叠**的累加器，因此调度器会并行运行它们；合并与积分系统则按依赖链排序 |
| 基于世代编号的实体句柄 | `world.Create()` 返回带世代编号的 `Entity`，实体回收后旧句柄不会误指向新实体 |

## 调度器工作方式：两个阶段

每一帧分为**两个调度阶段**，这是典型的帧管线模式：

```
阶段 1（规则，完全并行）
  SeparationSystem ──(写入 Separation)───┐
  AlignmentSystem  ──(写入 Alignment)────┼──┐
  CohesionSystem   ──(写入 Cohesion)─────┼──┼──> 阶段 2
  BoundsSystem     ──(写入 BoundsSteer)──┘  │
                                             │
阶段 2（应用 + 积分）                        │
  UpdateVelocitySystem ──(写入 Velocity)────┘
        |
        v
  IntegrateSystem ──(写入 Position)──> 下一帧
```

阶段 1 的规则系统只**读取**帧开始时的 `Position/Velocity`，并分别写入自己的累加器，因此调度器可以让它们**完全并行**运行。阶段 2 中，`UpdateVelocitySystem` 读取四个累加器并写入 `Velocity`；`IntegrateSystem` 读取 `Velocity` 并写入 `Position`，两者按照依赖链依次执行。

**为什么要拆成两个阶段？** 这是本案例最值得关注的部分。如果读取 `Position` 的系统（规则系统）与写入 `Position` 的系统（积分系统）处在同一阶段，调度器会检测到循环数据流并抛出“dependency cycle”错误。原因是 `Reads/Writes` 只描述访问方式，无法表达“规则系统需要读取**上一帧的** Position”。将“读取帧开始状态”和“写入新状态”拆为两个阶段，可以让 DAG 保持无环。这是实际 ECS 引擎中常见的帧管线约束。

> 空间网格会在每帧开始时**同步重建**，在调度阶段保持只读，因此并行规则系统不存在数据竞争。

## 实时查看器

**`ekit_boids_live`** 使用 **GLFW + OpenGL** 打开实时窗口并通过 GPU 渲染。使用 `--vsync 0` 时可达到数百 FPS；默认 `--vsync 1` 时帧率与显示器刷新率同步。

GLFW 使用你的 fork（https://github.com/chnnasn/glfw），**不包含在本仓库中**。请将它克隆到仓库外部（例如 `E:\Github\glfw`），然后通过 `-DEKIT_GLFW_ROOT` 将路径传给 CMake：

```powershell
# 1) 首次克隆 GLFW（放在本仓库外部）
git clone --depth 1 https://github.com/chnnasn/glfw.git E:\Github\glfw

# 2) 配置并构建
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DEKIT_GLFW_ROOT=E:/Github/glfw
cmake --build build --config Release --target ekit_boids_live

# 3) 运行
.\build\examples\boids\Release\ekit_boids_live.exe --boids 220
```

窗口会一直保持打开，直到**你主动关闭**（按 ESC 或单击窗口关闭按钮），默认不会自动退出。传入 `--frames N` 可让程序在 N 个模拟步后关闭，适合自动化测试。

**调整或最大化窗口时，群集范围会随之变化**：世界边界、空间网格和 OpenGL 投影都会跟踪当前窗口尺寸，boid 的位置也会按比例缩放，因此群集始终在整个窗口中活动，而不是局限在固定的 800x600 区域。

窗口内操作：

| 输入 | 操作 |
|---|---|
| 移动鼠标 | 群集跟随光标 |
| 按住鼠标左键 | 驱散群集，使其远离光标 |
| `SPACE` | 暂停 / 继续 |
| `R` | 重置群集（随机重新生成） |
| `UP` / `DOWN` | 加速 / 减速（调整每个渲染帧执行的模拟步数） |
| `ESC` | 退出 |

窗口标题栏会实时显示 **boid 数量 / 群组数量 / FPS / 速度倍率**。默认移动速度刻意设置得比较平缓（最大约 1.6 px/frame）；需要更快效果时可按 `UP` 加速。

## 无头模式（帧 / 动画）

```bash
cmake --build build --config Release --target ekit_boids
./build/Release/ekit_boids.exe --boids 220 --frames 180 --width 800 --height 600
```

程序会写入 `frames/frame_0000.ppm ... frame_0179.ppm`，并输出最终统计信息，包括 boid 数量、平均速度和群组数量。

### 命令行选项

```
--boids N       boid 数量（默认 220）
--frames N      运行帧数（默认 180；实时模式中表示自动退出前的模拟步数，0 表示无限）
--width N       世界/渲染宽度（默认 800）
--height N      世界/渲染高度（默认 600）
--seed N        随机种子（默认 20260810）
--threads N     调度器线程数，0 表示硬件并发数（默认 0）
--vsync 0|1     实时模式：1 表示同步显示器刷新率，0 表示不限制（默认 1）
--out DIR       PPM 帧输出目录（仅无头模式，默认为 frames）
--help          显示帮助
```

### 渲染动画

Windows（使用内置 .NET 绘图功能，无额外依赖）：

```powershell
powershell -ExecutionPolicy Bypass -File examples/boids/render.ps1 -Dir frames -Fps 30 -Out boids.gif -PngDir png
```

该命令会在 `png/` 中生成逐帧 PNG，并输出 `boids.gif` 动画。也可以使用任何支持 PPM 的工具，例如 ImageMagick 或 ffmpeg：

```bash
ffmpeg -framerate 30 -i frames/frame_%04d.ppm boids.gif
```

## 性能测试

`ekit_boids_bench` 会在不同 boid 数量与线程数组合下，测量完整模拟步骤的耗时，包括网格重建、并行规则计算以及应用/积分：

```powershell
cmake --build build --config Release --target ekit_boids_bench
.\build\examples\boids\Release\ekit_boids_bench.exe
# 自定义矩阵：.\...\ekit_boids_bench.exe --boids 200,500,1000 --threads 1,2,4,0
```

本机测量结果（24 个硬件线程，世界大小 800x600，120 个模拟步）：

```
boids    threads   ms/step     steps/s    speedup   k boids/s   us/boid
------   -------   --------    -------    -------   ---------   --------
200      1         0.1300      7692.4     1.00      1538.5      0.650
200      4         0.0717      13946.8    1.81      2789.4      0.359

500      1         0.6234      1604.1     1.00      802.0       1.247
500      4         0.2789      3585.0     2.23      1792.5      0.558

1000     1         1.8703      534.7      1.00      534.7       1.870
1000     4         0.8176      1223.1     2.29      1223.1      0.818

2000     1         5.6782      176.1      1.00      352.2       2.839
2000     4         2.4551      407.3      2.31      814.6       1.228

5000     1         37.6368     26.6       1.00      132.8       7.527
5000     4         15.7870     63.3       2.38      316.7       3.157

10000    1         113.4400    8.8        1.00      88.2        11.344
10000    4         51.6270     19.4       2.20      193.7       5.163

single-boid throughput (best): 2.789 boids/us (0.359 us/boid) at 200 boids, 4 threads
```

主要结论：

- **`us/boid`** 表示每只 boid 在每个模拟步中的耗时（微秒），它是单 boid 吞吐量 `boids/us` 的倒数。该数值会随密度增长：在固定大小的世界中，boid 越多，每只 boid 的邻居也越多。因此在 4 线程下，每只 boid 的耗时从约 0.36 us（200 只）上升到约 5.2 us（10k 只）。
- **4 线程时，并行加速比约为 2.3x。** 依赖图在阶段 1 中包含 4 个并行规则系统，而空间网格重建和阶段 2 的两个系统为串行部分；这一结果与当前负载中的串行工作占比一致。
- **在固定大小的世界中，单步耗时随 boid 数量超线性增长。** boid 数量翻倍时密度也翻倍，每只 boid 在搜索半径内会发现更多邻居；邻居查询复杂度为 O(n * 平均邻居数)。
- 使用 4 线程时，10k 只 boid 仍可达到约 20 steps/s（吞吐量约 200k boids/s）；200 只 boid 可达到约 14.5k steps/s。

## 文件说明

- `boids.hpp`：组件、配置、空间网格、六个系统以及 `SpawnBoids` / `CountFlocks`（核心实现）
- `canvas.hpp`：最小化软件光栅器（三角形/圆形 + PPM 输出，供无头模式使用）
- `render_helpers.hpp`：boid 着色与三角形绘制
- `cli.hpp`：无头和实时模式共用的命令行解析
- `main.cpp`：无头模式，包括模拟、PPM 帧导出和统计信息
- `main_live.cpp`：实时模式，包括 GLFW + OpenGL 窗口和交互控制
- `render.ps1`：PPM → PNG / GIF 转换脚本
- GLFW（外部依赖，**不包含在本仓库中**）：在仓库外执行 `git clone https://github.com/chnnasn/glfw.git`
