# UE 5.7.2 Renderer 补丁

状态：已整理并纳入自动检查（2026-08-14）。

## 为什么要做

Instance Channel 需要复用当前视图正式的 `FInstanceCullingManager`，才能让 ISM/HISM 使用 GPU Scene 已筛选的内部实例列表；Nanite 则必须在 `NaniteRasterResults` 的可见性缓冲仍然有效时导出 ID。官方 View Extension 回调没有公开这两个短生命周期对象，因此插件不能只依赖官方二进制引擎完成 R15。

## 如何做

补丁只修改 `PostProcessing.h/.cpp`：

1. 在 Renderer 私有头中声明 `FExtensionContext`，只包含两个借用指针。
2. `AddPostProcessingPasses` 进入时用 `TGuardValue` 发布栈上上下文，退出时自动恢复旧值。
3. 插件在后处理 View Extension 回调中读取上下文，不拥有、不释放、也不跨帧缓存指针。
4. `check_renderer_patch.ps1` 同时检查引擎版本、特征符号和补丁状态。

应用补丁：

```powershell
Set-Location D:\UnrealEngine5.7.2
git apply --check D:\ueprojects\SensorSimulationHost\Tools\EnginePatches\UE5.7.2\SensorSimulationRendererContext.patch
git apply D:\ueprojects\SensorSimulationHost\Tools\EnginePatches\UE5.7.2\SensorSimulationRendererContext.patch
```

当前引擎已经应用补丁时，正向检查会失败而反向检查会成功，这是正常状态。可直接运行：

```powershell
D:\ueprojects\SensorSimulationHost\Tools\check_renderer_patch.ps1 -EngineRoot D:\UnrealEngine5.7.2
```

补丁后需要重新编译 `UnrealEditor` 或至少重新构建依赖 Renderer 的编辑器目标，再构建项目。

## 下一步如何优化

- 每次升级 UE 小版本时，从对应官方 tag 重新验证补丁锚点，不能仅修改版本号绕过守卫。
- 如果 Epic 后续公开等价 Renderer 扩展上下文，应删除本补丁并把兼容层切换到官方 API。
- CI 中先执行检查脚本，再编译插件和运行 D3D11/D3D12 像素矩阵，避免“能编译但上下文时机已改变”。
