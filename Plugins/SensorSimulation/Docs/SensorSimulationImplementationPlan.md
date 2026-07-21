# UE5 自动驾驶/机器人传感器仿真系统实现文档

## 1. 文档目的

本文档定义一个由两名开发者在一个月内完成的 Unreal Engine 5 传感器仿真 MVP，作为框架设计、模块分工、接口联调、进度跟踪和最终验收的共同依据。

项目不以替代 CARLA/AirSim 为目标，而是实现一个可复用的 UE5 插件框架，同步生成 RGB、语义分割、简化多线 LiDAR 和 Ground Truth 数据，供外部感知算法离线使用。

## 2. 最终目标与需求边界

### 2.1 最终交付目标

最终系统应能在同一仿真场景中同步输出：

1. RGB 相机图像。
2. Semantic Segmentation 图像。
3. 16/32 线简化 LiDAR 点云。
4. 自车位姿、速度和加速度。
5. 周围目标的语义、实例 ID、位姿、速度和 3D 包围盒。
6. 相机及 LiDAR 的内参、外参和坐标系说明。
7. 全部数据共享统一的 `SequenceId`、`FrameId` 和仿真时间戳。
8. GPU Readback 和文件写入不造成持续性 Game Thread 阻塞。
9. 输出数据可由外部 Python/Open3D/OpenCV 工具读取和校验。

### 2.2 MVP 必须实现

- UE5 C++ 插件化，不修改 Engine 源码。
- 1 路 RGB Camera，最低支持 1280×720。
- 1 路 Semantic Camera，至少支持 8 个语义类别。
- Semantic 图像只能出现预定义标签值，不允许 TAA、Tonemapping 或压缩产生颜色污染。
- RGB/Semantic 使用异步 GPU Readback。
- LiDAR 支持可配置线数、水平采样数、量程和扫描频率。
- LiDAR 射线分批或异步执行，不能在单 Tick 串行执行完整高密度扫描。
- 支持 `.bin` 点云输出；`.pcd` 为推荐项。
- Ground Truth 使用 CSV/JSON 输出。
- 有界异步 I/O 队列以及队列积压统计。
- 固定时间步数据集模式。
- 一套汽车或机器人 Demo 场景。

### 2.3 可选增强项

- Depth 图像输出。
- Instance Segmentation。
- 自动生成 2D Bounding Box 和遮挡率。
- LiDAR 分块旋转扫描与基础运动畸变。
- PNG 与无损整数标签格式。
- ROS2 或 Socket 输出适配器。
- 天气、曝光和传感器噪声配置。

### 2.4 本期不实现

- 毫米波雷达物理模型。
- GPU Ray Tracing LiDAR。
- 完整透明物体语义渲染。
- 自定义 Nanite Mesh Pass。
- 多机环视、鱼眼和镜头畸变标定。
- 大规模交通系统。
- KITTI、nuScenes 全字段兼容。
- 实时 Sensor Fusion 或 AI 推理。

## 3. 总体框架

### 3.1 数据流

```text
World / Scenario
      |
      v
USensorSimulationSubsystem
      |-- Sensor Clock / Frame Scheduler
      |-- Semantic Registry
      |-- Ground Truth Collector
      `-- Capture Coordinator
               |
       +-------+----------------+
       |                        |
       v                        v
Camera Sensors             LiDAR Sensor
RGB/Semantic Capture       Async/Batch Trace
       |                        |
       v                        v
GPU Readback Queue         Point Cloud Builder
       |                        |
       +-----------+------------+
                   v
           FSensorFramePacket
                   |
                   v
          Bounded Export Queue
                   |
       +-----------+------------+
       |           |            |
       v           v            v
   PNG Writer   BIN/PCD      CSV/JSON
```

### 3.2 推荐插件结构

```text
Plugins/SensorSimulation/
|-- SensorSimulation.uplugin
|-- Config/
|-- Resources/
|-- Shaders/
|   `-- Private/
|       |-- SemanticCapture.usf
|       `-- SensorCopy.usf
`-- Source/
    |-- SensorSimulationCore/
    |   |-- Public/
    |   `-- Private/
    |-- SensorSimulationRenderer/
    |   |-- Public/
    |   `-- Private/
    |-- SensorSimulationRuntime/
    |   |-- Public/
    |   `-- Private/
    `-- SensorSimulationEditor/
        |-- Public/
        `-- Private/
```

### 3.3 模块依赖方向

```text
SensorSimulationEditor
          |
          v
SensorSimulationRuntime ---> SensorSimulationRenderer
          |                           |
          +------------+--------------+
                       v
             SensorSimulationCore
```

约束：

- `Core` 不依赖 UObject、Renderer 或 Editor，主要保存稳定的数据协议。
- `Renderer` 不依赖具体业务 Actor，只消费纯渲染数据。
- `Runtime` 可以依赖 `Core` 和 `Renderer`，负责 World、Actor 和传感器调度。
- `Editor` 只能包含编辑器工具，不允许 Runtime 模块引用它。

## 4. 模块化设计

## 4.1 SensorSimulationCore

### 职责

- 定义 Frame、Sensor、Calibration、Ground Truth 和 Export 数据结构。
- 定义语义类别和 Instance ID。
- 处理 UE 坐标系到目标数据坐标系的统一转换。
- 提供序列化接口和数据版本号。

### 核心数据结构

```cpp
struct FSensorFrameHeader
{
    uint64 SequenceId = 0;
    uint64 FrameId = 0;
    double SimulationTimestampSeconds = 0.0;
    FTransform EgoWorldTransform;
};

struct FSensorCalibration
{
    FString SensorName;
    FTransform SensorToEgo;
    FIntPoint ImageSize = FIntPoint::ZeroValue;
    double Fx = 0.0;
    double Fy = 0.0;
    double Cx = 0.0;
    double Cy = 0.0;
};

struct FLidarPoint
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Intensity = 0.0f;
    uint16 SemanticId = 0;
    uint32 InstanceId = 0;
    float RelativeTime = 0.0f;
};

struct FObjectGroundTruth
{
    uint32 InstanceId = 0;
    uint16 SemanticId = 0;
    FTransform WorldTransform;
    FVector3d LinearVelocity = FVector3d::ZeroVector;
    FVector3d AngularVelocity = FVector3d::ZeroVector;
    FBox3d WorldBounds;
};
```

### 数据版本

每个 Session 的 `metadata.json` 必须记录：

- Dataset schema version。
- UE 版本。
- 插件版本和 Git commit。
- 坐标系定义。
- 单位定义。
- Scenario/Random Seed。
- Sensor 配置哈希。

## 4.2 SensorSimulationRenderer

### 职责

- 管理 RGB 和 Semantic Scene Capture。
- 管理 Render Target 和 Shader。
- 处理 Custom Depth/Stencil 语义映射。
- 提交异步 GPU Texture Readback。
- 将 Readback 结果转换为 CPU Image Payload。
- 记录 GPU Capture 延迟和积压数量。

### 主要类

```text
USimCameraSensorComponent
FSensorCaptureProxy
FSemanticCaptureRenderer
FSensorImageReadbackManager
FSensorImageReadbackRequest
```

### RGB Capture

RGB Camera 支持：

- Resolution。
- Horizontal FOV。
- Capture Frequency。
- Sensor-to-Ego Transform。
- Exposure/Profile 配置。
- 输出相机 Intrinsic 和 Projection Matrix。

RGB 允许保留合理的曝光和色调映射，但所有配置必须写入 Session Metadata。

### Semantic Capture

MVP 使用 Custom Depth/Stencil：

```text
0 Background
1 Road
2 Vehicle
3 Pedestrian
4 Cyclist
5 Building
6 Vegetation
7 Obstacle
8 EgoVehicle
```

Semantic Capture 必须关闭：

- TAA/TSR。
- Motion Blur。
- Auto Exposure。
- Bloom。
- Film Grain。
- Vignette。
- Tonemapping 或其他会修改标签值的 Pass。

语义输出应采用整数 ID 或严格的无损 LUT，测试工具必须验证所有像素都属于合法集合。

### 异步 GPU Readback

禁止在正式采集路径每帧使用同步 `ReadPixels()`。推荐流程：

```text
SceneCapture Render Target
        -> Copy/Conversion Pass
        -> FRHIGPUTextureReadback
        -> IsReady in later frame
        -> Copy to owned CPU buffer
        -> Export queue
```

Readback Request 必须包含：

- Frame Header。
- Sensor Name。
- Image Type。
- Resolution。
- Pixel Format。
- Submit Frame 和 Submit Time。

Readback Manager 使用 3～6 个槽位的 Ring Buffer，不能无限创建未完成请求。

## 4.3 SensorSimulationRuntime

### 职责

- World Subsystem 和 Sensor Clock。
- Sensor 注册、启停和频率调度。
- Semantic/Instance Registry。
- LiDAR Trace 和 Point Cloud 组装。
- Ground Truth Snapshot。
- Frame Packet 聚合。
- Export Queue、Worker 和背压控制。
- Dataset Session 生命周期。

### 主要类

```text
USensorSimulationSubsystem
USimSensorComponentBase
USimCameraSensorComponent
USimLidarSensorComponent
USemanticObjectComponent
FSemanticObjectRegistry
FGroundTruthCollector
FSensorFrameAssembler
FSensorExportService
```

### Sensor Clock

支持两种模式：

1. `Realtime`：仿真实时运行，队列满时按照策略丢帧。
2. `DeterministicDataset`：使用固定时间步；当前 Frame Packet 完成后才推进数据集采样。

传感器不能各自在 `TickComponent()` 中独立决定 Frame ID。所有 Capture Request 必须由 Subsystem 统一创建。

### LiDAR

MVP 配置：

```cpp
struct FLidarConfig
{
    int32 Channels = 16;
    int32 HorizontalSamples = 512;
    float ScanFrequencyHz = 10.0f;
    float MinRangeMeters = 0.5f;
    float MaxRangeMeters = 100.0f;
    TArray<float> VerticalAnglesDegrees;
    int32 RaysPerBatch = 1024;
};
```

实现约束：

- Local Ray Direction 在配置变更时预计算。
- 射线按 Batch 执行。
- 优先使用异步 Trace；若 UE API 限制，则使用多帧分批同步 Trace 并明确记录限制。
- 使用专用 Collision Channel，排除无关对象。
- 结果数组预分配，避免逐 Ray 堆分配。
- 一圈完成后统一转换到 Sensor 坐标系并输出。
- `.bin` 默认字段为 `float32 x, y, z, intensity`。

### Ground Truth

Ground Truth Snapshot 必须在 Capture Request 创建时生成，而不是在图片 Readback 完成时生成。

记录内容：

- Ego Transform、Velocity、Angular Velocity、Acceleration。
- Sensor-to-Ego 和 Sensor-to-World Transform。
- Object Instance ID、Semantic ID、Transform、Velocity、3D Bounds。
- Frame ID 和 Simulation Timestamp。

### Export Queue

Export Service 必须使用有界队列：

```text
Default capacity: 8 complete frames
Realtime: DropOldest or DropNewest
DeterministicDataset: Block simulation sampling
```

I/O Worker 负责：

- PNG 或无损图像编码。
- `.bin/.pcd` 写入。
- CSV/JSON 写入。
- 原子化完成标记。

Render Thread 和 Game Thread 不执行 PNG 压缩。

## 4.4 SensorSimulationEditor

本模块为低优先级，可在 MVP 后半段实现：

- Sensor Config Data Asset 编辑。
- Semantic Category 表格。
- Actor 批量添加 Semantic Component。
- Camera/LiDAR Frustum 可视化。
- 采集状态面板。
- 一键开始/停止 Dataset Session。

若时间不足，使用 Blueprint、Developer Settings 和 Console Commands 代替。

## 5. Frame 同步协议

### 5.1 Frame 状态机

```text
Requested
  -> GroundTruthCaptured
  -> CameraSubmitted
  -> LidarSubmitted
  -> CameraReadbackReady
  -> LidarReady
  -> Assembled
  -> ExportQueued
  -> Exported
```

### 5.2 Frame Packet

```cpp
struct FSensorFramePacket
{
    FSensorFrameHeader Header;
    FEgoGroundTruth Ego;
    TArray<FObjectGroundTruth> Objects;
    TArray<FImagePayload> Images;
    TArray<FLidarPoint> PointCloud;
    uint32 ExpectedPayloadMask = 0;
    uint32 CompletedPayloadMask = 0;
};
```

只有 `CompletedPayloadMask == ExpectedPayloadMask` 时 Frame 才能进入完整导出流程。超时或失败必须记录，不允许静默生成缺少模态的数据帧。

### 5.3 时间语义

- `SimulationTimestamp` 表示 Capture Request 的仿真时间。
- RGB、Semantic 和 Ground Truth 使用同一时间语义。
- MVP LiDAR 可先采用整圈单时间戳。
- 实现 Rolling Scan 后，每个 LiDAR Point 使用 `RelativeTime` 表示相对扫描时间。

## 6. 坐标系和单位约定

UE 内部：

- 左手坐标系。
- X Forward、Y Right、Z Up。
- 长度单位 cm。

Dataset 默认输出：

- 右手车辆坐标系。
- X Forward、Y Left、Z Up。
- 长度单位 m。

默认位置转换：

```text
x_out =  x_ue / 100
y_out = -y_ue / 100
z_out =  z_ue / 100
```

旋转和四元数必须通过统一的 `FSensorCoordinateConverter` 转换，禁止各 Exporter 自行实现坐标变换。

相机坐标系如采用 OpenCV 约定，必须额外声明 X Right、Y Down、Z Forward 的变换矩阵。

## 7. 两人分工

## 7.1 Render Pipeline Owner

负责：

- `SensorSimulationRenderer` 模块。
- Camera Sensor 的渲染部分。
- RGB、Semantic 和可选 Depth Capture。
- Shader、RDG、Render Target。
- Custom Stencil 语义映射。
- GPU Readback Ring。
- Camera Intrinsic/Projection 数据。
- Semantic 边界和标签值验证。
- RenderDoc、GPU Profile 和渲染兼容性。

主要创新任务：

1. 无颜色污染的 Semantic 输出。
2. RGB/Semantic/Depth 同帧异步 Readback。
3. Class ID 与可选 Instance ID 双输出。
4. 基于 Instance Mask 自动统计 2D Bounding Box 和可见像素数。

交付接口：

```cpp
void SubmitCameraCapture(const FSensorCaptureRequest& Request);
bool PollCompletedImage(FCompletedSensorImage& OutImage);
```

## 7.2 Runtime/System Owner

负责：

- `SensorSimulationCore` 和 `SensorSimulationRuntime`。
- Sensor Subsystem、Clock 和调度。
- Semantic/Instance Registry。
- LiDAR 射线生成、Trace、点云组装。
- Ground Truth Snapshot。
- Frame Assembler。
- Export Service 和数据格式。
- Dataset Session、配置和 Debug 状态。

主要创新任务：

1. 跨传感器确定性 Frame 同步。
2. LiDAR 多帧分块扫描和 Relative Time。
3. 固定 Seed 的可复现数据集。
4. I/O 背压、丢帧和延迟可视化。

交付接口：

```cpp
FSensorCaptureRequest CreateCaptureRequest();
void SubmitImagePayload(FCompletedSensorImage&& Image);
void SubmitLidarPayload(FCompletedLidarScan&& Scan);
void TryAssembleAndExport(uint64 FrameId);
```

## 7.3 共同责任

- 冻结公共数据结构。
- 冻结坐标系和语义表。
- 维护 Demo 场景。
- 每周进行一次集成测试。
- 对 Frame 同步和数据正确性共同签字验收。
- 不允许任何一方单独修改公共接口后直接提交。

## 8. 工作约定

### 8.1 Git 约定

- `main` 始终保持可编译。
- 功能分支命名：`feature/render-*`、`feature/runtime-*`、`fix/*`。
- 每个 PR 聚焦一个模块或问题。
- 合并前至少由另一名开发者 Review。
- 不提交 `Binaries/`、`Intermediate/`、`Saved/` 和生成数据集。
- Shader 和公共结构变更必须在 PR 描述中说明兼容影响。

### 8.2 接口变更约定

修改 `Core/Public` 中的结构或枚举前：

1. 在 Issue/任务中描述变更原因。
2. 写出旧接口和新接口。
3. 列出受影响模块。
4. 双方确认后再修改。
5. 同一 PR 更新序列化、测试和文档。

### 8.3 线程约定

- UObject 和 World 查询默认只在 Game Thread 使用。
- Render Thread 不持有生命周期不明确的 UObject 指针。
- 通过纯数据快照跨线程传递。
- Readback 完成后先复制到自有 CPU Buffer，再交给 I/O Worker。
- I/O Worker 不调用依赖 Game Thread 的 UE API。
- 共享队列必须明确所有者、锁策略和最大容量。

### 8.4 错误处理

禁止静默失败。每个失败 Frame 至少记录：

- Frame ID。
- Sensor Name。
- Failure Stage。
- Error Code/Message。
- Queue Length。
- Capture/Readback/Export Latency。

### 8.5 Definition of Done

一个任务只有满足以下条件才算完成：

- Development Editor 编译通过。
- 功能在 Demo Map 可运行。
- 无新增持续性错误日志。
- 有最小验证方法或测试。
- 公共行为更新到本文档或 README。
- 另一位开发者完成 Review。

## 9. 阶段任务与进度

## 阶段 0：架构冻结（第 1～2 天）

共同任务：

- 创建插件和四模块骨架。
- 冻结语义类别表。
- 冻结坐标系、单位和输出目录。
- 定义 Frame Header、Calibration、Ground Truth 和 Payload。
- 创建最小 Demo Map。

验收：

- 空插件可以编译和加载。
- 两个 Owner 可以分别引用 `Core` 公共接口。
- `main` 分支可运行。

## 阶段 1：最小数据链（第 1 周）

Render Pipeline Owner：

- RGB Scene Capture。
- Semantic Scene Capture。
- 8 类 Custom Stencil 映射。
- 暂时使用同步 Readback 验证数据正确性。
- 导出相机 Intrinsic。

Runtime/System Owner：

- Sensor Subsystem 和 Frame ID。
- Semantic Registry 和 Instance ID。
- Ego/Object Ground Truth。
- Session 目录和基础 CSV/JSON Writer。

联合验收：

- 一次 Capture 能生成 RGB、Semantic、Ground Truth。
- 三者 Frame ID 一致。
- Semantic 图只有合法标签值。

## 阶段 2：异步链路和 LiDAR（第 2 周）

Render Pipeline Owner：

- `FRHIGPUTextureReadback`。
- 3～6 槽 Readback Ring。
- RGB/Semantic Payload。
- Semantic 后处理隔离。
- Readback 延迟统计。

Runtime/System Owner：

- 16/32 线 LiDAR 配置。
- Ray Direction 预计算。
- Batch/Async Trace。
- `.bin` 输出。
- 有界 Export Queue 和 I/O Worker。

联合验收：

- 连续采集时不再依赖正式同步 `ReadPixels()`。
- 点云可由外部工具打开。
- 导出队列不会无限增长。

## 阶段 3：同步与创新功能（第 3 周）

Render Pipeline Owner：

- RGB、Semantic 同帧 Capture 协议。
- 可选 Depth 或 Instance ID。
- 相机外参验证。
- 标签边界自动校验。
- Nanite/Skeletal Mesh/ISM 兼容测试。

Runtime/System Owner：

- `FSensorFrameAssembler`。
- Deterministic Fixed-Step 模式。
- Ground Truth Snapshot 时机固定。
- LiDAR 分块扫描。
- Queue Backpressure 策略。

联合验收：

- Frame N 的全部模态可准确关联。
- 固定 Seed 重复运行时 Actor 轨迹和 Frame 编号一致。
- 发生超时或缺失 Payload 时有明确日志。

## 阶段 4：优化与交付（第 4 周）

Render Pipeline Owner：

- RenderDoc/GPU Profile。
- 720p/1080p 性能测试。
- Readback Ring 压力测试。
- Semantic 合法值和边界报告。
- Camera Frustum/状态 Debug。

Runtime/System Owner：

- LiDAR 性能优化。
- I/O 压力测试。
- Session Metadata。
- 外部数据校验脚本。
- 配置示例和使用说明。

共同任务：

- 完成 Demo 场景。
- 录制演示。
- 输出性能报告和已知限制。
- 在干净 C++ UE 项目中进行一次插件迁移测试。

## 10. 进度跟踪表

| 里程碑 | Owner | 截止 | 状态 | 验收输出 |
|---|---|---:|---|---|
| 模块骨架与公共协议 | 共同 | Day 2 | 待开始 | 插件可编译 |
| RGB Capture | Render | Day 4 | 待开始 | RGB 图像 |
| Semantic Capture | Render | Day 7 | 待开始 | 合法标签图 |
| Sensor Clock/Registry | Runtime | Day 5 | 待开始 | Frame/ID 日志 |
| Ground Truth | Runtime | Day 7 | 待开始 | CSV/JSON |
| Async GPU Readback | Render | Day 12 | 待开始 | 无同步读回链 |
| LiDAR MVP | Runtime | Day 12 | 待开始 | BIN 点云 |
| Async Export Queue | Runtime | Day 14 | 待开始 | 队列统计 |
| Frame Assembler | Runtime | Day 18 | 待开始 | 同步 Frame Packet |
| Depth/Instance 增强 | Render | Day 19 | 待开始 | 可选图像输出 |
| Deterministic 模式 | Runtime | Day 21 | 待开始 | 重复性测试 |
| 性能与兼容测试 | 共同 | Day 26 | 待开始 | 测试报告 |
| Demo 与最终交付 | 共同 | Day 28 | 待开始 | 可复现演示 |

状态统一使用：`待开始`、`进行中`、`阻塞`、`待验收`、`完成`。

## 11. 最终验收指标

### 数据正确性

- RGB、Semantic、LiDAR 和 Ground Truth 使用一致 Frame ID。
- 坐标转换有至少三个已知点验证。
- Semantic 图中非法标签像素数为 0。
- 相机内外参可以把已知 3D 点投影到正确图像位置。
- LiDAR 点云与场景几何在外部工具中对齐。

### 性能

- 720p RGB + Semantic 可以稳定连续采集。
- 16×512 LiDAR 达到至少 5 Hz；推荐目标为 32×1024、5～10 Hz。
- 正式采集链没有每帧同步 GPU Readback。
- Export Queue 有最大容量且能报告积压。
- 长时间采集没有持续内存增长。

### 工程质量

- 插件不修改 Engine。
- `main` 可编译。
- 公共结构有版本。
- 配置、坐标系和格式有文档。
- 在另一个干净 UE C++ 项目中能够安装并运行。

## 12. 风险与降级策略

| 风险 | 影响 | 降级策略 |
|---|---|---|
| GPU Readback API 版本差异 | 相机链延期 | 先以低频同步验证，随后只保留异步正式路径 |
| Custom Stencil 覆盖不完整 | 标签缺失 | MVP 限定支持 Static/Skeletal Mesh，记录其他类型限制 |
| Async Trace API 不适配批量需求 | LiDAR 性能不足 | 多帧分批同步 Trace，限制线数与采样数 |
| PNG 压缩速度不足 | I/O 队列积压 | Semantic 改用原始/无损整数格式，RGB 降低频率 |
| 多模态完成时间差异 | Frame 长期等待 | Payload 超时、失败状态和确定性阻塞模式 |
| 一个月工期不足 | 无法交付 | 优先保留 RGB、Semantic、16×512 LiDAR、GT 和同步协议 |

## 13. 推荐优先级

从高到低：

1. Frame 同步、时间戳和坐标系正确。
2. Semantic 标签正确且无颜色污染。
3. RGB 与 LiDAR 数据可被外部工具读取。
4. GPU Readback 和 I/O 不持续阻塞主线程。
5. Ground Truth 字段完整。
6. LiDAR 扫描真实性。
7. Depth、Instance、噪声和 Editor 工具。

项目成功的核心不是传感器数量，而是生成的数据是否同步、可标定、可复现、可验证。
