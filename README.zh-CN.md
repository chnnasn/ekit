# Ekit

[English](README.md) | [简体中文](README.zh-CN.md)

**Ekit** 是一个友好的、仅包含头文件的 **C++20 ECS**（实体-组件-系统）库，面向游戏引擎设计。大多数 ECS 库通常分为两类：功能强大但难以上手（深度模板元编程、晦涩的编译错误），或简单但性能较低。Ekit 希望摆脱这种取舍，以稀疏集性能提供类似 C# LINQ + Unity DOTS 的 API：明确、流畅且零开销，让你能专注于游戏逻辑，而不是基础设施。

[EnTT](https://github.com/skypjack/entt) 是一个久经考验、功能丰富的 ECS 库。Ekit 则是一个规模更小、刻意强调易用性的替代方案，专注于友好的 API 和精简的功能集，可根据项目需求选择。

## 设计理念

1. **显式优于隐式**
   - 组件必须在结构体中添加 `EKIT_COMPONENT(T)` 进行声明，并通过 `world.RegisterComponent<T>()` 显式注册，不进行隐式注册。
   - 使用未声明的组件会产生易读的 `static_assert`；使用未注册的组件会抛出清晰的 `EkitException`，并明确提示需要调用的方法。
   - 系统在类中声明自己的数据依赖（`Reads` / `Writes`）。

2. **流畅的现代 API（C# LINQ + Unity DOTS）**
   - 使用 PascalCase 方法，例如 `world.Create()`、`world.Query<Ts...>().ForEach(...)`。
   - 系统是带有 `Execute(World&)` 方法的类，风格类似 Unity DOTS。

3. **对调用方极其友好**
   - 避免模板错误刷屏：通过 `static_assert` 和 `if constexpr` 给出准确错误。
   - 实体采用强类型、带世代编号的句柄，而不是裸 `uint32_t`。
   - 零开销：流式查询链在编译期组合，无类型擦除，也没有逐实体虚函数调用。

4. **架构基石**
   - 稀疏集存储、依赖感知的并行调度器、命名实体和事件系统，为声明式自动并行、编辑器集成和网络同步提供可靠基础。

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
- **组件（Component）**：在类体内通过 `EKIT_COMPONENT(T)` 声明的 POD 结构体；使用 `world.RegisterComponent<T>()` 显式注册；采用稀疏集存储（缓存友好的密集数组及交换删除）。
- **世界（World）**：支持实体创建与销毁、组件 `Add / Emplace / Set / Get / TryGet / Has / Remove / Patch / Clear`、命名实体、批量注册和 `ClearAll`。
- **查询（Query）**：支持 `Where / With / Without / Optional / ForEach / Count` 的流式查询，并从符合条件的最小存储开始迭代：
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
  写入者会排在同一组件的所有读取者和写入者之前；两个系统同时写入同一组件属于真实冲突，会被报告为依赖环。
- **事件（Event）**：使用 `world.Subscribe<T>(handler)` / `world.Emit<T>(args...)`：
  ```cpp
  struct HitEvent { int damage; ekit::Entity target; };
  ekit::EventSubscription sub = world.Subscribe<HitEvent>(
      [](const HitEvent& ev) { /* ... */ });
  world.Emit<HitEvent>(10, target);
  sub.Unsubscribe();
  ```

## 集成

仅包含头文件，无运行时依赖：

- **CMake**
  ```cmake
  add_subdirectory(ekit)
  target_link_libraries(app PRIVATE ekit::ekit)
  ```
- **手动集成**：将 `include/` 添加到 include 路径，并使用 `#include <ekit/ekit.hpp>`。

要求 C++20（MSVC 19.29+、GCC 11+、Clang 14+）。

## 案例：Boids

`examples/boids/` 是一个基于 Ekit 构建的完整鸟群模拟示例，展示了显式组件注册、流式查询、带 `Reads/Writes` 声明的系统、并行调度器（四个鸟群规则系统并发运行）、空间哈希邻居查询，以及经典的两阶段帧流水线：

```bash
# 实时窗口（GLFW + OpenGL，GPU 渲染）。GLFW 不包含在本仓库中：
# 请将 https://github.com/chnnasn/glfw 克隆到仓库外，然后执行：
cmake -S . -B build -DEKIT_GLFW_ROOT=E:/Github/glfw
cmake --build build --config Release --target ekit_boids_live
./build/examples/boids/Release/ekit_boids_live.exe --boids 220    # 空格暂停、R 重置、ESC 退出

# 无窗口模式：PPM 帧 -> 动态 GIF
cmake --build build --config Release --target ekit_boids
./build/examples/boids/Release/ekit_boids.exe --boids 220 --frames 180
powershell -ExecutionPolicy Bypass -File examples/boids/render.ps1 -Fps 30   # 输出 boids.gif
```

详细说明请参阅 [examples/boids/README.md](examples/boids/README.md)。

## 构建与测试

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

## 项目结构

```text
include/ekit/
  core.hpp        异常、TypeList、类型 ID
  entity.hpp      Entity（带世代编号的句柄）
  component.hpp   EKIT_COMPONENT、ComponentStorage（稀疏集）
  query.hpp       流式 Query（Where / With / Without / Optional / ForEach）
  world.hpp       World、组件 CRUD、命名实体、事件
  system.hpp      系统接口及 Reads/Writes 提取
  scheduler.hpp   依赖图调度器及线程池
  ekit.hpp        统一入口
```

## 路线图

- [x] Entity / Component / World 核心（稀疏集存储）
- [x] 支持 `Where / With / Without / Optional` 的流式 Query
- [x] System `Reads/Writes` + 并行 Scheduler
- [x] 事件系统（`Subscribe` / `Emit`）
- [ ] 原型块（SoA），作为下一层存储方案
- [ ] 与 `entt` 进行单元测试和基准测试
- [ ] CMake 包配置（`find_package(ekit)`）

## 许可证

[MIT](LICENSE) © 2026 chnnasn
