# 测试与 CI（持续集成）落地指南

## 1. 测试的分层（企业常用）

- 单元测试（UT）：对“类/函数/算法”做快速验证（不依赖网络/GUI）
- 集成测试（IT）：验证模块协作（可依赖网络，但要可控）
- 手工测试（Manual）：UI 与设备联调（用 checklist 固化）

本项目已落地的 UT 模块：

- `RouteFollower`：`tests/test_routefollower.cpp`
- `ConfigManager`：`tests/test_configmanager.cpp`
- `RoutePathFinder`：`tests/test_routepathfinder.cpp`
- `StatusProtocol`：`tests/test_statusprotocol.cpp`
- `MapDocument`：`tests/test_mapdocument.cpp`（地图 JSON 编解码与校验）

本项目已补齐的 IT 模块：

- `Map` 路线执行集成验证：`tests/test_map_route_integration.cpp`
  - 验证 `Map` 路线队列启动后会按顺序派发段执行
  - 验证段完成回调会推动下一段继续执行
  - 验证当前段失败后，会基于当前位置对剩余路线做动态重规划

---

## 2. Qt 的单元测试怎么写（方向）

Qt 自带 Qt Test：

- `#include <QtTest>`
- 常用：`QCOMPARE` / `QVERIFY` / `QSignalSpy`

企业落地时一般会：

- 单测工程与主工程分离（`tests/` 目录）
- CI 中编译并执行测试，失败则禁止合并

当前仓库已包含：

- `tests/CMakeLists.txt`
- 可通过 `ctest` 直接执行

---

## 3. CI（GitHub Actions）的最低配方案

企业里 CI 最低要做到：

- 干净环境拉代码可编译（Debug/Release 至少一个）
- 跑单测（哪怕只有 1 个）
- 产出构建产物（可选）

### 3.1 工作流文件位置（已落地）

- `.github/workflows/ci.yml`（Debug/Release 矩阵构建 + 测试）

### 3.2 Windows 上 Qt 安装与构建的常见方式

- 使用 `jurplel/install-qt-action` 安装指定版本 Qt
- CMake 构建：
  - `cmake --preset default`
  - `cmake --build --preset default`
  - `ctest --preset default`

### 3.3 CI 的坑（经验）

- Qt 版本与编译器必须匹配（例如 Qt6+MinGW 的组合）
- GUI 工程在 CI 里通常只做“编译 + 运行测试”，不跑真正 UI
- 如果要打包，Windows 需 `windeployqt`（见发布文档）

---

## 4. 本地执行命令

```bash
cmake -S . -B build/cmake -G Ninja -DTENCO_BUILD_TESTS=ON
cmake --build build/cmake -j
ctest --test-dir build/cmake --output-on-failure
```

---

## 5. 推荐的演进路线（从易到难）

1. 先加 CI：只做编译（最容易）
2. 再加 1~2 个 UT：让 CI 能跑测试
3. 再加静态检查/格式化：clang-format/clang-tidy（可选）
4. 最后再做打包与签名（更贴近交付）
