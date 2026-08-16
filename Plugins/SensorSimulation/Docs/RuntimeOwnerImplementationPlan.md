# Runtime/System Owner 实施计划

> 负责人：Runtime/System Owner
> 负责模块：SimulationCore + SimulationRuntime
> 起始日期：2026-07-21
> 文档用途：跟踪每个任务的完成状态、验收标准和优化方向

---

## 已完成项（队友 Render Owner 交付 + 已有代码）

- [x] SimulationSubsystem 基础框架（Tick、RequestFrame、CaptureGroundTruth）
- [x] SemanticRegistry 注册与校验
- [x] SimSensorComponentBase 传感器基类（自动注册/注销）
- [x] SimCameraSensorComponent 适配器（CameraRig ↔ Subsystem 桥接）
- [x] SimLidarSensorComponent 分批扫描（RebuildPattern、TraceBatch、FinalizeScan）
- [x] FrameAssembler 帧聚合骨架（BeginFrame、AddImage、PopCompleteFrame）
- [x] ExportService 队列骨架（有界 MPSC Queue、Start/Stop/Enqueue）
- [x] CoordinateConverter 坐标转换
- [x] LidarScanPattern 射线方向预计算
- [x] SimulationTypes 公共数据协议

---

## 任务 1：闭环 LiDAR → Subsystem 数据流

**状态**：✅ 完成
**完成日期**：2026-07-21

### 修改内容

在 `SimLidarSensorComponent::FinalizeScan` 中增加 Subsystem 提交逻辑：
- 添加 `#include "SimulationSubsystem.h"`
- 在广播委托之前，通过 `GetWorld()->GetSubsystem<USimulationSubsystem>()` 获取 Subsystem
- 调用 `Subsystem->SubmitLidar(MoveTemp(ActiveScan))` 将点云移交给帧聚合器
- `FrameAssembler::AddLidar` 已有实现，无需修改

### 注意事项

- `SubmitLidar` 使用移动语义，调用后 `ActiveScan` 为空
- `ScanCompleteDelegate` 广播的是移动后的空 Payload（已知行为）
- 外部监听者如需完整数据，应从 Subsystem 的组装帧中获取

### 涉及文件

- `Source/SimulationRuntime/Private/SimLidarSensorComponent.cpp`（已修改）
- `Source/SimulationRuntime/Private/FrameAssembler.cpp`（无需修改，逻辑已正确）

---

## 任务 2：Export Worker 后台线程 + 文件 Writer

**状态**：✅ 完成
**完成日期**：2026-07-21

### 实现内容

1. **FImpl 改为 FRunnable**：继承 FRunnable，实现 Init/Run/Stop 接口
2. **后台 Worker 线程**：创建 FRunnableThread，优先级 BelowNormal
3. **Run 循环**：Dequeue → ExportFrame；空队列等待 FEvent，由 Enqueue/Stop 唤醒
4. **退出排空**：Stop 后先排空队列中剩余帧再退出
5. **PNG Writer**：使用 IImageWrapperModule 将 RGBA8→BGRA→PNG 编码
6. **BIN Writer**：每点 4 个 float32（x,y,z,intensity）= 16 字节
7. **JSON Writer**：使用 TJsonWriter 输出 Ground Truth 和帧元数据
8. **背压策略**：RejectNewest（拒绝）、DropOldest（丢弃最旧）、PauseDatasetClock（非阻塞显式暂停）
9. **统计计数**：ExportedFrameCount、FailedFrameCount 原子计数器
10. **Subsystem 集成**：Initialize 创建并启动 ExportService，Tick 中将完整帧入队，Deinitialize 停止

### 输出目录结构

```
Saved/SensorSimulation/<SessionName>/
├── frame_000001/
│   ├── rgb.png
│   ├── semantic.png
│   ├── lidar.bin
│   ├── groundtruth.json
│   └── frame_info.json
├── frame_000002/
│   └── ...
```

### 涉及文件

- `Source/SimulationRuntime/Public/ExportService.h`（已修改）
- `Source/SimulationRuntime/Private/ExportService.cpp`（已重写）
- `Source/SimulationRuntime/Public/SimulationSubsystem.h`（已修改：添加 ExportService 成员）
- `Source/SimulationRuntime/Private/SimulationSubsystem.cpp`（已修改：集成 ExportService）

### 注意事项

- PNG 编码在 Worker 线程执行，不阻塞 Game Thread
- PauseDatasetClock 不阻塞 Subsystem::Tick；完整帧留在 FrameAssembler，独立调度器等待 Export 容量
- 像素格式转换（RGBA→BGRA）在 Worker 线程完成

### 后续优化

- 使用临时文件 + 原子 Rename 防止写入中断损坏
- 增加 .pcd 格式支持
- 建立 PNG 编码对象池减少内存分配

---

## 任务 3：FrameAssembler 多传感器完成计数

**状态**：✅ 完成
**完成日期**：2026-07-21

### 实现内容

1. **新增 FSensorFrameStatus 结构**：按传感器名记录 ExpectedPayloads 和 CompletedPayloads
2. **FrameAssembler 新增 PerSensorStatus 映射**：TMap<uint64, TMap<FName, FSensorFrameStatus>>
3. **新增 RegisterSensor 方法**：由 Subsystem 在 RequestFrame 时调用，注册每个传感器的预期模态
4. **AddImage / AddLidar 更新**：同时更新整帧位掩码和按传感器完成状态
5. **CheckAndEnqueueComplete**：先检查整帧位掩码，再遍历所有传感器确认全部完成
6. **Subsystem RequestFrame 更新**：循环中调用 FrameAssembler.RegisterSensor
7. **清理**：PopCompleteFrame 时同时移除 PerSensorStatus 条目

### 涉及文件

- `Source/SimulationRuntime/Public/FrameAssembler.h`（已重写）
- `Source/SimulationRuntime/Private/FrameAssembler.cpp`（已重写）
- `Source/SimulationRuntime/Private/SimulationSubsystem.cpp`（已修改）

### 注意事项

- 保持向后兼容：未注册传感器的帧仍通过整帧位掩码判断完成
- Ground Truth 不关联传感器名，仍通过整帧位掩码管理

---

## 任务 4：Frame Timeout + 丢帧统计与日志

**状态**：✅ 完成
**完成日期**：2026-07-21

### 实现内容

1. **FFrameAssemblerStats 结构**：TotalFrames、CompletedFrames、TimeoutFrames、FailedFrames
2. **FrameCreationTime 映射**：记录每帧创建时间，用于超时检测
3. **PurgeTimedOutFrames 方法**：遍历 Pending 帧，清理超时帧并输出详细日志
4. **超时日志内容**：FrameId、等待时长、缺失模态列表、各传感器完成状态
5. **CleanupFrame 方法**：统一清理 PendingFrames、PerSensorStatus、FrameCreationTime
6. **SimulationSettings 新增 FrameTimeoutSeconds**：默认 2.0 秒，可在项目设置中配置
7. **Subsystem Tick 调用**：每帧调用 PurgeTimedOutFrames 清理超时帧

### 涉及文件

- `Source/SimulationRuntime/Public/FrameAssembler.h`（已修改）
- `Source/SimulationRuntime/Private/FrameAssembler.cpp`（已修改）
- `Source/SimulationRuntime/Public/SimulationSettings.h`（已修改）
- `Source/SimulationRuntime/Private/SimulationSubsystem.cpp`（已修改）

---

## 任务 5：确定性固定步长模式 + Seed 控制

**状态**：✅ 完成
**完成日期**：2026-07-21

### 实现内容

1. **SimulationSettings 新增 RandomSeed**：默认 42，可在项目设置中配置
2. **Subsystem::Initialize**：DeterministicDataset 模式下调用 FMath::RandInit 和 FMath::SRandInit 设置种子
3. **FSimulationScheduler**：DeterministicDataset 忽略 Tick DeltaTime，仅在帧流水线为空且 Export 有容量时推进一个固定步
4. **背压联动**：Deterministic 模式使用 PauseDatasetClock，队列满时显式冻结时间轴且不阻塞游戏线程

### 涉及文件

- `Source/SimulationRuntime/Public/SimulationSettings.h`（已修改）
- `Source/SimulationRuntime/Private/SimulationSubsystem.cpp`（已修改）

### 后续优化

- [x] Seed 已记录到 Session Metadata。
- [x] Session 设置已使用不可变运行时快照，DatasetRoot 已稳定解析。
- 支持 Seed、模式和输出目录从命令行覆盖后再捕获快照。
- 增加暂停原因、时长和队列高水位指标。

---

## 任务 6：Dataset Session 生命周期 + Metadata

**状态**：✅ 完成
**完成日期**：2026-07-21

### 实现内容

1. **FDatasetSession 类**：管理会话生命周期（Idle → Running → Stopping → Idle）
2. **唯一目录名**：格式 `YYYYMMDD_HHMMSS_<GUID前缀>`
3. **metadata.json**：包含 session_id、时间、模式、种子、统计、坐标系、引擎版本
4. **calibration.json**：包含所有注册相机的内外参和 sensor_to_ego 变换
5. **Subsystem 集成**：Initialize 创建并启动 Session，Deinitialize 写入元数据并停止
6. **相机标定注册**：SimCameraSensorComponent::BeginPlay 自动注册标定参数
7. **FCalibration::SensorToEgo**：记录传感器相对于自车的外参变换

### 涉及文件

- `Source/SimulationRuntime/Public/DatasetSession.h`（新增）
- `Source/SimulationRuntime/Private/DatasetSession.cpp`（新增）
- `Source/SimulationRuntime/Public/SimulationSubsystem.h`（已修改）
- `Source/SimulationRuntime/Private/SimulationSubsystem.cpp`（已修改）
- `Source/SimulationRuntime/Private/SimCameraSensorComponent.cpp`（已修改）

### 输出示例

```
Saved/SensorSimulation/20260721_143052_a1b2c3/
├── metadata.json
├── calibration.json
├── frame_000001/
│   ├── rgb.png
│   ├── semantic.png
│   ├── lidar.bin
│   ├── groundtruth.json
│   └── frame_info.json
└── ...
```

---

## 任务 7：LiDAR 性能优化 + I/O 压力测试

**状态**：⏸ 待编辑器验证
**对应阶段**：阶段 4（第 4 周）

### 待验证项

1. Profile LiDAR TraceBatch 性能，确认 16×512 能达到 5Hz+
2. 压力测试 Export Queue：连续采集 1000 帧
3. 确认长时间运行无内存泄漏
4. 记录性能数据

### 验收标准

- 16×512 LiDAR 稳定 5Hz+
- Export Queue 不超过设定容量上限
- 长时间采集无持续内存增长

---

## 任务 8：外部数据校验脚本 + 配置示例文档

**状态**：✅ 完成
**完成日期**：2026-07-21

### 实现内容

1. **validate_dataset.py**：Python 校验脚本
   - 验证 metadata.json 存在和格式
   - 检查帧目录完整性（RGB + Semantic + LiDAR + GT）
   - 验证 Semantic PNG 像素标签合法性（需要 Pillow）
   - 验证 LiDAR BIN 文件大小和数据合理性
   - 验证 Ground Truth JSON 格式
   - 验证 Frame ID 连续性
   - 输出详细校验报告

2. **ConfigurationGuide.md**：配置指南文档
   - 项目设置（时钟、导出、帧、确定性）
   - LiDAR 配置参数和推荐配置
   - 相机配置和 Channel 设置
   - 语义物体配置和标准类别表
   - 运行时验证方法
   - Python 数据读取示例
   - 常见问题解答

### 涉及文件

- `Tools/validate_dataset.py`（新增）
- `Plugins/SensorSimulation/Docs/ConfigurationGuide.md`（新增）

---

## 进度跟踪

| 任务 | 状态 | 完成日期 | 备注 |
|------|------|----------|------|
| 1. LiDAR→Subsystem | ✅ 完成 | 2026-07-21 | FinalizeScan 中提交给 Subsystem |
| 2. Export Worker | ✅ 完成 | 2026-07-21 | FRunnable 后台线程 + PNG/BIN/JSON Writer |
| 3. 多传感器计数 | ✅ 完成 | 2026-07-21 | 按 SensorName+PayloadType 精确计数 |
| 4. Frame Timeout | ✅ 完成 | 2026-07-21 | 超时清理 + 详细日志 + 统计 |
| 5. 确定性模式 | ✅ 完成 | 2026-07-21 | Seed 控制 + 阻塞采样 |
| 6. Session 管理 | ✅ 完成 | 2026-07-21 | 生命周期 + metadata.json + calibration.json |
| 7. 性能优化 | ⏸ 待验证 | - | 需在编辑器中 Profile |
| 8. 校验脚本 | ✅ 完成 | 2026-07-21 | Python 校验工具 + 配置指南 |

状态：`待开始` → `进行中` → `待验收` → `完成`
