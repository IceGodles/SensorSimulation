# Cosys-AirSim 设计抽取与轻量化映射

## 原则

本插件只借鉴设计，不包含 Cosys-AirSim 源码。目标是保留传感器系统中有价值的边界，同时去除 AirLib、RPC、飞控、车辆动力学、ROS 和 NED 全套依赖。

## 参考源码入口

本地参考仓库：`D:\ueprojects\Cosys-AirSim-reference`

| 主题 | Cosys-AirSim 入口 | 抽取结论 |
|---|---|---|
| Camera 组织 | `Unreal/Plugins/AirSim/Source/PIPCamera.*` | 一个逻辑相机管理多个 Capture Channel 和 Render Target；每个通道独立配置格式、后处理与启停 |
| Sensor Factory | `UnrealSensors/UnrealSensorFactory.*` | 配置只描述 Sensor，工厂负责实例化具体 UE Adapter；轻量版用 Component 注册替代复杂工厂 |
| Annotation | `Annotation/ObjectAnnotator.*`、`AnnotationComponent.*` | 标签注册和渲染表示分离；对象标签必须稳定，支持 RGB、灰度、纹理和 Instance 模式 |
| CPU LiDAR | `UnrealSensors/UnrealLidarSensor.*` | 水平/垂直角预计算，跨 Tick 扫描，完整一圈才刷新发布；每个 Hit 同时产生位置与标签 |
| LiDAR 参数 | `AirLib/include/sensors/lidar/LidarSimpleParams.hpp` | 配置包含 Channels、Measurements、Rotation、FOV、Range、Noise、External、Update Frequency |
| 扫描完整性 | `AirLib/include/sensors/lidar/LidarSimple.hpp` | 临时缓存与最终输出分离；只有发生完整扫描刷新时更新公共输出和时间戳 |
| GPU LiDAR | `UnrealSensors/UnrealGPULidarSensor.*`、`LidarCamera.*` | 以相机/深度渲染替代大量 CPU Trace；首期只保留接口和架构扩展点 |
| 坐标转换 | `NedTransform.*` | 所有 UU/NED/FLU 转换集中在单一服务，禁止在各 Sensor/Exporter 中重复实现 |

## 新框架映射

### Camera Sensor

`USensorCameraRigComponent` 对应一个物理相机，并拥有多个 `FSensorCameraChannelConfig`：

- RGB
- Semantic
- Depth
- Instance

每个 Channel 拥有独立 `USceneCaptureComponent2D` 和 `UTextureRenderTarget2D`。这保留 Cosys 的多通道组织，但不携带 PIP、Cine Camera、Noise Material、RPC 和 Detection API。

### Annotation

`USemanticObjectComponent` 保存稳定的 `SemanticId` 和由 Registry 分配的 `InstanceId`。MVP 使用 Custom Depth/Stencil；未来独立 Semantic Mesh Pass 不改变 Registry 协议。

与 Cosys 的区别：

- 不通过组件代理替换所有 Scene Proxy。
- 不在第一版支持 Texture Annotation。
- 数据 ID 是主表示，调试颜色只是 LUT 显示。

### CPU LiDAR

`FLidarScanPattern` 只负责角度和 Local Direction；`USimLidarSensorComponent` 只负责 UE World Trace。二者分离对应 Cosys 的 `LidarSimpleParams/LidarSimple` 与 `UnrealLidarSensor` 边界。

扫描契约：

1. 配置变化时预计算全部 Ray Direction。
2. `RequestCapture` 创建一次扫描事务。
3. 每 Tick 最多处理 `RaysPerTick`。
4. 每条 Ray 增加 `CompletedRayCount`，无命中也必须计数。
5. 只有 `CompletedRayCount == ExpectedRayCount` 才设置 `bCompleteRevolution`。
6. Frame Assembler 拒绝不完整扫描。
7. 每个 Hit 同时读取 `SemanticId/InstanceId`。

### GPU LiDAR 扩展点

未来实现独立 `ISimLidarBackend`：

```text
USimLidarSensorComponent
        |
        +-- FPhysicsTraceLidarBackend
        `-- FGpuDepthLidarBackend
```

GPU Backend 预期流程：

```text
Cube/Cylindrical depth capture
    -> RDG depth conversion
    -> GPU point reconstruction
    -> semantic/instance lookup
    -> asynchronous buffer readback
    -> FLidarScanPayload
```

首期不实现是因为 GPU LiDAR 同时涉及投影误差、遮挡、整数标签、Readback 和平台差异，不适合与 CPU MVP 同期展开。

### 坐标转换

`FSensorCoordinateConverter` 集中处理：

- UE：X Forward、Y Right、Z Up、cm、左手。
- Dataset FLU：X Forward、Y Left、Z Up、m、右手。
- OpenCV Camera：X Right、Y Down、Z Forward、m。

所有 Sensor 输出先以 Sensor Local 表示，再由 Exporter 选择目标坐标系。

## 明确不抽取的内容

- AirLib `SensorBase` 继承树。
- msgpack RPC 和 Python Server。
- Vehicle API、PX4、ArduPilot。
- NED/GPS 全局坐标体系。
- ROS/ROS2 Bridge。
- Echo、WiFi、UWB 等非 MVP Sensor。
- Cosys 的 Annotation Scene Proxy 实现。
- GPU LiDAR 具体源码。

这些内容均可在稳定的数据协议之上以后作为 Adapter 增加，不应污染首期核心模块。
