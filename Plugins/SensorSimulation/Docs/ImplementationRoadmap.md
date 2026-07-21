# SensorSimulation 实现路线

## 当前状态

已完成：

- [x] Semantic Channel 无后处理污染的 Global Shader。
- [x] `FImageReadbackManager::Enqueue` 与非阻塞 GPU Readback。
- [x] RGB/Semantic Payload 输出并提交给 `USimulationSubsystem`。
- [x] Semantic 标签、View Rect、Gamma 与像素格式校验。
- [x] `SensorSimulationHostEditor Win64 Development` 编译验证。

仍待完成：

- [ ] Export Worker 与 PNG/BIN/CSV Writer。
- [ ] 多个同模态传感器的完成数量追踪。
- [ ] Frame Timeout、丢帧统计与更完整的背压策略。
- [ ] 自动化 Demo Map 和运行时像素回归测试。
- [ ] Instance 32 位整数标签 Pass。

## Render Pipeline Owner

### 1. [x] 为 Semantic Channel 创建无后处理污染的 Global Shader

#### 为什么要这样做

Semantic 图像保存的是离散类别编号，而不是用于显示的颜色。TAA/FXAA、Bloom、Motion Blur、曝光和 Tonemap 都可能混合相邻像素或改变数值，使一个合法标签变成不存在的中间值。因此 Semantic Channel 必须绕开颜色后处理，并逐像素输出确定的整数标签。

#### 如何做

1. `USemanticObjectComponent` 把 `SemanticId` 限制到 Custom Stencil 可表达的 `0..255`；`0` 保留为背景。
2. Semantic Scene Capture 使用线性 `RGBA8` Render Target，并关闭 Anti-Aliasing、Bloom、Depth of Field、Eye Adaptation 和 Motion Blur。
3. `FSemanticCaptureViewExtension` 只接管 Semantic 专用 Scene Capture。
4. `FSemanticCapturePS` 使用整数像素坐标读取 Custom Stencil，不读取 Scene Color，不做双线性采样。
5. Shader 输出协议固定为 `R=SemanticId, G=0, B=0, A=255`，并覆盖 Tonemap 后的颜色结果。
6. 工程使用 `r.CustomDepth=3`，即 Custom Depth with Stencil。

对应代码：

- `Shaders/Private/SemanticCapture.usf`
- `SimulationRenderer/Private/SemanticCaptureViewExtension.*`
- `SimulationRenderer/Private/SimulationRenderer.cpp`
- `SimulationRenderer/Private/CameraRigComponent.cpp`
- `SimulationRuntime/Private/SemanticObjectComponent.cpp`

#### 下一步可以怎么优化

- 用真正的整数 Render Target 扩展 Instance Channel，避免 32 位实例编号被 RGBA8 限制。
- 增加离屏自动化测试，逐像素比较预期标签，并覆盖物体运动、遮挡和边缘情况。
- 用显式的捕获标识替代对 Scene Capture Source 的约定，让 Semantic View 的识别更清晰。

> 本项本轮只补充文档，没有修改已经完成的 Global Shader 代码。

### 2. [x] 实现 `FImageReadbackManager::Enqueue`

#### 为什么要这样做

每帧调用 `ReadPixels()` 会让游戏线程等待渲染线程和 GPU，破坏实时性。异步 GPU Readback 先把纹理复制到 staging texture，再通过 fence 判断 GPU 是否完成，CPU 只在就绪后复制数据，因此不会在正常轮询路径中主动等待 GPU。

#### 如何做

1. 游戏线程先验证 Render Target、Payload 类型、尺寸、Gamma 和像素格式。
2. 使用原子计数预留有限队列容量；队列满时拒绝请求并记录日志，防止内存无上限增长。
3. 通过 `ENQUEUE_RENDER_COMMAND` 在渲染线程创建 `FRHIGPUTextureReadback`。
4. 对 `[0, 0, Width, Height]` 完整 View Rect 调用 `EnqueueCopy`。
5. `PollCompleted` 只排队一次非阻塞 fence 检查；`IsReady()` 为真后才执行 `Lock`。
6. 依据返回的 `RowPitchInPixels` 逐行复制，忽略 staging texture 的行尾 padding。
7. 若底层格式为 `PF_B8G8R8A8`，复制时交换 R/B，最终 `FImagePayload::Bytes` 始终使用紧密排列的规范 RGBA8。
8. CPU Payload 拥有自己的 `TArray<uint8>`，解锁 staging texture 后数据仍然有效。
9. 共享状态使用线程安全引用计数，保证排队的渲染命令不会访问已经销毁的 Manager。

对应代码：

- `SimulationRenderer/Public/ImageReadbackManager.h`
- `SimulationRenderer/Private/ImageReadbackManager.cpp`

#### 下一步可以怎么优化

- 用统一的渲染线程 ticker 或单一 pump 命令代替每次游戏线程轮询都排队一个 pump。
- 建立 `FRHIGPUTextureReadback` 对象池，减少高频采集中的 staging 资源创建。
- 增加按通道统计的队列深度、GPU 延迟、拒绝次数和读回失败次数。
- 为 Depth、Instance 增加独立的格式转换器，而不是把 Manager 限定在 RGBA8。

### 3. [x] 输出 RGB/Semantic Payload 并提交给 Subsystem

#### 为什么要这样做

Renderer 模块只应负责捕获和读回，不能反向依赖 Runtime Subsystem，否则会形成 `SimulationRuntime -> SimulationRenderer -> SimulationRuntime` 的循环依赖。需要一个位于 Runtime 模块的 Adapter，把渲染结果交给帧聚合逻辑。

#### 如何做

1. `UCameraRigComponent::SubmitCapture` 根据请求中的模态位，只处理启用的 RGB/Semantic 通道。
2. 每个通道先调用 `CaptureScene()`，再排队 `EnqueueCopy`；渲染命令顺序保证读到本次捕获结果，而不是上一帧。
3. Camera Rig 暴露 `PollCompletedImage` 和 `GetEnabledPayloadTypes`，但不引用 Runtime。
4. 新增 `USimCameraSensorComponent` 作为 Runtime Adapter：
   - 自动查找同一 Actor 上的 `UCameraRigComponent`；
   - 把 Subsystem 的 `FCaptureRequest` 转发给 Camera Rig；
   - 每 Tick 非阻塞清空完成队列；
   - 使用移动语义调用 `USimulationSubsystem::SubmitImage`。
5. `USimSensorComponentBase::GetPayloadTypes` 让 Camera 返回 RGB/Semantic、LiDAR 返回 LiDAR。
6. Subsystem 用真实传感器能力计算整帧 `ExpectedPayloads`，不再把所有传感器都误判为 LiDAR。

运行时使用要求：

- Camera Actor 上同时添加 `UCameraRigComponent` 与 `USimCameraSensorComponent`。
- Camera Rig 的 Channels 中启用 RGB 和/或 Semantic。
- Adapter 默认自动关联同一 Actor 上的 Camera Rig，也可以在 Details 中显式指定。
- 只有通过 Runtime Adapter 发起的捕获才会进入同步 Frame/Subsystem 管线；编辑器调试按钮仍是独立人工检查入口。

对应代码：

- `SimulationRenderer/Public/CameraRigComponent.h`
- `SimulationRenderer/Private/CameraRigComponent.cpp`
- `SimulationRuntime/Public/SimCameraSensorComponent.h`
- `SimulationRuntime/Private/SimCameraSensorComponent.cpp`
- `SimulationRuntime/Public/SimSensorComponentBase.h`
- `SimulationRuntime/Public/SimLidarSensorComponent.h`
- `SimulationRuntime/Private/SimulationSubsystem.cpp`

#### 下一步可以怎么优化

- 当前 `EPayloadType` 是模态位集合，同一帧存在两台 RGB 相机时，第一台到达就会把 RGB 位标记完成。后续应按“传感器名 + 模态”记录预期数量。
- Adapter 可增加采样频率调度，避免所有相机完全依赖 Subsystem 的全局固定步长。
- 读回队列满时应把失败状态反馈给 FrameAssembler，使帧超时或明确丢弃，而不是永久等待。
- 把完成 Payload 送入 Export Worker 的无锁队列，避免文件编码占用游戏线程。

### 4. [x] 验证标签合法值、View Rect、Gamma 和像素格式

#### 为什么要这样做

即使 Shader 正确，错误的纹理格式、Gamma、View Rect 或 CPU 通道顺序仍会悄悄破坏标签。如果等到导出后才发现，错误会扩散到整批数据集。因此校验分布在“GPU Copy 前”和“进入 FrameAssembler 前”两个边界。

#### 如何做

##### 标签合法值

- `FSemanticRegistry::GetImageSemanticIds` 收集背景 `0` 和当前有效 Semantic Component 的 8 位标签。
- Subsystem 对 Semantic Payload 逐像素检查：
  - R 必须属于合法标签集合；
  - G 必须为 0；
  - B 必须为 0；
  - A 必须为 255。
- 任一像素非法时，整个 Payload 被拒绝，不进入 FrameAssembler，并输出包含 Frame、Sensor、类型和尺寸的错误日志。

##### View Rect

- 提交时要求 Render Target 的宽高大于 0。
- 渲染线程再次确认 RHI Texture 的尺寸与提交时尺寸完全一致。
- GPU Copy 显式使用完整 `FResolveRect(0, 0, Width, Height)`。
- CPU 复制检查 `RowPitch >= Width`、`BufferHeight >= Height`，并逐行复制有效宽度。
- Payload 必须满足 `Bytes.Num() == Width * Height * 4`。

##### Gamma

- Semantic Render Target 必须设置 `bForceLinearGamma=true`，否则 Enqueue 直接拒绝。
- RGB 保留 Channel 配置，可按显示图像需求使用非线性输出。
- Semantic 值不经过 sRGB/Gamma 转换，R 字节才能精确恢复原始 ID。

##### 像素格式

- 当前正式图像管线只接受 `PF_B8G8R8A8` 或 `PF_R8G8B8A8`。
- 两种 RHI 布局都会被规范化成协议统一的紧密 RGBA8。
- Payload 固定 `BytesPerPixel=4`；尺寸或字节数不匹配会被 Subsystem 拒绝。

对应代码：

- `SimulationRenderer/Private/ImageReadbackManager.cpp`
- `SimulationRuntime/Public/SemanticRegistry.h`
- `SimulationRuntime/Private/SemanticRegistry.cpp`
- `SimulationRuntime/Public/SimulationSubsystem.h`
- `SimulationRuntime/Private/SimulationSubsystem.cpp`

#### 下一步可以怎么优化

- 当前 Semantic 校验是每帧全像素扫描，适合正确性优先阶段；后续可用 SIMD、并行任务或 GPU reduction 统计非法标签。
- 在 Payload 中显式记录 `PixelFormat`、`ColorSpace`、`ViewRect` 和 `RowStride`，减少仅靠管线约定解释数据的风险。
- 将第一个非法像素的坐标、实际 RGBA 和期望集合写入诊断日志，提升定位效率。
- 增加开发环境严格模式与发布环境采样校验模式，平衡性能和数据质量。

## 验证方法

### 编译验证

已执行：

```text
E:\unreal\Windows\Engine\Build\BatchFiles\Build.bat SensorSimulationHostEditor Win64 Development -Project=D:\ueprojects\SensorSimulationHost\SensorSimulationHost.uproject -WaitMutex -NoHotReloadFromIDE
```

结果：`Succeeded`。

### 编辑器运行时验证

1. 创建 Camera Actor，添加 `UCameraRigComponent` 和 `USimCameraSensorComponent`。
2. 在 Camera Rig 的 Channels 中启用 RGB 与 Semantic；Semantic 必须为线性 Gamma。
3. 创建三个带可渲染 Primitive 的 Actor，分别添加 `USemanticObjectComponent`，设置 `SemanticId=10/20/200`。
4. 确认 `r.CustomDepth=3`，修改配置后重启编辑器。
5. 进入 PIE，让 `USimulationSubsystem` 按固定步长发起请求。
6. Output Log 中不应出现：
   - `Readback rejected`
   - `Readback View Rect validation failed`
   - `GPU readback returned an invalid buffer`
   - `Rejected image payload`
7. 人工检查 Semantic Render Target：
   - 背景为 0；
   - 对象 R 通道分别为 10、20、200；
   - G/B 为 0，A 为 255；
   - 物体内部均匀；
   - 边缘无渐变、泛光和拖影；
   - 相机或物体移动后没有旧标签残影。

> `Save Semantic Debug Image` 使用同步 PNG 导出，仅用于人工调试。正式逐帧管线使用本次实现的 `FRHIGPUTextureReadback`，不会复用同步 `ReadPixels` 路径。

## 首个集成验收

- 场景中一个 Camera Rig + Camera Adapter 和一个 16×512 LiDAR。
- 三个带 Semantic Component 的可渲染 Actor。
- 同一 Frame 输出 RGB、Semantic、LiDAR 和 Ground Truth。
- LiDAR `ExpectedRayCount == CompletedRayCount`。
- Semantic 图像非法 ID 像素数为 0。
- 正式逐帧管线不调用同步 `ReadPixels()`。
