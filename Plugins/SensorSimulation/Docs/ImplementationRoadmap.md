# Implementation Roadmap

## 当前框架状态

已搭建：

- 四模块 UE5 Plugin。
- 统一 Frame、Image、LiDAR、Ground Truth 数据协议。
- 集中坐标转换。
- Camera 多通道组件骨架。
- Semantic/Instance Registry。
- CPU LiDAR 角度预计算、分批 Trace、点标签与完整扫描输出。
- Frame Assembler。
- GPU Readback 与异步 Export 的明确接口边界。

尚未完成：

- Semantic Post Process/独立整数 Label Pass。
- `FRHIGPUTextureReadback` 实际提交与轮询。
- Camera Sensor 到 Runtime Subsystem 的完整 Adapter。
- Export Worker、PNG/BIN/CSV Writer。
- Deterministic Dataset Clock 的暂停/推进控制。
- 自动化 Demo Map。

## 两人第一轮分工

### Render Pipeline Owner

1. 为 Semantic Channel 创建无后处理污染的 Material/Global Shader。
2. 实现 `FSensorImageReadbackManager::Enqueue`。
3. 输出 RGB/Semantic Payload 并提交给 Subsystem。
4. 验证标签合法值、View Rect、Gamma 和像素格式。

### Runtime/System Owner

1. 完善 Camera Adapter 和 Sensor 类型识别，修正 Subsystem 的 Expected Payload 计算。
2. 将 CPU LiDAR 同步批处理替换或扩展为 UE Async Trace。
3. 实现 Export Worker 和 `.bin/CSV/JSON` Writer。
4. 完成固定时间步、队列背压和 Frame Timeout。

## 首个集成验收

- 场景中一个 Camera Rig 和一个 16×512 LiDAR。
- 三个带 Semantic Component 的 Actor。
- Frame 1 同步输出 RGB、Semantic、LiDAR 和 Ground Truth。
- LiDAR `ExpectedRayCount == CompletedRayCount`。
- Semantic 图非法 ID 像素数为 0。
- 不使用每帧同步 `ReadPixels()`。
