# Ekit

[English](README.md) | [简体中文](README.zh-CN.md)

**Ekit** 是一个面向游戏引擎、仅包含头文件的 **C++20 ECS**（实体-组件-系统）库。它结合稀疏集存储与受 C# LINQ 和 Unity DOTS 启发的显式流畅 API，侧重于提高常用 ECS 操作的可读性，并在查询迭代中避免类型擦除和逐实体虚函数调用。

[EnTT](https://github.com/skypjack/entt) 是一个成熟、功能丰富的 ECS 库。Ekit 规模较小，覆盖的功能范围也更窄，侧重显式注册和流畅查询。具体选择取决于项目需求。

## 设计理念

1. **显式优于隐式**
   - 组件必须在结构体中添加 `EKIT_COMPONENT(T)` 进行声明，并通过 `world.RegisterComponent<T>()` 显式注册，不进行隐式注册。
   - 使用未声明的组件会产生易读的 `static_assert`；使用未注册的组件会抛出清晰的 `EkitException`，并明确提示需要调用的方法。
   - 系统在类中声明自己的数据依赖（`Reads` / `Writes`）。

2. **流畅的现代 API（C# LINQ + Unity DOTS）**
   - 使用 PascalCase 方法，例如 `world.Create()`、`world.Query<Ts...>().ForEach(...)`。
   - 系统是带有 `Execute(World&)` 方法的类，风格类似 Unity DOTS。

3. **可读的诊断与类型化接口**
   - 避免模板错误刷屏：通过 `static_assert` 和 `if constexpr` 给出准确错误。
   - 实体采用强类型、带世代编号的句柄，而不是裸 `uint32_t`。
   - 流式查询链在编译期组合，不使用类型擦除或逐实体虚函数调用。

4. **核心架构**
   - 稀疏集存储、依赖感知的并行调度器、命名实体和事件系统，可作为声明式自动并行、编辑器集成和网络同步的基础组件。

## 快速开始

```cpp
#include <ekit/ekit.hpp>

struct Position {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(Position);
};

struct Velocity {
    float vx = 0.f;
    float vy = 0.f;
    EKIT_COMPONENT(Velocity);
};

int main() {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();

    auto ship = world.Create("ship");
    world.Add<Position>(ship, 10.f, 5.f);
    world.Add<Velocity>(ship, 2.f, 0.f);

    const float dt = 1.f / 60.f;
    world.Query<Position, Velocity>()
         .ForEach([dt](Position& p, Velocity& v) {
             p.x += v.vx * dt;
             p.y += v.vy * dt;
         });
}
```

## 功能

- **实体（Entity）**：强类型、带世代编号的句柄，可安全处理失效句柄（`Entity::Null`、`IsAlive`，槽位回收时自动递增世代编号）。
- **组件（Component）**：在类体内通过 `EKIT_COMPONENT(T)` 声明的 POD 结构体；使用 `world.RegisterComponent<T>()` 显式注册；采用稀疏集存储（缓存友好的密集数组与交换删除）。
- **世界（World）**：实体创建与销毁、组件 `Add / Emplace / Set / Get / TryGet / Has / Remove / Patch / Clear`、命名实体、批量注册和 `ClearAll`。
- **查询（Query）**：支持 `Where / With / Without / Optional / ForEach / Count` 的流畅查询，并从符合条件的存储中最小的那个开始迭代：
  ```cpp
  world.Query<Position, Velocity>()
       .With<Renderable>()
       .Without<Disabled>()
       .Optional<Health>()
       .Where([](Position& p, Velocity& v, Renderable&, Health* hp) {
           return hp == nullptr || hp->hp > 0;
       })
       .ForEach([](ekit::Entity e, Position& p, Velocity& v, Renderable&, Health* hp) {
           // ...
       });
  ```
  必需组件以引用传入，可选组件以指针传入（不存在时为 `nullptr`）；`Entity` 句柄是可选参数，存在时必须位于首位。
- **系统与调度器（System & Scheduler）**：系统声明 `Reads` / `Writes`；调度器构建依赖 DAG，并通过内部线程池并行执行相互独立的系统：
  ```cpp
  struct GravitySystem {
      using Writes = ekit::TypeList<Velocity>;
      void Execute(ekit::World& world) {
          world.Query<Velocity>().ForEach([](Velocity& v) { v.vy -= 9.8f; });
      }
  };

  ekit::Scheduler scheduler(4);           // 0 表示使用硬件并发数
  scheduler.AddSystem(GravitySystem{})
           .AddSystem(MoveSystem{});
  scheduler.Run(world);                   // 也可以使用 RunSingleThreaded(world)
  ```
  写入者会排在读取同一组件的读取者之前。两个写入同一组件的系统**不再形成依赖环**：它们按注册顺序串行执行。只有当声明的依赖真正互相矛盾（例如 A 写 X / 读 Y，而 B 写 Y / 读 X）时才会报告依赖环。
- **事件（Event）**：使用 `world.Subscribe<T>(handler)` / `world.Emit<T>(args...)`：
  ```cpp
  struct HitEvent { int damage; ekit::Entity target; };
  ekit::EventSubscription sub = world.Subscribe<HitEvent>(
      [](const HitEvent& ev) { /* ... */ });
  world.Emit<HitEvent>(10, target);
  sub.Unsubscribe();
  ```

## 集成

仅包含头文件、零运行时依赖：

- **CMake**
  ```cmake
  add_subdirectory(ekit)
  target_link_libraries(app PRIVATE ekit::ekit)
  ```
- **手动**：将 `include/` 加入包含路径，然后 `#include <ekit/ekit.hpp>`。

需要 C++20（MSVC 19.29+、GCC 11+、Clang 14+）。

## 案例：Boids

`examples/boids/` 是一个基于 ekit 构建的鸟群模拟，演示了显式组件注册、流畅查询、带 `Reads/Writes` 声明的系统、并行调度器（四条规则系统并发执行）、空间哈希近邻查询和二阶段帧管线：

```bash
# 实时窗口（GLFW + OpenGL，GPU 渲染）。GLFW 不在本仓库内：
# 先在仓库外克隆 https://github.com/chnnasn/glfw，然后：
cmake -S . -B build -DEKIT_GLFW_ROOT=E:/Github/glfw
cmake --build build --config Release --target ekit_boids_live
./build/examples/boids/Release/ekit_boids_live.exe --boids 220    # SPACE 暂停, R 重置, ESC 退出

# 无头模式：PPM 帧 -> 动画 GIF（无需 GLFW）
cmake --build build --config Release --target ekit_boids
./build/examples/boids/Release/ekit_boids.exe --boids 220 --frames 180
powershell -ExecutionPolicy Bypass -File examples/boids/render.ps1 -Fps 30   # -> boids.gif
```

详见 [examples/boids/README.zh-CN.md](examples/boids/README.zh-CN.md)。

## 基准测试

完整测试条件、原始数据与分析脚本见 [`benchmarks/`](benchmarks/README.zh-CN.md)。要点结果（Intel i7-14650HX，24 线程，MSVC Release /O2，世界 800x600，种子 20260810）：

### boid 数量增加时的每步耗时

| 从 | 到 | boid 倍数 | 耗时倍数 | 指数 |
| --- | --- | --- | --- | --- |
| 200 | 500 | 2.5x | 4.05x | 1.53 |
| 1000 | 2000 | 2.0x | 3.07x | 1.62 |
| 5000 | 10000 | 2.0x | 3.41x | 1.77 |

每步耗时按 n^1.5..n^1.8 增长，且指数**随密度上升趋向 2**：世界尺寸固定，boid 数量翻倍 → 密度翻倍 → 每只 boid 的邻居数翻倍，近邻搜索为 O(n x 邻居数)，均匀密度极限下即 O(n^2)。吞吐量从 200 只时的约 270 万 boids/s 跌至 10000 只时的约 28.5 万 boids/s。

![每步耗时 vs boid 数](benchmarks/chart_cost_vs_boids.png)

### 不同线程数下的并行扩展

| boid 数 | t2 | t4 | t24 |
| --- | --- | --- | --- |
| 200 | 1.32x | 1.94x | 1.96x |
| 10000 | 1.69x | 2.23x | 2.44x |

在这些测量中，超过 **4 线程**后加速比变化较小。依赖图包含 4 条可并行的规则系统，而网格重建与阶段二链为串行部分，因此观测到的加速比约为 2.2-2.9x，而不是 4x。

![加速比 vs 线程数](benchmarks/chart_speedup_vs_threads.png)

### ekit vs EnTT（相同算法，EnTT v4）

在这项基准测试中，单线程时 ekit 的测量结果快约 15-25%，4 线程时 EnTT 的测量结果快约 25-30%。差异与两者采用的查询迭代和并行化策略一致；在该测试负载下，两种实现产生的模拟状态位级一致。

## 构建与测试

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

## 单元测试

`tests/tests.cpp` 随库一起发布，通过 `ctest` 运行：**34 个测试用例 / 269 项断言，全部通过**。覆盖范围：

| 领域 | 用例数 |
| --- | --- |
| 实体 - 世代编号、失效句柄安全、槽位回收、世代溢出 | 5 |
| 组件 - 注册、增删改查、错误路径、清除 | 6 |
| 查询 - ForEach、Where、With/Without/Optional、const 引用 | 5 |
| 命名实体 | 1 |
| 事件 - 订阅/派发、回调中退订 | 3 |
| 系统与调度器 - 依赖排序、并行、双写串行、真实环检测 | 6 |
| 组件声明 - 特性、手动特化 | 2 |
| 回归 - 销毁后遍历、空闲槽访问、遍历中改组件、世代溢出、自退订、任务异常后调度器恢复 | 6 |
| **合计** | **34 / 269** |

运行方式：

```bash
cmake -S . -B build
cmake --build build --config Release --target ekit_tests
./build/Release/ekit_tests.exe        # 或 ctest --test-dir build -C Release
```

## 项目结构

```
include/ekit/
  core.hpp        异常、TypeList、类型 ID
  entity.hpp      Entity（带世代编号的句柄）
  component.hpp   EKIT_COMPONENT、ComponentStorage（稀疏集）
  query.hpp       流畅 Query（Where / With / Without / Optional / ForEach）
  world.hpp       World、组件 CRUD、命名实体、事件
  system.hpp      系统接口 + Reads/Writes 提取
  scheduler.hpp   依赖图调度器 + 线程池
  ekit.hpp        统一入口
```

## 路线图

- [x] Entity / Component / World 核心（稀疏集存储）
- [x] 流畅 Query（`Where / With / Without / Optional`）
- [x] 系统 `Reads/Writes` + 并行 Scheduler
- [x] 事件系统（`Subscribe` / `Emit`）
- [ ] Archetype 分块（SoA）作为下一层存储
- [x] 单元测试 + 与 `entt` 对比基准
- [ ] CMake 包配置（`find_package(ekit)`）

## 许可证

[MIT](LICENSE) © 2026 chnnasn
