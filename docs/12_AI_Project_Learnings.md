# Tenco 项目 AI 学习总结（带证据链）

## 目的

这份文档不是重复 README，也不是泛泛而谈的“项目介绍”。它只沉淀两类内容：

1. 我能从当前仓库直接验证的事实。
2. 这些事实能提炼出的、对后续 AI/开发者真正有用的工程经验。

约定：

- 事实后面尽量给出源码证据，格式为 `相对路径:行号`。
- 如果是推断，我会明确标记为【推断】。
- 我优先引用代码，而不是只引用已有文档。
- 随重构推进，部分行号可能漂移；优先按“函数名/类型名”搜索定位。

## 三行结论

- `Tenco` 是一个基于 Qt Widgets 的机器人/底盘上位机，覆盖状态监控、手动控制、MJPEG 视频、地图编辑和路线执行。证据：`README.md:3`、`main.cpp:6`。
- 这个仓库最值得学习的，不是某个单点算法，而是“把多协议设备集成到一个桌面上位机里”的拆分方式：窗口壳层、页面控制器、协议适配器、路线算法、地图编辑器各自有边界。证据：`mainwindow.cpp:63`、`home.cpp:53`、`statusclient.cpp:19`、`chassisclient.cpp:122`、`videoclient.cpp:78`、`routefollower.cpp:19`、`map.cpp:152`。
- 这个项目已经具备“可交付样板”的很多特征：CMake 构建、配置外置、版本化文档、断线重连、录像/截图、地图持久化、路径搜索与协议映射模块化；但也保留了典型样板项目的债务：`Home`/`Map` 过大、GPS 保存字段仍硬编码、暂停语义并非真正“即时暂停”。证据：`CMakeLists.txt:1`、`docs/README.md:1`、`routepathfinder.cpp:13`、`statusprotocol.cpp:7`、`home.cpp:1055`、`map.cpp:1090`、`mainwindow.cpp:69`。

## 先读什么：给 AI 的最短阅读路径

如果后续 AI 要快速接手这个仓库，推荐按下面顺序读：

1. `main.cpp:6`：应用入口，确认是 Qt Widgets + 无边框主窗体。
2. `mainwindow.cpp:21`：主窗口如何装配页面与跨页面信号。
3. `home.cpp:43`：首页控制器如何把状态、控制、视频、路线跟随组装起来。
4. `map.cpp:152`：地图模块初始化；再看 `map.cpp:2343`、`map.cpp:2855`、`map.cpp:3025`。
5. `statusclient.cpp:19` + `homenetworkworker.cpp:60`：状态轮询线程模型。
6. `chassisclient.cpp:122`：底盘 WebSocket 协议封装。
7. `videoclient.cpp:139`：MJPEG 解帧与录像/截图。
8. `routefollower.cpp:121`：路线跟随控制逻辑。
9. `configmanager.cpp:24`：配置查找策略；`config.json:1`：当前实参。

## 一张可信的架构图（文字版）

| 层 | 角色 | 已验证职责 | 关键证据 |
| --- | --- | --- | --- |
| 入口层 | `main.cpp` | 创建 `QApplication`，设置图标，显示无边框 `MainWindow` | `main.cpp:8`、`main.cpp:11` |
| 壳层窗口 | `MainWindow` | 持有导航和 `QStackedWidget`，装配 `Home/Map/...`，转发键盘事件，处理拖动与置顶 | `mainwindow.h:26`、`mainwindow.cpp:52`、`mainwindow.cpp:63`、`mainwindow.cpp:191`、`mainwindow.cpp:284` |
| 首页控制器 | `Home` | 组装状态轮询、底盘控制、视频、路线跟随、按钮/键盘控制 | `home.h:22`、`home.cpp:53`、`home.cpp:101`、`home.cpp:127`、`home.cpp:204` |
| 状态链路 | `StatusClient` + `HomeNetworkWorker` | 在独立线程里周期 POST JSON，解析 JSON，再回传 UI 线程 | `statusclient.h:14`、`statusclient.cpp:59`、`homenetworkworker.cpp:13`、`homenetworkworker.cpp:77`、`homenetworkworker.cpp:124` |
| 控制链路 | `ChassisClient` | 通过 WebSocket 发送 `cmd_vel` / reboot / stopLocation，带重连和启动握手 | `chassisclient.h:12`、`chassisclient.cpp:20`、`chassisclient.cpp:122`、`chassisclient.cpp:142`、`chassisclient.cpp:175` |
| 视频链路 | `VideoClient` | HTTP 拉 MJPEG，按 JPEG 头尾切帧，支持重连、录像、截图 | `videoclient.h:16`、`videoclient.cpp:78`、`videoclient.cpp:139`、`videoclient.cpp:221`、`videoclient.cpp:255`、`videoclient.cpp:311` |
| 地图/路线 | `Map` | 管理点、路径、路线队列、地图文件，按段调度路线 | `map.h:106`、`map.h:130`、`map.cpp:966`、`map.cpp:2343`、`map.cpp:2855`、`map.cpp:3025` |
| 跟随算法 | `RouteFollower` | 根据 polyline + 起终朝向输出平滑 `cmd_vel` | `routefollower.h:11`、`routefollower.cpp:44`、`routefollower.cpp:121`、`routefollower.cpp:205` |
| 配置层 | `ConfigManager` | 提供默认值、文件查找、参数加载、地理坐标换算 | `configmanager.h:13`、`configmanager.cpp:24`、`configmanager.cpp:77`、`configmanager.cpp:184` |

## 学到的核心经验

### 1. Qt Widgets 不一定要“每页一个 QWidget”，也可以用“壳层窗口 + QObject 页面控制器”

这是这个仓库最有辨识度的设计点。

- `Home` 和 `Map` 都继承自 `QObject`，不是 `QWidget`。证据：`home.h:22`、`map.h:45`。
- 它们共享同一个 `Ui::MainWindow *ui` 来操作页面中的具体控件。证据：`home.h:73`、`map.h:207`。
- `MainWindow` 统一创建这些控制器，并把跨页面信号在主窗体层连起来。证据：`mainwindow.cpp:63`、`mainwindow.cpp:69`。

可以学到什么：

- 这种做法适合“单窗体、多业务面板、UI 布局基本固定”的桌面程序。
- 它比“每页一个大 QWidget 子类”更像 MVC 中的 controller：UI 结构保留在 `.ui` 文件里，行为逻辑下沉到 `QObject`。
- 代价也很明确：控制器会直接依赖具体控件名，长期演进后容易形成“大对象 + 大量 UI 细节逻辑”。【推断】这一点在 `Home` 和 `Map` 里已经有迹象。依据：`home.h:75` 到 `home.h:138`、`map.h:138` 到 `map.h:321`。

### 2. 跨页面协作不是直接互调，而是通过信号把“业务闭环”串起来

`MainWindow` 里最关键的不是切页，而是这几条连接：

- `Home::vehiclePoseUpdated -> Map::updateVehiclePose`，把实时位姿同步到地图。证据：`mainwindow.cpp:70`。
- `Map::routeSegmentDispatched -> Home::followRouteSegment`，由地图发段、首页执行。证据：`mainwindow.cpp:71`。
- `Home::routeSegmentCompleted -> Map::handleRouteSegmentCompleted`，执行结果再回到地图继续推进队列。证据：`mainwindow.cpp:74`。

这说明项目的作者没有把地图直接写成“控制器”，而是把地图定义成“路线段调度器”，把真正的运动控制留给 `Home + RouteFollower + ChassisClient`。

这是一种很值得复用的边界划分：

- 地图负责“该走哪条段”；
- 跟随器负责“这条段怎么走”；
- 底盘客户端负责“把命令发出去”。

### 3. 周期状态轮询用了一个非常实用的 Qt 模式：`QObject worker + QThread + pending 合并`

状态轮询没有直接在 UI 线程里用 `QTimer + QNetworkAccessManager`，而是拆成两层：

- `StatusClient` 负责生命周期、线程创建、暴露信号。证据：`statusclient.cpp:19`、`statusclient.cpp:59`。
- `HomeNetworkWorker` 在独立线程里真正发 HTTP 请求。证据：`homenetworkworker.cpp:13`、`homenetworkworker.cpp:28`、`homenetworkworker.cpp:77`。

更重要的是，它没有让请求无限重叠：

- 如果上一个请求还没结束，`triggerFetch()` 只把 `m_fetchPending` 设为 `true`，不立刻发第二个。证据：`homenetworkworker.cpp:70`、`homenetworkworker.cpp:71`。
- 当前请求结束后，如果 pending 为真，再补发一次。证据：`homenetworkworker.cpp:101`、`homenetworkworker.cpp:103`。

这是一个小而硬的经验：

- 对高频轮询，不要简单“定时器一到就新开请求”；
- 更稳的策略是“同一时间只允许一个请求在途，超期期间只记一个 pending”。

这能避免：

- 请求堆积；
- UI 因结果乱序而闪烁；
- 设备端被压垮。

### 4. 协议封装做得比较干净：UI 不直接拼 WebSocket/MJPEG 协议细节

首页虽然很大，但协议细节没有完全散落在 UI 代码里。

#### 4.1 底盘控制协议

- 发送速度命令时，`Home` 只调用 `sendVelocityCommand()`，真正 JSON 包格式在 `ChassisClient` 内部。证据：`home.cpp:452`、`chassisclient.cpp:122`。
- `cmd_vel` 数据包结构是 `{packet:{cmd:"region",region:"cmd_vel",index:1}, msg:{xvel,yvel,thetavel,isRemote}}`。证据：`chassisclient.cpp:124`、`chassisclient.cpp:125`。

#### 4.2 视频协议

- `Home` 只负责选 topic、切 URL、显示画面；真正拉流与拆帧在 `VideoClient` 里。证据：`home.cpp:329`、`videoclient.cpp:32`、`videoclient.cpp:139`。

#### 4.3 状态协议

- `Home` 负责声明需要轮询哪些地址；真正 POST 和 JSON 解析在 worker 里。证据：`home.cpp:102`、`homenetworkworker.cpp:82`、`homenetworkworker.cpp:117`。

这里能学到一个原则：

- “字段语义”可以在页面层；
- “传输协议格式”最好收敛到 client/worker 类里。

这样换协议时，不会把 UI 页面一起污染掉。

### 5. 这个项目非常重视“操作安全感”，不是只会发命令

几个细节很值得学习：

#### 5.1 手动控制有显式授权开关

- `imageSwitch1` 控制按钮操作授权；`imageSwitch2` 控制键盘授权。证据：`home.cpp:175`、`home.cpp:183`、`home.cpp:374`、`home.cpp:379`。
- 键盘事件还显式忽略 auto-repeat，只认真实按下/释放。证据：`home.cpp:839`、`home.cpp:878`。

这不是“炫 UI”，而是把误触风险前置。

#### 5.2 长按控制不是靠系统按键重复，而是自己用定时器稳定连发

- 四个方向都有独立 `QTimer`，统一周期 40ms。证据：`home.h:71`、`home.cpp:187`。
- 按下时先立即发一次，再启动重复发送；释放时停表。证据：`home.cpp:839`、`home.cpp:878`。

这意味着控制节奏由程序掌握，不受系统键盘重复率影响。

#### 5.3 路线开始前会校验“车当前在哪个点”

- 地图模块会把实时位姿吸附到最近地图点，形成 `m_vehicleCurrentPointId`。证据：`map.cpp:2816`。
- 启动路线前，要求当前点必须等于首段起点。证据：`map.cpp:1069`、`map.cpp:1070`。

这个约束很朴素，但非常重要：它把“路线计划”和“现场状态”对齐了。

### 6. 地图模块的本质不是“画图”，而是“图结构 + 几何 + 执行调度”三合一

从数据结构看，`Map` 不是只管理 `QGraphicsItem`：

- `MapPoint` 保存逻辑点位、朝向和图元。证据：`map.h:106`。
- `MapPath` 保存起终点、路径类型、polyline、圆弧参数和图元。证据：`map.h:116`。
- `RouteStep` 保存路线段、路径 ID 列表、点序列和进度。证据：`map.h:130`。
- 逻辑图的邻接关系单独保存在 `m_outgoingPathIds`。证据：`map.h:230`。

这意味着作者在有意识地区分：

- 逻辑模型（点、边、路线）；
- 几何模型（polyline、圆弧中心、半径）；
- 视图模型（`QGraphicsItem`）。

这是一种很适合继续工程化的结构。

### 7. 路线搜索已升级为“加权最短路”（Dijkstra）

`findRoutePathIds()` 现在会先将地图路径转换为带权图，再调用 `RoutePathFinder` 计算最短代价路径。证据：`map.cpp:2343`、`routepathfinder.cpp:12`。

当前代价模型：

- 基础代价：路径几何长度（米）；
- 固定代价：每条边常量惩罚（抑制无意义绕行）；
- 圆弧代价：弧线附加惩罚（在同距离下偏向直线）。

这让路线选择从“最少边数”提升为“更接近真实行驶成本”。

### 8. 路径几何支持“直线 + 圆弧”，而且圆弧是通过 sagitta（弓高）参数化的

- 直线路径直接由起点和终点组成 polyline。证据：`map.cpp:2003`、`map.cpp:2016`。
- 弧线路径使用 `sagitta`，并在 `refreshPathGeometry()` 里反推圆心、半径、扫角，再离散为多段 polyline。证据：`map.cpp:2022`、`map.cpp:2161`、`map.cpp:2185`、`map.cpp:2196`。

这说明项目选了一种对业务人员比较友好的建模方式：

- 用户不需要直接输入圆心或半径；
- 只要给起点、终点和“弯曲程度”，系统自己生成弧线。

这对 AGV/小车地图编辑很实用。

### 9. 路线执行链路设计得很“松耦合”：Map 发段，Home 跟段

当前路线执行不是把整条路线一次性下发给底盘，而是按段闭环：

1. `Map` 从 `RouteStep.pathIds` 生成一段 polyline。证据：`map.cpp:2884`。
2. `Map` 发出 `routeSegmentDispatched(...)`。证据：`map.cpp:2904`。
3. `Home` 收到后把这段送进 `RouteFollower`。证据：`home.cpp:1103`。
4. `RouteFollower` 周期输出 `velocityCommand`。证据：`home.cpp:96`、`routefollower.cpp:205`。
5. 段执行结束后，`Home` 发 `routeSegmentCompleted(bool)` 回给 `Map`。证据：`home.h:35`、`mainwindow.cpp:74`。
6. `Map` 再决定发下一段或结束。证据：`map.cpp:270`、`map.cpp:305`、`map.cpp:308`。

这套设计的好处：

- 地图逻辑不用知道运动控制细节；
- 运动控制也不用关心整个路线队列；
- 失败可以停在“段边界”，更易于恢复和定位问题。

### 10. `RouteFollower` 是这个仓库里最值得复用的“纯逻辑模块”

它最像一个可抽出来单测的模块，因为它基本不依赖 UI。

#### 已验证控制流程

- 收到一段 polyline 后入队。证据：`routefollower.cpp:44`。
- 用 100ms 定时器驱动。证据：`routefollower.cpp:13`、`routefollower.cpp:24`。
- 如果当前 pose 无效，就发 0 速。证据：`routefollower.cpp:128`、`routefollower.cpp:129`。
- 段起点先对齐 `startTheta`，不对齐就只转向不前进。证据：`routefollower.cpp:143`、`routefollower.cpp:151`。
- 中途按 waypoint 逐点推进。证据：`routefollower.cpp:159`、`routefollower.cpp:177`。
- 到终点后再对齐 `endTheta`，满足距离与角度阈值才算真正完成。证据：`routefollower.cpp:163`、`routefollower.cpp:166`、`routefollower.cpp:167`。
- 输出速度前使用加减速度限制（slew rate）。证据：`routefollower.cpp:205`、`routefollower.cpp:219`。

#### 这个算法为什么值得学

- 它不是“黑盒控制器”，逻辑完全可读、可调。
- 参数全部外置到 `config.json`。证据：`home.cpp:73`、`config.json:6`。
- 起点对齐、终点对齐、朝向误差降速、近目标降速、加减速度限制这几个工程技巧齐了，已经超过很多 demo 级 pure-pursuit 代码。

#### 这个算法当前的边界

- 它按 polyline waypoint 跟随，不是 MPC，也不是全局规划器。
- 它依赖外部持续喂 pose。证据：`home.cpp:611`、`routefollower.cpp:31`。

### 11. 视频处理是“实用主义实现”：不严格解析 MIME boundary，而是直接找 JPEG 头尾

`VideoClient` 的拆帧逻辑很典型：

- 把 `readyRead()` 里的字节流持续追加到 buffer。证据：`videoclient.cpp:145`。
- 通过 `0xFFD8` / `0xFFD9` 查找 JPEG 起止。证据：`videoclient.cpp:150`、`videoclient.cpp:164`。
- 找到一帧就 `QImage::loadFromData()`，再发 `frameReceived`。证据：`videoclient.cpp:173`、`videoclient.cpp:183`。

这背后的经验是：

- 对可控设备环境，常常不需要把 MJPEG 协议栈做得很“重”；
- 如果输入流稳定，按 JPEG marker 拆帧就能解决 80% 问题。

但它也意味着一个明确取舍：

- 如果未来接更复杂的流，可能要补严格的 multipart boundary 解析。【推断】依据：当前实现没有处理 boundary 文本，只处理 JPEG marker。证据：`videoclient.cpp:150`。

### 12. 配置层做得比很多 demo 稳：默认值、路径搜索、运行目录兼容、算法参数外置

#### 12.1 配置文件查找策略

`ConfigManager` 会按多级路径查找 `config.json`：

- 可执行文件同级；
- 当前工作目录；
- 可执行文件上级；
- 可执行文件上上级。证据：`configmanager.cpp:24` 到 `configmanager.cpp:47`。

这对 Qt Creator、命令行、不同输出目录都比较友好。

#### 12.2 构建系统也配合了这件事

- CMake 在 post-build 时把 `config.json` 复制到可执行文件目录。证据：`CMakeLists.txt:103`、`CMakeLists.txt:105`。

#### 12.3 地理坐标换算采用本地近似模型

- `geoToLocal()` / `localToGeo()` 使用基准纬经度和每度米数换算。证据：`configmanager.cpp:174`、`configmanager.cpp:184`、`configmanager.cpp:193`。

这说明地图坐标不是全球投影，而是围绕 `baseLatitudeDeg/baseLongitudeDeg` 的局部平面近似。

这很适合园区/工厂场景，但不适合大尺度跨区域导航。【推断】依据：只用了每度米数近似，没有更复杂投影。证据：`configmanager.cpp:176`、`configmanager.cpp:188`。

### 13. UI 实现采用“Designer 负责壳，代码负责复杂面板”的混合策略

- 主窗体和通用页面框架来自 `mainwindow.ui`，由 `ui->setupUi(this)` 装载。证据：`mainwindow.cpp:26`、`mainwindow.ui:1`。
- 地图面板的大量控件和交互是代码里动态创建/连接的。证据：`map.cpp:1249`、`map.cpp:1648`。
- `MapGraphicsView` 进一步把缩放、平移、点击场景坐标标准化出来。证据：`mapgraphicsview.h:20`、`mapgraphicsview.cpp:37`、`mapgraphicsview.cpp:62`。

这是一个很实用的经验：

- 固定布局适合 `.ui`；
- 复杂画布、动态工具栏、强交互画面更适合代码构建。

### 14. 这个仓库把“文档跟代码一起版本化”当成正式工程实践

- `docs/README.md` 明确说明这些文档是“随代码版本化”的开发文档。证据：`docs/README.md:1`、`docs/README.md:3`。
- 文档目录并不是一个 README，而是覆盖流程、架构、模块、协议、测试、发布、路线图的一套文档。证据：`docs/README.md:7`。
- README 也显式把更详细资料指向 `docs/README.md`。证据：`README.md:92`、`README.md:96`。

这件事本身就是可学习的工程经验：

- 对设备集成型项目，代码不够，协议、配置、联调方式必须一起版本化。

## 需要特别谨慎理解的地方

### 1. “暂停路线”当前不是“立即停车暂停”，而更像“下一段前暂停”

这是一个非常关键的细节。

- `Map` 暂停时只是设置 `m_pauseRequested = true` 并发出 `routeExecutionPauseRequested()`。证据：`map.cpp:1096`、`map.cpp:1101`。
- 但 `MainWindow` 只连接了 `routeSegmentDispatched`、`routeQueueCompletedOnce`、`routeExecutionCancelled`、`routeSegmentCompleted`，没有连接 pause/resume 信号。证据：`mainwindow.cpp:69`。
- `dispatchNextEdge()` 在派发下一段前检查 `m_pauseRequested`，因此暂停会阻止“下一段派发”，而不是中断“当前正在跑的这一段”。证据：`map.cpp:2864`、`map.cpp:2904`。
- 段完成回调里也会因为 `m_pauseRequested` 先停住，不再自动发下一段。证据：`map.cpp:300`。

结论：

- 当前“暂停”的真实语义是“段边界暂停”，不是即时运动学暂停。

### 2. WebSocket 断线时，速度命令不会排队补发

- `sendJson()` 在 socket 未连接时只会尝试 `openIfPossible()`，然后直接返回。证据：`chassisclient.cpp:113`、`chassisclient.cpp:114`。

结论：

- 如果某一帧控制命令发出时连接正好断了，这个命令就丢了；
- 这是合理的保守行为，但如果以后要做“可靠命令队列”，修改点就在这里。

### 3. GPS 配置仍有明显硬编码（状态字段已模块化）

- 状态轮询地址与字段映射已抽离到 `statusprotocol.*`。证据：`statusprotocol.cpp:7`、`statusprotocol.cpp:40`。
- GPS 配置保存请求里的 JSON 字段和值仍有较多硬编码。证据：`home.cpp:1055`、`home.cpp:1079`。

结论：

- 这套代码非常适合“当前设备协议固定”的场景；
- 如果未来设备型号增多，下一步应该抽协议 schema，而不是继续在 `Home` 里堆字段。

### 4. `Home` 和 `Map` 都很强，但也都偏大

- `Home` 同时负责 UI 交互、协议组装、日志、录像、截图、重启、模式切换、GPS 保存、路线跟随接线。证据：`home.h:42`、`home.cpp:148`、`home.cpp:913`、`home.cpp:964`、`home.cpp:1103`。
- `Map` 同时负责 UI 构建、图结构、几何计算、文件读写、执行调度、鼠标交互。证据：`map.cpp:1249`、`map.cpp:1874`、`map.cpp:2343`、`map.cpp:2855`、`map.cpp:3025`。

【推断】这说明项目当前阶段优先级更偏“先做成、再逐步工程化”，而不是一开始就细分成很多小类。

## 如果另一个 AI 接手这个仓库，修改入口应该这样找

### 改状态字段 / UI 映射

- 先看 `statusprotocol.cpp:7`：轮询请求地址声明。
- 再看 `statusprotocol.cpp:40` + `home.cpp:591`：地址如何映射到字段、字段如何映射到 UI。

### 改底盘命令协议

- 先看 `chassisclient.cpp:122`：速度命令格式。
- 再看 `chassisclient.cpp:129`、`chassisclient.cpp:135`：重启/停止定位。
- 如果涉及连通性，补看 `chassisclient.cpp:88`、`chassisclient.cpp:175`。

### 改视频流来源或录像逻辑

- 先看 `home.cpp:329`：topic 切换到 URL 的入口。
- 再看 `videoclient.cpp:32`：如何基于 topic 生成 URL。
- 再看 `videoclient.cpp:139`、`videoclient.cpp:255`、`videoclient.cpp:311`：解帧/录像/截图。

### 改路线算法

- 先看 `home.cpp:73`：参数如何从配置注入。
- 再看 `routefollower.cpp:121`：核心控制。
- 再看 `routefollower.cpp:205`：速度平滑限制。

### 改地图路径搜索或路线调度

- 先看 `map.cpp:2343`：路径搜索。
- 再看 `map.cpp:2401`：path polyline 合并。
- 再看 `map.cpp:2855`：按段派发执行。
- 再看 `map.cpp:3025`：地图文件格式。

### 改配置 schema

- 先看 `configmanager.h:13`：配置结构定义。
- 再看 `configmanager.cpp:89`：JSON 读取。
- 再看 `config.json:1`：当前仓库示例配置。

## 最后浓缩成 8 条可迁移经验

1. 设备集成型桌面程序，最怕协议细节散进 UI；应尽早收敛到 client/worker 类。证据：`chassisclient.h:12`、`videoclient.h:16`、`statusclient.h:14`。
2. 高频轮询要控制并发，`pending 合并` 比“请求排队”更适合状态采集。证据：`homenetworkworker.cpp:70`、`homenetworkworker.cpp:101`。
3. 复杂业务 UI 可以用 `QObject controller + shared ui`，但要接受后期再拆分的成本。证据：`home.h:22`、`map.h:45`、`mainwindow.cpp:63`。
4. 路线执行最好分成“地图调度”和“运动跟随”两层，而不是混成一个类。证据：`mainwindow.cpp:71`、`home.cpp:1103`、`routefollower.cpp:44`。
5. 地图编辑本质上是“逻辑图 + 几何 + 视图”三层，不要只盯着 `QGraphicsScene`。证据：`map.h:106`、`map.h:116`、`map.cpp:2043`、`map.cpp:2234`。
6. 控制类应用要把“授权、前置校验、状态反馈”当主功能，而不是附属功能。证据：`home.cpp:175`、`home.cpp:657`、`map.cpp:1064`。
7. 参数外置比硬编码算法常数更重要，尤其是控制、网络、视频、地理基准。证据：`configmanager.h:18`、`home.cpp:73`、`config.json:6`。
8. 文档跟代码一起版本化，是设备项目里最划算的工程投入之一。证据：`docs/README.md:1`。

## 一句话收尾

如果只用一句话概括这个仓库：它不是“炫技型 Qt 工程”，而是一份非常适合学习“机器人上位机如何做成一个可维护样板”的仓库；真正值得学的是它的模块边界、数据流、故障处理和可演进空间，而不只是界面本身。
