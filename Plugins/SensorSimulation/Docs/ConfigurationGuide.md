# SensorSimulation 配置指南

## 项目设置

在 UE5 编辑器中，通过 **Edit → Project Settings → Plugins → Sensor Simulation** 访问配置。

### 时钟设置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| SimulationMode | Enum | Realtime | `Realtime`：实时运行；`DeterministicDataset`：确定性模式，相同 Seed 可复现 |
| FixedStepSeconds | double | 0.05 | 采集频率的倒数（秒）。0.05 = 20Hz 采集 |

### 导出设置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| MaxPendingFrames | int | 8 | 导出队列最大积压帧数 |
| DatasetRoot | Path | (空) | 数据集输出根目录，空则使用 `Saved/SensorSimulation/` |

### 帧设置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| FrameTimeoutSeconds | double | 2.0 | 帧等待模态到齐的超时时间 |

### 确定性设置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| RandomSeed | int | 42 | 确定性模式的随机种子 |

---

## LiDAR 配置

在场景中放置 Actor，添加 `SimLidarSensorComponent` 组件后配置：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| SensorName | FName | TopLidar | 人类可读显示名称，允许重复；唯一身份由组件自动维护的 SensorGuid 提供 |
| UpdateFrequencyHz | float | 10.0 | 扫描频率（Hz） |
| bSensorEnabled | bool | true | 是否启用 |
| Config.Channels | int | 16 | 垂直通道数（线数） |
| Config.HorizontalSamples | int | 512 | 水平采样数 |
| Config.VerticalFovUpperDegrees | float | 10.0 | 垂直视场上界（度） |
| Config.VerticalFovLowerDegrees | float | -10.0 | 垂直视场下界（度） |
| Config.MinRangeMeters | float | 0.5 | 最小量程（米） |
| Config.MaxRangeMeters | float | 100.0 | 最大量程（米） |
| Config.RaysPerTick | int | 1024 | 每帧最大射线数 |
| Config.TraceChannel | CollisionChannel | Visibility | 碰撞通道 |

### 推荐配置

**高速采集（低密度）**：
- Channels=16, HorizontalSamples=512, RaysPerTick=2048
- 约 8192 条射线，1-2 个 Tick 完成一圈

**高密度采集**：
- Channels=32, HorizontalSamples=1024, RaysPerTick=1024
- 约 32768 条射线，32 个 Tick 完成一圈

---

## 相机配置

在场景中放置 Actor，添加以下两个组件：

1. `CameraRigComponent` — 管理渲染通道
2. `SimCameraSensorComponent` — Runtime 适配器

### CameraRigComponent 配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| SensorName | FName | FrontCamera | 人类可读显示名称，允许重复；唯一身份由组件自动维护的 SensorGuid 提供 |
| HorizontalFovDegrees | float | 90.0 | 水平视场角（度） |
| MaxPendingReadbacks | int | 8 | GPU 回读队列上限 |
| Channels | Array | - | 输出通道配置列表 |

### Channel 配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| ChannelType | Enum | Rgb | Rgb / Semantic / Depth / Instance |
| Resolution | FIntPoint | 1280x720 | 图像分辨率 |
| bEnabled | bool | true | 是否启用 |
| bForceLinearGamma | bool | false | 线性 Gamma（Semantic 必须开启） |

### 推荐配置

**RGB + Semantic 双通道**：
```
Channels:
  - ChannelType: Rgb, Resolution: 1280x720, bEnabled: true
  - ChannelType: Semantic, Resolution: 1280x720, bEnabled: true, bForceLinearGamma: true
```

---

## 语义物体配置

在需要标注的 Actor 上添加 `SemanticObjectComponent`：

| 参数 | 类型 | 范围 | 说明 |
|------|------|------|------|
| SemanticId | int | 0-255 | 语义类别 ID（0 保留为背景） |
| InstanceId | int | 0+ | 实例编号（自动分配） |

### 标准语义类别

| ID | 类别 |
|----|------|
| 0 | 背景 (Background) |
| 1 | 道路 (Road) |
| 2 | 车辆 (Vehicle) |
| 3 | 行人 (Pedestrian) |
| 4 | 骑行者 (Cyclist) |
| 5 | 建筑 (Building) |
| 6 | 植被 (Vegetation) |
| 7 | 障碍物 (Obstacle) |
| 8 | 自车 (EgoVehicle) |

---

## 运行时验证

### 编辑器内验证

1. 确认 `r.CustomDepth=3`（Project Settings → Rendering）
2. 进入 PIE（Play In Editor）
3. 检查 Output Log 无以下错误：
   - `Readback rejected`
   - `Rejected image payload`
   - `Frame X timed out`

### 输出数据验证

```bash
# 使用校验脚本
python Tools/validate_dataset.py Saved/SensorSimulation/20260721_143052/

# 跳过像素级验证（更快）
python Tools/validate_dataset.py Saved/SensorSimulation/20260721_143052/ --skip-pixels
```

### Python 读取示例

```python
import numpy as np
import json
from PIL import Image

# 读取 RGB 图像
rgb = np.array(Image.open("frame_000001/rgb.png"))

# 读取 Semantic 图像
semantic = np.array(Image.open("frame_000001/semantic.png"))
semantic_ids = semantic[:, :, 0]  # R 通道即为语义 ID

# 读取 LiDAR 点云
points = np.fromfile("frame_000001/lidar.bin", dtype=np.float32).reshape(-1, 4)
# 列: x, y, z, intensity

# 读取 Ground Truth
with open("frame_000001/groundtruth.json") as f:
    gt = json.load(f)
```

---

## 常见问题

**Q: Semantic 图像全黑？**
A: 确认 `r.CustomDepth=3`，重启编辑器后生效。

**Q: LiDAR 没有输出？**
A: 检查 `SimLidarSensorComponent` 的 `bSensorEnabled` 是否为 true，以及场景中是否有可碰撞物体。

**Q: 帧超时日志？**
A: 检查传感器组件是否正常工作，增大 `FrameTimeoutSeconds` 或降低采集频率。

**Q: 导出目录为空？**
A: 检查 `DatasetRoot` 配置，确认 `ExportService` 正常启动。
