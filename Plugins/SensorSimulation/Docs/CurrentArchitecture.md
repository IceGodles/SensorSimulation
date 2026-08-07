# SensorSimulation 当前架构说明

> 本文描述当前代码实际存在的架构，不把路线规划中的功能写成已完成。
> 各部分统一采用：**为什么要这样做 → 当前如何做 → 相较旧架构修改了什么及原因 → 下一步做什么/如何优化**。
> 当前状态基于 2026-08-07 的项目源码、编译结果与自动化验收结果。

## 0. 当前状态摘要

当前已经闭环：

- RGB、Semantic、Depth Scene Capture。
- RGB、Semantic、Depth、Instance 异步 GPU 读回。
- Semantic 无颜色后处理污染的 Global Shader。
- Camera Adapter → Runtime Subsystem → FrameAssembler。
- LiDAR 分批扫描 → Runtime Subsystem → FrameAssembler。
- Ground Truth → FrameAssembler。
- 按传感器记录预期/完成模态和帧超时清理。
- 完整帧 → 有界 Export Queue → 后台 Worker → 数据集文件。
- Dataset Session、逐通道相机标定、会话元数据和 Renderer 按通道指标快照。
- Camera Rig/Readback 与 Export Worker 生命周期自动化测试。
- 真正的 32 位 Instance 网格绘制通道、原生 `PF_R32_UINT` 目标与读回、协议校验和文件写出。

当前仍未闭环：

- 独立 HISM、Masked foliage、SkeletalMesh、半透明遮挡等特殊对象的完整跨 RHI 验收矩阵（Nanite 与 ISM/HISM 内部实例已闭环）。
- 完整的 D3D11/D3D12 × RGB/Semantic/Depth × 多分辨率自动化矩阵。

---

## 1. 总体架构

SensorSimulation 分成四个模块：

- `SimulationCore`：公共协议、坐标转换和纯算法。
- `SimulationRenderer`：Camera Channel、Scene Capture、Semantic Global Shader 和异步 GPU Readback。
- `SimulationRuntime`：时钟、传感器调度、语义注册、数据校验、帧聚合、数据集会话和后台导出。
- `SimulationEditor`：编辑器扩展入口，目前仍以模块骨架为主；验收场景主要通过关卡资产和 `Tools` 脚本维护。

### 模块依赖

```mermaid
flowchart LR
    Core["SimulationCore<br/>协议 · 坐标 · 扫描方向"]
    Renderer["SimulationRenderer<br/>SceneCapture · Shader · GPU Readback"]
    Runtime["SimulationRuntime<br/>时钟 · 调度 · 校验 · 聚合 · 导出"]
    Editor["SimulationEditor<br/>编辑器扩展骨架"]

    Core --> Renderer
    Core --> Runtime
    Renderer --> Runtime
    Core --> Editor
    Renderer --> Editor
    Runtime --> Editor
```

箭头表示“被依赖模块 → 使用它的模块”。`SimulationRenderer` 不依赖 `SimulationRuntime`；Camera 到 Subsystem 的桥接仍放在 Runtime，避免形成循环依赖。

### 为什么要这样分层

渲染线程、游戏世界调度和公共数据协议具有不同生命周期：

- Shader、RDG、RHI Readback 只能由 Renderer 理解。
- World、Actor、Sensor 调度和导出会话属于 Runtime。
- 图像、点云和帧头必须使用不依赖具体渲染后端的公共协议。
- Editor API 不应进入打包运行时模块。

### 当前如何做

- 公共数据结构集中在 `SimulationCore/Public/SimulationTypes.h`。
- Renderer 只生产 `FImagePayload`，不了解 `USimulationSubsystem`。
- Runtime 通过 `USimCameraSensorComponent` 连接 Camera Rig 和 Subsystem。
- Runtime 通过 `USimLidarSensorComponent` 直接把完成扫描提交给 Subsystem。
- Subsystem 把完整 `FFramePacket` 交给 `FExportService`，文件编码与 IO 不进入 Renderer。

### 相较旧架构修改了什么及原因

- **Runtime 从“只有导出队列骨架”升级为完整 Worker。**
  原因是 PNG 编码、BIN/JSON 写入不能阻塞主采集路径；完整帧现在由后台线程消费。
- **帧聚合从纯模态位掩码增加到按传感器状态跟踪。**
  原因是两台相机都输出 RGB 时，第一台 RGB 到达不能代表整帧 RGB 已全部到齐。
- **Renderer 的图像协议增加显式格式和单位。**
  原因是 Depth 与 Semantic 都不能由消费者根据通道名称猜测字节含义。

### 下一步做什么/如何优化

- 继续缩小 Public 头文件的 UE 渲染依赖。
- [x] 运行模式统一使用 Core 的 `ESimulationMode`；Runtime 设置直接引用该反射枚举，配置值保持兼容。
- 在 Editor 模块实现验收场景生成、结果浏览和数据质量面板，逐步替代分散的外部脚本。

---

## 2. 一帧数据的总体流程

```mermaid
flowchart TD
    Tick["USimulationSubsystem::Tick"]
    Request["RequestFrame<br/>创建 Header 和整帧 ExpectedPayloads"]
    Begin["FrameAssembler::BeginFrame"]
    Truth["CaptureGroundTruth"]
    Register["RegisterSensor<br/>记录 SensorName + ExpectedPayloads"]
    Sensors["向启用的 Sensor 下发 RequestCapture"]

    Camera["Camera Adapter"]
    Rig["CameraRig::SubmitCapture"]
    GPU["RGB/Semantic/Depth/Instance<br/>SceneCapture + GPU Readback"]
    Image["PollCompletedImage"]
    Validate["ValidateImagePayload"]
    AddImage["FrameAssembler::AddImage"]

    Lidar["LiDAR 分 Tick TraceBatch"]
    FinalLidar["FinalizeScan"]
    SubmitLidar["Subsystem::SubmitLidar"]

    Complete{"整帧模态与每个 Sensor<br/>都完成？"}
    Packet["PopCompleteFrame"]
    ExportQueue["ExportService::Enqueue"]
    Worker["Export Worker"]
    Files["PNG / BIN / JSON"]

    Tick --> Request --> Begin
    Request --> Truth
    Request --> Register --> Sensors
    Sensors --> Camera --> Rig --> GPU --> Image --> Validate --> AddImage --> Complete
    Sensors --> Lidar --> FinalLidar --> SubmitLidar --> Complete
    Truth --> Complete
    Complete -->|是| Packet --> ExportQueue --> Worker --> Files
    Complete -->|否| Tick
```

### 为什么要统一 Frame Header

RGB、Semantic、Depth 和 LiDAR 的完成时刻不同：

- Scene Capture 与 GPU Readback 可能晚一到数帧返回。
- LiDAR 会把完整扫描拆成多个 Tick。
- Export Worker 又在独立线程异步写盘。

如果结果不保留最初的 FrameId、时间戳和 SensorGuid，就无法知道不同异步结果是否属于同一同步帧；SensorName 只负责提供可读显示。

### 当前如何做

- `RequestFrame` 创建唯一 `FFrameHeader`。
- 所有传感器收到相同的 `SequenceId`、`FrameId` 和仿真时间戳。
- 每个 Payload 保存原始 Header、稳定 SensorGuid 和显示用 SensorName。
- `FFrameAssembler` 使用 FrameId 索引待完成帧。
- `RegisterSensor` 以 SensorGuid 区分传感器，并以 ChannelGuid 分别保存图像预期；同名传感器或同模态多通道都必须逐项完成。
- 完成帧移动到 Export Queue，避免复制大图像和点云缓冲区。

### 相较旧架构修改了什么及原因

- **LiDAR 的 `FinalizeScan` 已调用 `SubmitLidar`。**
  原因是旧架构只广播完成委托，包含 LiDAR 的帧会永久等待。
- **增加 `PurgeTimedOutFrames`。**
  原因是任何传感器拒绝或丢失 Payload 都不应让 Pending Frame 永久占用内存。
- **完整帧已连接 ExportService。**
  原因是第一轮验收需要验证真正的数据集输出，而不只是内存中的 `FFramePacket`。

### 下一步做什么/如何优化

- [x] 完成计数键已从 SensorName 迁移为持久 SensorGuid；普通复制生成新 GUID，PIE 复制保持身份。
- [x] `RequestCapture` 返回 `Accepted/Busy/Rejected`；Subsystem 遇到 Busy 或 Rejected 时立即调用 `FailFrame`，不再等待超时。
- [x] Pending 阶段的同传感器、同 `ChannelGuid` 重复 Payload 被拒绝并计入 `DuplicatePayloads`；完成队列使用 FrameId 集合保证只入队一次。
- [x] Completed、Failed、TimedOut 都进入最近 1024 帧的有限终态历史；其后到达的 Payload 被丢弃并计入 `LatePayloads`，避免历史无限增长。
- 下一步可为 Pending Frame 增加容量上限，并把终态历史容量开放为项目设置。

---

## 3. SimulationCore：协议和纯算法

### 为什么要这样做

Camera、LiDAR、Ground Truth、FrameAssembler 和 Writer 必须共享唯一的数据解释，否则容易出现：

- Unreal 厘米与数据集米混用。
- BGRA 被误当作 RGBA。
- Semantic 图被误执行 Gamma。
- Depth 数组被误认为颜色图。
- ViewRect、RowPitch 和实际图像宽度混淆。

### 当前如何做

`SimulationTypes.h` 定义：

- `EPayloadType`：RGB、Semantic、Depth、Instance、LiDAR、GroundTruth。
- `FFrameHeader`：序列号、帧号、时间戳和自车位姿。
- `FCaptureRequest`：传感器采集请求；`ExpectedImageChannels` 按 `ChannelGuid + PayloadType` 列出本帧的独立图像输出。
- `FCalibration`：带 `ChannelGuid + PayloadType` 身份的逐通道相机外参与针孔内参。
- `FImagePayload`：CPU 图像及其机器可读元数据，包含生成它的稳定 `ChannelGuid`。
- `FLidarScanPayload`：点云和扫描进度。
- `FObjectGroundTruth`：对象真值。
- `FFramePacket`：最终聚合帧。

`FImagePayload` 当前显式包含：

| 字段 | 作用 |
|---|---|
| `ImageSize` | 有效图像宽高 |
| `ViewRect` | 有效像素区域 |
| `PixelFormat` | `Rgba8`、`R32Float` 或 `R32Uint` |
| `ColorSpace` | `SRgb`、`Linear` 或 `Data` |
| `ValueUnit` | `None`、`Meters` 或 `Identifier` |
| `BytesPerPixel` | 当前支持的正式图像均为 4 |
| `RowStrideBytes` | CPU 规范化后的紧密行字节数 |
| `Bytes` | 独立拥有的 CPU 字节数组 |

当前图像协议：

| 模态 | CPU 格式 | ColorSpace | ValueUnit |
|---|---|---|---|
| RGB | RGBA8 | SRgb 或 Linear | None |
| Semantic | RGBA8，`R=ID,G=0,B=0,A=255` | Data | Identifier |
| Depth | R32Float | Data | Meters |
| Instance | R32Uint | Data | Identifier |

`FCoordinateConverter` 集中处理 Unreal、车辆 FLU 和 OpenCV Camera 坐标转换。
`FLidarScanPattern` 生成稳定顺序的 LiDAR 局部射线。

### 相较旧架构修改了什么及原因

- **PixelFormat、ColorSpace、ValueUnit、ViewRect 和 RowStride 已从规划进入正式协议。**
  原因是新增 Depth 后，仅靠 `PayloadType` 无法安全解释同样为 4 字节/像素的数据。
- **Depth 在 Renderer/Runtime 边界统一转换为米。**
  原因是单位换算应只发生一次，下游 Writer 和算法不应重复猜测 UE 厘米。

### 下一步做什么/如何优化

- 为协议增加版本号和字节序声明。
- 为 Depth 定义未命中背景值、无效值和 NaN/Inf 策略。
- 将 Instance 校验扩展到 R15 特殊渲染对象和 ISM/HISM 逐内部实例语义。
- 扩充坐标、旋转、投影和相机标定自动化测试。

---

## 4. SimulationRenderer：相机、多模态渲染与 GPU Readback

### 4.1 Camera Rig

#### 为什么要这样做

同一相机位姿需要产生多个严格对齐的输出。RGB、Semantic 和 Depth 具有不同 CaptureSource、RenderTarget 格式、Gamma 和后处理要求，因此由一个 Camera Rig 统一创建、配置和触发。

#### 当前如何做

```mermaid
flowchart LR
    Config["FCameraChannelConfig[]"] --> Identity["EnsureChannelGuids<br/>稳定序列化身份"]
    Identity --> Constraint["有效配置筛选<br/>ChannelGuid 唯一，ChannelType 可重复"]
    Constraint --> Apply["ApplyConfiguration<br/>配置 Diff"]
    Apply --> Reuse["FOV/顺序/容量变化<br/>复用 Capture + RT"]
    Apply --> Rebuild["Resolution/Gamma/模态变化<br/>复用 Capture + 新 RT"]
    Apply --> AddRemove["Enable/Disable<br/>只增删受影响通道"]
    Rebuild --> Retire["旧 RT 暂存<br/>Pending Readback 归零后释放"]
    Reuse --> Submit["SubmitCapture"]
    Rebuild --> Submit
    AddRemove --> Submit
    Submit --> Readback["全局 FImageReadbackManager Pump"]
    Readback --> Metrics["逐通道指标<br/>周期写入 Session metadata"]
```

- `OnRegister` 创建 `FImageReadbackManager`，首次 `BuildChannels` 只负责建立初始运行时资源。
- 每条 `FCameraChannelConfig` 拥有序列化 `ChannelGuid`。缺失或重复 GUID 会自动修复；编辑器旧关卡的迁移进入事务并标记待保存，数组重排后仍按 GUID 匹配原资源。
- 同一 `ChannelType` 允许多条 Enabled 配置；运行时资源复用、RenderTarget 查询和像素格式查询均以 `ChannelGuid` 为键，`ChannelType` 只描述输出数据类型。
- `Instance` 使用普通 SceneCapture 工作目标建立视图，并使用独立的原生 `PF_R32_UINT` 目标保存正式输出；该通道不再被拒绝。
- `ApplyConfiguration` 比较有效 Channels、Resolution、FOV、Gamma 和 `MaxPendingReadbacks` 与已应用快照；资源配置相同时是 O(n) 空操作，不触碰 UObject 或 GPU 资源。
- `MaxPendingReadbacks` 通过原子容量快照安全热更新：缩容不取消在途任务，只约束后续 Enqueue，也不重建 Manager、Capture 或 RT。
- 编辑器 `PostEditChangeProperty`、正式 `SubmitCapture` 和 Debug Capture 都进入同一个热更新入口，避免编辑器预览与数据集采集使用不同配置。
- FOV 或数组顺序变化只重新配置 Capture，Capture 与 RenderTarget 都复用。
- Resolution/Gamma 变化复用 `USceneCaptureComponent2D`，只为受影响通道创建新 `UTextureRenderTarget2D`。
- 通道启用、禁用或删除只创建/销毁对应通道，未受影响通道保持原对象。
- 如果旧 Target 仍关联 Pending Readback，它会进入 `RetiredTargets` 保活；最后一个 Payload 交付后才注销 Semantic 身份并交给 GC，避免在途渲染命令持有悬空资源。
- `FCameraRigResourceStats` 记录配置 Apply/Change/No-op，以及 Capture/RT 的创建、复用、重建和销毁次数。
- 配置真正变化后广播 `OnConfigurationChanged`；Camera Adapter 为每个实际运行通道构建独立 Calibration，Dataset Session 按 `SensorName + ChannelGuid` Upsert，使不同模态/分辨率不会互相覆盖。
- Camera Adapter 每秒以及 `EndPlay` 前登记一次资源、Readback 聚合和按通道指标快照，最终写入 `metadata.json` 的 `renderer.camera_rigs`。
- `SubmitCapture` 已统一处理 RGB、Semantic、Depth 和正式的 32 位 Instance 通道。

通道配置：

| 通道 | CaptureSource | GPU RenderTarget | 正式 CPU Payload |
|---|---|---|---|
| RGB | `SCS_FinalColorLDR` | RGBA8 | RGBA8 |
| Semantic | `SCS_FinalToneCurveHDR` | Linear RGBA8 | RGBA8 标签 |
| Depth | `SCS_SceneDepth` | RGBA32F | 提取 R 后转换为 R32Float 米 |
| Instance | `SCS_FinalColorLDR` 工作视图 | 原生 `PF_R32_UINT` | 紧密排列的 R32Uint 标识符 |

#### 为什么 Depth 的 GPU Target 是 RGBA32F，而 CPU 是 R32Float

UE 5.7 的 Scene Capture 在 RGBA32F RenderTarget 上能可靠把 SceneDepth 写到逻辑 R。直接使用 R32F RenderTarget 的实测结果为全零，因此：

1. GPU 端使用 RGBA32F 保证 SceneCapture 兼容。
2. Readback 只提取逻辑 R。
3. UE 厘米乘 `0.01` 转为米。
4. 最终 CPU Payload 仍是紧密 R32Float，没有四通道带宽泄漏到数据集协议。

#### 相较旧架构修改了什么及原因

- **Depth 已从“未来通道”变为正式支持通道。**
  原因是 GPU Debug EXR、正式异步 BIN 和单位转换已通过验收。
- **Capture/RT 已从“仅逐帧复用”升级为配置 Diff 驱动的选择性复用。**
  原因是全量 `DestroyChannels → BuildChannels` 会让一个通道的 FOV 或分辨率变化同时抖动其他通道；现在只有真正受影响的资源发生变化。
- **运行时资源身份从 `ChannelType` 改为稳定 `ChannelGuid`。**
  原因是模态只描述数据类型，不能唯一表达配置身份；GUID 允许数组重排与属性热更新后继续复用正确资源，并为未来产品化多同模态输出保留身份基础。
- **同类型多通道已改为使用 ChannelGuid 独立寻址，Instance 也已成为正式通道。**
  Instance 不再把 LDR 颜色目标当作正式数据，而是拥有原生整数输出目标和精确的 uint32 协议；同一模态的多条配置则通过各自 ChannelGuid 独立创建、路由和完成计数。
- **旧 Target 增加 Pending Readback 排空前保活。**
  原因是热更新可能发生在 GPU Copy 尚未完成时；直接原地重建或释放会让已排队命令引用过期资源。
- **Calibration 从“每个 Rig 取第一条”改为逐运行通道登记。**
  原因是 RGB、Semantic、Depth 可以采用不同分辨率；`SensorName + ChannelGuid` Upsert 才能保证热更新后每份内参与实际图像一致。
- **`MaxPendingReadbacks` 改为原子容量热更新。**
  原因是容量策略变化不应销毁 Manager 或取消已提交的 GPU Copy；缩容只影响后续准入更安全。
- **Renderer 资源与逐通道 Readback 指标进入会话 metadata。**
  原因是只提供查询 API 无法在一次数据集采集结束后追溯拥塞来源；现在会话文件保留容量、拒绝/失败、资源复用及延迟快照。
- **调试保存增加预热捕获和 Shader 编译等待。**
  原因是命令行编辑器首次捕获可能早于验收材质 Shader 完成，导致棋盘格回退材质；这一同步等待只存在于 Debug Save，不进入正式逐帧路径。
- **增加无 DefaultPawn/HUD 的 `SensorSimulationGameMode`。**
  原因是默认 Pawn 曾进入 RGB 画面并形成无关的半圆形凸起。

#### R13-R14 真正的 32 位 Instance 路径（已完成）

#### 为什么要这样做

`CustomStencil` 只有 8 位，如果直接复用，会把 `0x01020304` 这样的 InstanceId 截断为 `4`。因此 Instance 必须使用独立的整数渲染路径。

#### 当前如何做

- `USemanticObjectComponent` 分别保存 SemanticId 和 uint32 InstanceId，并把每个图元注册到 `FInstanceCaptureRegistry`。
- `FInstanceCaptureTarget` 持有原生 `PF_R32_UINT` 纹理，因为当前 UE 5.7 环境中的 `UTextureRenderTarget2D` 无法提供所需的整数目标路径。
- 普通 SceneCapture 目标负责建立 View 和可见性集合；随后在 Tonemap 扩展点调用 `AddDrawDynamicMeshPass`，把结果绘制到独立整数目标。
- 每次绘制都显式绑定完整 InstanceId 和捕获 View 的 `TranslatedWorldToClip` 矩阵。这样可以避免后处理材质通道读取到不同的隐式 `ResolvedView` 静态槽位。
- 绘制通道使用 `CF_DepthNearOrEqual` 重建当前 Pass 独立的反向 Z 深度，从而保留最近的可见不透明表面，同时不借用池化 SceneDepth。
- 无输入像素着色器向 `SV_Target0` 写入一个 `uint`，不执行过滤、Gamma、Tonemap 或通道编码。
- Readback 会去除 GPU RowPitch 填充，生成紧密排列的小端 uint32 像素。协议要求 `R32Uint + Data + Identifier + 4 字节/像素`，Writer 按 ChannelGuid 导出 `instance_u32_<ChannelGuid>.bin`。

#### 相较旧架构修改了什么及原因

Instance 不再被拒绝，也不再使用 RGBA8 或 CustomStencil 表示。显式矩阵和静态 Uniform 绑定还使 D3D11/D3D12 的 PSO 契约保持确定。

#### R15 阶段 A（已完成，2026-08-04）

- Masked 绘制保留源材质并执行 UE 的材质裁剪路径，因此 Opacity Mask 孔洞保持为背景，WPO 仍能影响几何体。
- Opaque 绘制继续使用默认材质快速路径。
- Registry 会显式排除 Nanite 和 Translucent 图元，并说明 ISM/HISM 当前只提供组件级身份。
- 普通非 Nanite SkeletalMesh 继续通过动态网格路径处理。

这样做的原因是特殊对象并不共享同一套可见性契约。经典 Mesh Pass 可以精确实现 Masked 覆盖，但如果静默接受 Nanite、Translucent 或 ISM/HISM 逐实例语义，就会生成外观看似合理、实际错误的真值。

当前流程为：

`RegisterPrimitive → 特殊对象分类 → 支持项进入 Registry／不支持项显式排除 → 可见 MeshBatch → 默认 Opaque 材质／源 Masked 材质 → Pass 独立深度 + PF_R32_UINT`

#### 下一步可以怎么优化

R15 阶段 B 当前为**部分完成（2026-08-07，ISM/HISM 与 Nanite 已闭环）**：

- [x] 已为 Actor 与 ISM/HISM 内部实例预留连续 ID 区间，Payload 合法值校验覆盖整个区间。
- [x] 官方 UE 5.7.2 Renderer 在后处理调用栈内借出 `FInstanceCullingManager`；Instance Pass 使用 `AddSimpleMeshPass` 复用正式 GPU Scene Primitive-ID 流。
- [x] Shader 同时处理 `USE_INSTANCING` 与 `USE_INSTANCE_CULLING`，两个 ISM 内部实例的不同 32 位 ID 已升级为强制断言，并在 D3D11/D3D12 通过。
- [x] 半透明产品策略固定为“忽略透明表面，让其后的最近受支持不透明表面写标签”；需要标注玻璃本体时使用不透明代理几何体。
- [x] Nanite 路径利用扩展上下文中的 `NaniteRasterResults`，在 VisBuffer 存活阶段解码 `PrimitiveId/RelativeId` 并输出稳定 InstanceId；D3D12 自包含真实像素断言通过。
- [ ] 独立 HISM、Masked foliage、SkeletalMesh 与半透明遮挡的完整跨 RHI 特殊对象矩阵仍需补齐。

详细证据和后续接入方案见 ImplementationRoadmap.md 的“R15 阶段 B 实施审计”。

#### 验收状态

D3D11 和 D3D12 已分别通过真实 PIE/GPU 大 ID 生命周期测试、带 RowPitch 填充的 R32Uint 转换测试和 ExportService Writer 生命周期测试。证据位于：

- `Saved/Acceptance/R13_R14_Instance32/Run30_FinalD3D12`
- `Saved/Acceptance/R13_R14_Instance32/Run31_FinalD3D11`
- `Saved/Acceptance/R15_SpecialObjects/UE572_OfficialRenderer/D3D12_ISM_RequiredAssertion.log`
- `Saved/Acceptance/R15_SpecialObjects/UE572_OfficialRenderer/D3D11_ISM_RequiredAssertion.log`
- `Saved/Logs/R15_NanitePixelDiscardFix_DX12.log`

#### 下一步做什么/如何优化

- [x] RenderTarget 查询、Payload 路由、文件命名和预期图像通道已从 `ChannelType` 寻址升级为 `ChannelGuid` 寻址，同一 `ChannelType` 多配置已经放开。
- [x] Camera Adapter 持有稳定 SensorGuid；Calibration、Renderer 指标和异步 Payload 均以该身份关联，Rig 的 SensorName 可独立热改。

### 4.2 Semantic 无颜色后处理污染流程

#### 为什么要这样做

Semantic ID 是离散整数，不是显示颜色。Tonemap、Gamma、TAA/FXAA、Bloom、Motion Blur 和双线性采样都可能产生不存在的中间标签。

#### 当前如何做

```mermaid
flowchart TD
    Object["USemanticObjectComponent"]
    Stencil["CustomStencil 8 bit"]
    Capture["Semantic SceneCapture<br/>关闭 AA/Bloom/DoF/曝光/运动模糊"]
    TargetRegistry["Semantic Target Registry"]
    Extension["FSemanticCaptureViewExtension"]
    Shader["FSemanticCapturePS"]
    Target["Linear RGBA8<br/>R=ID G=0 B=0 A=255"]

    Object -->|ApplySemanticRenderState| Stencil
    Capture --> TargetRegistry
    TargetRegistry --> Extension
    Stencil --> Shader
    Extension -->|Tonemap 回调| Shader
    Shader --> Target
```

1. `USemanticObjectComponent` 给所属 Actor 的全部 `UPrimitiveComponent` 启用 CustomDepth，并写入 8 位 CustomStencil。
2. Camera Rig 创建 Semantic RenderTarget 后，把对应 `FRenderTarget*` 注册到线程安全集合。
3. View Extension 通过 `ViewFamily->RenderTarget` 是否已注册来识别 Semantic View。
4. 只在 Tonemap Pass 注册 `RenderSemanticLabels`。
5. Global Shader 使用整数像素位置读取 CustomStencil，不采样 SceneColor。
6. Shader 按有效 `ViewRect` 而不是池化纹理 Extent 计算像素坐标。
7. 输出固定为 `R=SemanticId/255,G=0,B=0,A=1`，落到线性 RGBA8 后恢复为精确标签字节。

#### 相较旧架构修改了什么及原因

- **Semantic View 身份从 `CaptureSource == FinalToneCurveHDR` 改为显式 RenderTarget 注册表。**
  原因是 CaptureSource 只描述渲染内容和时序，其他业务也可能使用相同枚举；把它当身份哨兵会误接管其他 Capture。
- **不再依赖 `ProfilingEventName` 传播到渲染线程。**
  原因是 UE 5.7 实测没有稳定传播到 `ViewFamily`；ProfilingEventName 现在只用于调试标识。
- **Shader 输入签名保留 `TEXCOORD0` 与 `SV_POSITION` 的寄存器顺序。**
  原因是 D3D12 PSO 对 VS/PS 输入签名兼容性更严格。
- **像素位置使用有效 ViewRect。**
  原因是奇数分辨率下 RDG 池化纹理可能大于实际 Capture Viewport，使用 Extent 会发生边缘错位。

#### SemanticId 非法值策略

- 背景固定为 0。
- 图像对象标签只允许 1..255。
- 非法值不再静默 Clamp。
- 非法对象退出 Semantic 图，但完整 SemanticId 仍保留给 LiDAR 和 Ground Truth。
- Runtime 校验 Semantic 图中每个像素必须属于当前注册 ID 集合，并满足 `G=0,B=0,A=255`。

改变原因：Clamp 会把不同非法类别合并成 0 或 255，生成“看起来合法但语义错误”的数据，比显式拒绝更危险。

#### 下一步做什么/如何优化

- R15：覆盖 Nanite、Masked、半透明、植被、ISM/HISM、SkeletalMesh 和多 Primitive Actor。
- 把 Runtime 全图合法性扫描移到 SIMD、TaskGraph 或 GPU Reduction。
- 日志记录第一个非法像素坐标、RGBA 和合法 ID 集合。
- 明确半透明对象是否写 Semantic、写前景标签还是保持背景的产品策略。

### 4.3 异步 GPU Readback

#### 为什么要这样做

同步 `ReadPixels()` 会让游戏线程等待渲染线程和 GPU，破坏固定频率采集。正式管线必须使用 Fence 驱动的异步 GPU Copy；同步 PNG/EXR 导出只能用于人工调试。

#### 当前如何做

```mermaid
flowchart TD
    Enqueue["Enqueue"]
    Validate{"类型/尺寸/Gamma/Format/容量合法？"}
    Reserve["原子预留 Pending 容量"]
    RT["排队 Render Command"]
    Acquire["从池获取或创建 Readback"]
    Copy["FRHIGPUTextureReadback::EnqueueCopy"]
    Poll["PollCompleted"]
    Global["Renderer 全局 bPumpQueued<br/>活跃 Manager 弱引用注册表"]
    Batch["一条 Render Command<br/>批量 Pump 全部 Manager"]
    Ready{"IsReady？"}
    Lock["Lock RowPitch/Height"]
    Convert["RGBA/BGRA 或 Depth 转换"]
    Unlock["Unlock + 回收到对象池"]
    Payload["紧密 FImagePayload"]

    Enqueue --> Validate
    Validate -->|否| Reject["拒绝并计数"]
    Validate -->|是| Reserve --> RT --> Acquire --> Copy
    Poll --> Global --> Batch --> Ready
    Ready -->|否| Wait["下次 Poll 再检查"]
    Ready -->|是| Lock --> Convert --> Unlock --> Payload
```

线程边界：

- 游戏线程：验证 UObject 属性、原子预留容量、提交命令、消费 Completed Queue。
- 渲染线程：取得 RHI Texture、提交 GPU Copy、检查 Fence、Lock/Unlock、执行当前 CPU 格式转换。
- CPU Payload：拥有独立 `TArray<uint8>`，Unlock 后不再依赖 staging resource。

格式转换：

- `PF_R8G8B8A8`：逐行紧密复制。
- `PF_B8G8R8A8`：逐像素交换 R/B，输出统一 RGBA8。
- `PF_R32_FLOAT`：逐行复制并从厘米转米。
- `PF_A32B32G32R32F`：只提取逻辑 R，并从厘米转米。
- 所有路径都检查 `RowPitchInPixels >= Width` 和 BufferHeight。

Readback 对象池：

- 以 `ImageSize + PixelFormat` 匹配可复用对象。
- Fence 完成且 Unlock 后回收到池。
- 不会把不同尺寸或格式的 staging resource 混用。

当前指标分三层：

- Manager 聚合：Capacity、Pending、PeakPending、Enqueued、Completed、Rejected、Failed、Created/ReusedReadbackResources。
- `SensorGuid + PayloadType` 通道：上述容量与结果计数，另含 Delivered、平均/最大 GPU Readback 延迟、平均/最大端到端交付延迟。
- Renderer 全局 Pump：当前/峰值注册 Manager 数、PumpCommandCount、累计访问 Manager 数和单次最大批量。

`UCameraRigComponent::GetImageReadbackStats()` 和 `GetImageReadbackChannelStats()` 向上层提供快照；`FImageReadbackManager::GetGlobalPumpStats()` 提供全局协调器快照。Runtime Camera Adapter 每秒和退出前把 Rig 快照登记到 Dataset Session。

#### 相较旧架构修改了什么及原因

- **转换逻辑从 Manager 拆到 `ImageReadbackConversion.*`。**
  原因是 RowPitch、BGRA/RGBA 和 Depth 单位转换需要脱离 GPU 做自动化测试。
- **增加对象池。**
  原因是高频采集时反复创建 staging/readback resource 会造成 RHI 分配抖动。
- **把每 Manager 的 `bPumpQueued` 提升为 Renderer 全局协调器。**
  原因是多台 Camera 各自 Poll 时，单 Manager 合并仍会产生多条渲染命令；现在任一 Poll 只触发一条命令，并通过弱引用快照批量检查全部活跃 Manager。
- **指标从 Manager 总量扩展为 `SensorGuid + PayloadType`。**
  原因是总量只能说明“某处拥塞”，无法定位具体相机和 RGB/Semantic/Depth 通道；延迟也必须在 Enqueue、GPU 完成和游戏线程领取三个时刻分别记录。

#### 当前边界

- 全局 Pump 目前仍由 `PollCompleted` 驱动，而不是 Renderer Ticker 主动推进；只要 Camera Adapter 正常 Tick 就会持续工作。
- 大块像素转换仍发生在渲染线程。
- Camera Rig 指标已周期性写入 Dataset metadata；全局 Pump 指标目前仍只提供进程级查询，尚未写入会话文件。
- Destructor 通过共享状态保证排队命令不会访问已销毁 Manager；D3D12 自动化已覆盖组件反复注册、PIE Stop、关卡切换和带 Pending Readback 退出。

#### 下一步做什么/如何优化

- R8/R10 基础闭环及 Camera Rig 指标落盘已完成；下一步评估是否把全局 Pump 指标也写入会话，并改为 Renderer Ticker 主动 Pump。
- R11 已完成基础闭环；下一步扩充多相机并发销毁、模块卸载和 RHI 设备重建等更强压力场景。
- 评估把 CPU 格式转换移到 TaskGraph/Worker，但必须先复制或映射出不依赖 Lock 生命周期的数据。

---

## 5. SimulationRuntime：调度、适配、校验、聚合与导出

### 5.1 运行时设置与时钟

#### 为什么要这样做

实时预览追求不卡顿；确定性数据集采集追求稳定顺序和可复现性。两者在背压和是否允许同时存在多个 Pending Frame 上需要不同策略。

#### 当前如何做

`USimulationSettings` 提供：

- `SimulationMode`：Realtime 或 DeterministicDataset。
- `FixedStepSeconds`：固定采集间隔。
- `FrameTimeoutSeconds`：等待模态到齐的超时时间。
- `RandomSeed`：确定性模式随机种子。
- `MaxPendingFrames`：Export Queue 容量。
- `DatasetRoot`：数据集根目录。

Subsystem 使用累加器把不稳定的游戏帧 DeltaTime 转为固定采样间隔。

- Realtime：允许持续请求新帧。
- DeterministicDataset：只有 FrameAssembler 没有 Pending Frame 时才请求下一帧。
- 确定性模式初始化 `Rand` 与 `SRand` 种子。

#### 相较旧架构修改了什么及原因

- **固定步长、随机种子、帧超时和导出容量已经进入项目设置。**
  原因是这些参数会直接改变数据集可复现性和背压行为，不应散落为硬编码。
- **确定性模式会等待当前帧聚合完成。**
  原因是允许多帧重叠会让 GPU/LiDAR 完成顺序影响数据集帧序列。

#### 下一步做什么/如何优化

- 当前确定性时钟仍由游戏 Tick 驱动，不是完全独立的仿真调度器。
- `BlockDatasetClock` 会在 Export Queue 满时阻塞调用线程；后续应让时钟显式暂停，而不是用 Sleep 轮询。
- 相对 `DatasetRoot` 当前可能受进程工作目录影响；应统一解析为 Project/Saved 或要求绝对路径。
- 为设置变更增加运行时快照，避免采集中途修改 CDO 造成同一 Session 语义变化。

### 5.2 传感器基类与 Camera Adapter

#### 为什么要这样做

Camera 和 LiDAR 的采集方式不同，但 Subsystem 需要统一注册、查询能力并下发 `FCaptureRequest`。

#### 当前如何做

- `USimSensorComponentBase::BeginPlay/EndPlay` 自动注册和注销。
- `GetPayloadTypes` 报告传感器能力。
- `RequestCapture` 是统一采集入口。
- Camera Adapter 自动查找同 Actor 的 `UCameraRigComponent`。
- Camera Adapter 为每条实际启用的图像通道分别注册标定参数，并按 `ChannelGuid` 更新同一通道。
- Camera Adapter 每 Tick 清空 Readback Completed Queue，并移动提交给 Subsystem。

#### 相较旧架构修改了什么及原因

- **Camera 能力已从 RGB/Semantic 扩展为 RGB/Semantic/Depth。**
  原因是 Depth 已进入正式 Readback 和 Runtime 校验路径。
- **Camera Adapter 会向 DatasetSession 注册 Calibration。**
  原因是数据集消费者需要从像素坐标恢复射线或三维位置。

#### 下一步做什么/如何优化

- [x] RenderTarget、Readback/Payload 路由、FrameAssembler 图像预期和文件命名已统一使用 ChannelGuid；同一 ChannelType 多配置已解除运行时过滤。
- Camera Rig 缺失时输出明确错误并禁用 Adapter。
- [x] `RequestCapture` 已返回 `Accepted/Busy/Rejected`，Readback 队列满或资源无效会立即反馈给 FrameAssembler。
- [x] 每个实际启用的相机通道已使用独立 Calibration，并通过 `SensorGuid + ChannelGuid` 登记和热更新。
- 使用集中完成通知替代每个 Camera Adapter 每 Tick Poll。

### 5.3 Semantic Registry 与图像校验

#### 为什么要这样做

Shader 正确不代表最终 Payload 一定正确。Gamma、RHI 通道顺序、尺寸、RowPitch 或非法标签都可能在 GPU Copy 或协议边界破坏数据。

#### 当前如何做

- Registry 为运行时语义对象分配会话内 `InstanceId`。
- Registry 保存 Actor → Semantic Component 弱引用。
- Registry 收集背景 0 和当前有效 8 位 SemanticId。
- Runtime 在图像进入 FrameAssembler 前验证格式、尺寸、字节数和 RowStride。
- Semantic 额外执行逐像素合法 ID 与 RGBA 约束检查。
- Depth 必须是 `R32Float + Data + Meters`。

#### 相较旧架构修改了什么及原因

- **非法 SemanticId 从 Clamp 改为显式拒绝图像标签。**
  原因是静默 Clamp 会制造合法外观的错误类别。
- **校验从“所有图像都是 RGBA8”扩展到按模态验证。**
  原因是 Depth 同样是 4 字节/像素，但其含义是 float32 而不是四个颜色字节。

#### 下一步做什么/如何优化

- Registry 主动清理失效弱引用。
- 为 Instance 图维护合法的 uint32 InstanceId 集合。
- 校验失败日志增加第一个错误像素坐标和值。
- 将全图扫描移出游戏线程。

### 5.4 LiDAR

#### 为什么要这样做

一次执行完整扫描的所有同步 Trace 会阻塞单帧，因此扫描必须分批推进；但完整扫描仍必须回到同一个 FrameId。

#### 当前如何做

```mermaid
flowchart LR
    Pattern["RebuildPattern"]
    Request["RequestCapture"]
    Batch["TraceBatch<br/>RaysPerTick"]
    Label["读取 Semantic/Instance"]
    Final["FinalizeScan"]
    Submit["Subsystem::SubmitLidar"]
    Assemble["FrameAssembler::AddLidar"]

    Pattern --> Request --> Batch --> Label
    Label -->|未完成| Batch
    Label -->|完成| Final --> Submit --> Assemble
```

- `RebuildPattern` 预生成局部射线方向。
- `RequestCapture` 初始化带 Header 的 ActiveScan。
- `TraceBatch` 每 Tick 只执行 `RaysPerTick` 条射线。
- 命中点转换为传感器局部米制坐标。
- 命中 Actor 的 SemanticId/InstanceId 写入点。
- `FinalizeScan` 验证 `CompletedRayCount == ExpectedRayCount`，随后提交 Subsystem。

#### 相较旧架构修改了什么及原因

- **`FinalizeScan → SubmitLidar` 已闭环。**
  原因是只广播 Delegate 无法让 FrameAssembler 收到 LiDAR Payload。

#### 下一步做什么/如何优化

- 当前传感器变换使用 Owner Actor Transform；应改用组件世界变换，支持一个 Actor 上安装多个 LiDAR。
- 使用 UE Async Trace 或任务化批处理。
- 增加运动畸变、噪声、漏检、多回波和材质反射率模型。
- `FinalizeScan` 当前移动 `ActiveScan` 后再广播同一对象；应调整顺序或广播不可变副本，避免监听者收到 moved-from 数据。

### 5.5 FrameAssembler

#### 为什么要这样做

不同模态异步完成；整帧只有在全局预期模态和每个已注册传感器的预期模态都满足时才能发布。

#### 当前如何做

- `BeginFrame` 创建待完成 Packet 并记录创建时间。
- `RegisterSensor` 保存 `FrameId → SensorGuid → SensorName + Expected/Completed`。
- `RequestCapture` 用 `Accepted/Busy/Rejected` 表示接纳结果；非 Accepted 通过 `FailFrame` 立即终止整帧。
- Camera 请求额外携带 `ExpectedImageChannels`；`AddImage` 按 `SensorGuid + ChannelGuid` 校验预期类型和完成状态，同一 RGB 的多条配置不会被一个模态位提前满足。
- `AddLidar` 继续按 SensorGuid + LiDAR 模态更新传感器状态。
- `AddGroundTruth` 完成真值模态，同一 FrameId 的重复真值也会被拒绝。
- `CheckAndEnqueueComplete` 先检查整帧和逐传感器状态，再用 `EnqueuedCompleteFrames` 保证 FrameId 只入完成队列一次。
- `PurgeTimedOutFrames` 清理超时帧、记录缺失状态，并将 FrameId 标记为 `TimedOut`。
- `PopCompleteFrame` 使用移动语义发布，将 FrameId 标记为 `Completed` 后清理关联状态。
- 最近 1024 个 `Completed/Failed/TimedOut` FrameId 构成有限终态历史；迟到 Payload 直接丢弃并计数。

#### 相较旧架构修改了什么及原因

- **增加按传感器完成状态。**
  原因是单一位掩码无法表达“两台相机各需要一张 RGB”。
- **完成键从 SensorName 改为 SensorGuid。**
  原因是名称允许重复和热改；持久 GUID 才能在请求、GPU Readback、LiDAR、Calibration、指标与导出之间保持同一身份。
- **图像文件始终使用完整 ChannelGuid 后缀。**
  例如 `rgb_<32位ChannelGuid>.png`、`depth_meters_f32_<32位ChannelGuid>.bin`；文件身份不依赖 ChannelType、SensorName 或同模态数量，配置增删和改名不会改变剩余通道的文件名。
- **增加 Timeout 与统计。**
  原因是读回拒绝、传感器 Busy 或缺失组件都可能让帧永远不完整。

#### 下一步做什么/如何优化

- [x] 使用稳定 Sensor GUID，SensorName 只作为显示名称；同名传感器自动化回归已覆盖。
- [x] 重复 Payload 与重复完成队列项已分别由逐传感器完成状态和 `EnqueuedCompleteFrames` 阻止。
- [x] `Accepted/Busy/Rejected` 已与 `FailFrame` 闭环；Busy、Rejected、Timeout、Duplicate、Late 均独立统计并写入 `metadata.json`。
- [x] 迟到策略已定义：终态历史命中的 Payload 一律丢弃并增加 `LatePayloads`；历史按 FIFO 保留最近 1024 帧。
- 增加 Pending Frame 上限和更细的人工取消原因。
- `MissingModalities` 日志补充 Depth 与 Instance。

### 5.6 ExportService 与 DatasetSession

#### 为什么要这样做

PNG 编码和磁盘 IO 的耗时与 GPU 采集不同步，不能占用游戏线程。数据集还需要统一目录、标定、会话统计和可解释的文件协议。

#### 当前如何做

```mermaid
flowchart LR
    Packet["完整 FFramePacket"]
    Queue["有界 MPSC Queue"]
    Policy["RejectNewest / DropOldest / BlockDatasetClock"]
    Worker["FRunnable Worker"]
    FrameDir["frame_XXXXXX"]
    Session["metadata.json<br/>calibration.json"]

    Packet --> Policy --> Queue --> Worker --> FrameDir
    Worker --> Session
```

Export Worker：

- `Start` 创建输出目录并启动低优先级线程。
- `Run` 消费完整帧，队列为空时短暂休眠。
- `Stop` 等待 Worker 结束；退出前排空队列。
- 统计成功和失败写出帧数。

当前文件：

| 文件 | 内容 |
|---|---|
| `rgb.png` | RGBA8 Payload 转为 PNG Writer 所需 BGRA 后编码 |
| `semantic.png` | 精确标签 RGBA8 PNG |
| `depth_meters_f32.bin` | 紧密 little-endian float32 米 |
| `lidar.bin` | 每点 float32 x/y/z/intensity，共 16 字节 |
| `groundtruth.json` | InstanceId、SemanticId、位姿、速度和包围盒 |
| `frame_info.json` | FrameId、SequenceId、时间戳和各类数据数量 |
| `metadata.json` | Session、模式、随机种子、帧统计、坐标约定和版本 |
| `calibration.json` | 相机尺寸、内参和 SensorToEgo |

背压策略：

- Realtime 默认 `RejectNewest`。
- `DropOldest` 已有实现但当前 Subsystem 不选择它。
- DeterministicDataset 使用 `BlockDatasetClock`。

#### 相较旧架构修改了什么及原因

- **ExportService 已从骨架变为完整后台 Worker。**
  原因是第一轮正式链路需要验证 RGB、Semantic、Depth 真正落盘。
- **DatasetSession 管理会话目录、标定和 metadata。**
  原因是单独的帧文件不足以解释坐标系、相机参数和采集配置。
- **Subsystem Deinitialize 先 Stop Worker 再写 Session Metadata。**
  原因是元数据统计必须在所有已排队帧完成写出之后生成。

#### 下一步做什么/如何优化

- 使用临时文件加原子 Rename，避免进程异常时留下看似完整的半文件。
- 检查每个 Writer 的返回值；`frame_info.json` 当前返回值未计入整帧成功状态。
- 为 Depth/Instance 文件增加格式版本、字节序和无效值说明。
- LiDAR 当前导出不包含 Point 中已有的 SemanticId、InstanceId 和 RelativeTime，需要定义扩展格式或伴随文件。
- 把 Export 指标写入 metadata，并记录 Reject/Drop 的 FrameId。
- 避免 `BlockDatasetClock` 在游戏线程中忙等待。

---

## 6. 关键对象关系

```mermaid
classDiagram
    class USimulationSubsystem {
        +Tick()
        +SubmitImage()
        +SubmitLidar()
        -RequestFrame()
        -ValidateImagePayload()
    }
    class USimSensorComponentBase {
        +GetPayloadTypes()
        +RequestCapture()
    }
    class USimCameraSensorComponent
    class USimLidarSensorComponent
    class UCameraRigComponent {
        +SubmitCapture()
        +PollCompletedImage()
    }
    class FImageReadbackManager {
        +Enqueue()
        +PollCompleted()
        +GetStats()
    }
    class FFrameAssembler
    class FSemanticRegistry
    class FExportService
    class FDatasetSession

    USimSensorComponentBase <|-- USimCameraSensorComponent
    USimSensorComponentBase <|-- USimLidarSensorComponent
    USimulationSubsystem o-- USimSensorComponentBase
    USimulationSubsystem *-- FFrameAssembler
    USimulationSubsystem *-- FSemanticRegistry
    USimulationSubsystem *-- FExportService
    USimulationSubsystem *-- FDatasetSession
    USimCameraSensorComponent --> UCameraRigComponent
    UCameraRigComponent *-- FImageReadbackManager
    FFrameAssembler --> FExportService
```

---

## 7. 线程、队列和所有权

| 对象/数据 | 线程 | 当前所有权 |
|---|---|---|
| `USimulationSubsystem` | 游戏线程 | World Subsystem |
| Sensor Components | 游戏线程 | Actor/Component |
| Scene Capture 配置和动态组件 | 游戏线程 | Camera Rig |
| Semantic Target 注册集合 | 游戏线程写、渲染线程读 | 全局集合 + `FRWLock` |
| Global Shader / RDG Pass | 渲染线程 | View Extension |
| Pending/Reusable `FRHIGPUTextureReadback` | 渲染线程 | Readback Shared State |
| Completed 图像队列 | 渲染线程生产、游戏线程消费 | MPSC |
| `FImagePayload::Bytes` | 创建后移交游戏线程 | 独立 CPU 内存 |
| `FFrameAssembler` | 游戏线程 | Subsystem 成员 |
| Export Pending Queue | 游戏线程生产、Worker 消费 | MPSC |
| 文件 Writer | Export Worker | `FExportService::FImpl` |
| Dataset Session | 游戏线程 | Subsystem 成员 |

### 为什么要明确所有权

最危险的不是一次同步操作，而是排队命令在 UObject 或 Manager 销毁后仍访问旧地址。当前 Readback 使用线程安全共享状态延长命令依赖数据的寿命；Semantic Target 在销毁资源前先注销。

### 当前边界与下一步优化

- 为游戏线程/渲染线程专属函数增加 `check(IsInGameThread())` 或 `check(IsInRenderingThread())`。
- R11 生命周期测试覆盖 PIE Stop、关卡切换、组件反复注册和带 Pending Readback 退出。
- Export Worker 不应访问 UObject；当前只消费值类型 Packet，保持这一边界。
- 评估关闭时是否需要显式 Flush Render Commands，必须避免在普通逐帧路径引入同步等待。

---

## 8. 验收、自动化与兼容状态

### 为什么要单独记录

“代码存在”不等于“跨 RHI、非规则分辨率和长序列下正确”。架构文档需要区分实现状态与验证范围。

### 当前验证结果

- `SensorSimulation` Automation：12 项通过、0 项失败。
  - 坐标转换。
  - RGBA RowPitch。
  - BGRA → RGBA。
  - 非法 RowPitch 拒绝。
  - R32F Depth 厘米 → 米。
  - RGBA32F 提取逻辑 R → 米。
  - SemanticId 合法范围。
  - Camera Rig 反复注册、PIE 中带待读回任务停止运行，以及关卡切换。
  - Export Worker Stop、重复 Stop、Restart 和退出前 Drain。
  - 两个 Manager 的 Poll 合并为一条全局 Pump 命令。
  - Front RGB 与 Rear Semantic 拒绝指标按通道独立归属。
- D3D11/D3D12：
  - 641×359 Semantic 均通过。
  - 解码后的 RGBA 像素逐像素一致。
- RGB/Semantic：
  - 红、绿、蓝、白 Unlit Cube。
  - SemanticId 为 10、20、100、200。
  - RGB 与 Semantic 区域对齐。
- Depth：
  - Debug EXR 有效。
  - 正式 `depth_meters_f32.bin` 有效。
- 稳定性：
  - 正式异步链路生成 1,067 帧。
  - 第 1 帧和第 1000 帧的 Depth 与 Semantic 哈希一致。
  - 日志未发现 Readback 拒绝、ViewRect 失败、无效 Buffer 或队列满。

主要证据位于：

`Saved/Acceptance/RendererOwner_20260723`

全局 Pump 与通道指标增量证据位于：

`Saved/Acceptance/RendererOwner_20260725/08_GlobalPumpMetrics`

全局 Pump 实现闭环及 R17/R18 热更新、资源复用证据位于：

`Saved/Acceptance/RendererOwner_20260725/09_HotReloadResourceReuse`

稳定 ChannelGuid、逐通道标定、容量热更新和指标落盘证据位于：

`Saved/Acceptance/RendererOwner_20260725/11_ChannelIdentityMetrics`

其中聚焦自动化结果为 Readback 8/8、DatasetSession 2/2、D3D12 CameraRig 1/1，均为 0 Warning / 0 Failed；目录同时保存 `SampleChannelCalibration.json` 和 `SampleRendererMetadata.json`。

### 当前验证边界

- D3D12 Semantic 由启动脚本和外部像素分析自动完成，但尚未全部注册为 UE Automation Framework 的 GPU 测试。
- D3D11/D3D12 矩阵目前重点覆盖 Semantic；RGB/Depth 尚未形成同等完整的组合矩阵。
- Instance 生命周期已经在 D3D11 和 D3D12 上实现自动化；多相机销毁、模块卸载和 RHI 重置压力测试仍未完成。
- 特殊渲染对象仍缺自动化。

### 下一步做什么/如何优化

1. 把外部验收脚本收口为单入口测试命令和机器可读汇总报告。
2. 建立 `RHI × Resolution × Modality × SceneCase` 参数矩阵。
3. 扩充跨 RHI 生命周期压力场景，并增加遮挡、运动和特殊对象用例。
4. 把 Readback/Export/FrameAssembler 指标写入每次验收报告。

---

## 9. 当前完成度与推荐顺序

### 已完成

- R3：清理验收场景。
- R4～R6：RGB 正确性、通道顺序和 Semantic 对齐。
- R7：显式 Semantic View 标识。
- R8：Renderer 全局 Readback Pump。
- R10：`SensorGuid + ChannelGuid` 通道指标、GPU/交付延迟与全局 Pump 指标。
- R11：Camera Rig/Readback 与 Export Worker 生命周期基础自动化。
- R9：Readback 对象池。
- R17：Channels/Resolution/FOV/Gamma/Readback Capacity 配置热更新、稳定 ChannelGuid 与逐通道 Calibration 同步。
- R18：Capture/RenderTarget 选择性复用、退休 Target 生命周期保护、资源指标及会话 metadata 落盘。
- ChannelGuid 寻址：同类型多通道的 RenderTarget、Readback/Payload、FrameAssembler 预期项、文件名和元数据已闭环。
- R12：Depth Channel。
- R13-R14：真正的 32 位 Instance Mesh Pass、R32Uint 读回与协议、Writer，以及跨 RHI 生命周期验收。
- R16：SemanticId 非法值策略。
- LiDAR → Subsystem。
- 按传感器 FrameAssembler、Timeout。
- Export Worker、Dataset Session 和正式文件 Writer。

### 基本完成但仍需收口

- R1：D3D12 Semantic 脚本化自动验收，尚未完全纳入 UE Automation Framework。
- R2：D3D11/D3D12 Semantic 矩阵，尚未覆盖所有模态。

### 尚未完成

- R15 Phase B：ISM/HISM 逐内部实例 ID、Nanite 专用路径与 Translucent 产品规则已完成；独立 HISM、Masked foliage、SkeletalMesh、透明遮挡等完整自动化矩阵仍待收口。

### 推荐实施顺序

```mermaid
flowchart LR
    A["1. R15 阶段 B 收口<br/>特殊对象跨 RHI 矩阵"]
    B["2. R1/R2<br/>完整 GPU 验收矩阵"]
    C["3. 发布加固<br/>压力测试与报告"]

    A --> B --> C
```

顺序原因：

1. R13-R14 已完成整数模态基础，R15 阶段 A 已支持 Masked 并建立显式回退规则，因此阶段 B 是当前第一优先级。
2. 使用特殊对象矩阵同时约束 Semantic 与 Instance 的行为。
3. 最后把 RHI、分辨率、模态和特殊对象组合固化成持续回归矩阵。

---

## 10. 最小运行时装配

Camera Actor（相机角色）：

- `UCameraRigComponent`
- `USimCameraSensorComponent`
- Channels 中启用 RGB、Semantic、Depth 和/或 Instance。

Semantic Actor（语义对象角色）：

- 一个或多个可渲染 `UPrimitiveComponent`
- `USemanticObjectComponent`
- `bRenderToSemanticCapture=true`
- 图像 SemanticId 使用 1..255；0 保留为背景

LiDAR Actor（激光雷达角色）：

- `USimLidarSensorComponent`
- 合理设置 Channels、HorizontalSamples、RaysPerTick 和量程

项目设置：

- `r.CustomDepth=3`
- `GlobalDefaultGameMode=/Script/SimulationRuntime.SensorSimulationGameMode`
- 在 Sensor Simulation Settings 中配置时钟、帧超时、导出容量和 DatasetRoot

正式数据流：

- RGB、Semantic、Depth 和 Instance 均使用异步 `FRHIGPUTextureReadback`；其中 Instance 读取原生 R32Uint 目标。
- `SaveRgbDebugImage`、`SaveSemanticDebugImage`、`SaveDepthDebugImage` 会同步等待，仅用于人工调试和验收，不能放入逐帧采集路径。
- Instance 是正式的逐像素 R32Uint 模态；Ground Truth 和 LiDAR 继续使用同一套完整 InstanceId 命名空间。
