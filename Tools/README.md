# SensorSimulation 验证与构建工具

## Renderer 前置门禁

统一构建入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Tools\build_with_renderer_preflight.ps1 `
  -EngineRoot D:\UnrealEngine5.7.2
```

执行顺序固定为：

1. 检查 UE 版本是否为 5.7.2。
2. 检查 Renderer 补丁的类型、成员、Getter 和作用域 Guard。
3. 只有检查通过才调用 `Build.bat`。
4. 把前置检查和构建结果写入 `Saved/Acceptance/RendererPreflight/UE572`。

只运行前置检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Tools\build_with_renderer_preflight.ps1 `
  -EngineRoot D:\UnrealEngine5.7.2 `
  -SkipBuild
```

也可以设置 `UE_ENGINE_ROOT`，省略命令行中的 `-EngineRoot`。

自托管 CI、Jenkins 或本机构建机应调用这个统一入口，而不是直接调用 UE `Build.bat`。进程退出码和 `RendererPreflightSummary.json` 的 `Passed` 字段都必须为成功，流水线才能继续运行像素测试。

### GitHub Actions 手动门禁

`.github/workflows/renderer-preflight.yml` 只支持手动触发。运行前需要：

1. 在隔离的 Windows x64 构建机安装 GitHub Actions Runner。
2. 给 Runner 添加 `ue-5.7.2` 标签。
3. 确保 Runner 服务环境的 `UE_ENGINE_ROOT` 指向已应用补丁的 UE 5.7.2；未设置时默认使用 `D:\UnrealEngine5.7.2`。
4. 在 Actions 页面手动运行 `Renderer preflight and build`。

为了保护持有 Epic 源码的构建机，工作流不会由 PR 或 push 自动触发，也不会保留 Checkout 凭据。

## Renderer 输出矩阵

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Tools\run_renderer_output_matrix.ps1 `
  -EngineRoot D:\UnrealEngine5.7.2
```

该入口顺序运行 D3D11/D3D12 四模态小尺寸静态输出测试并生成汇总 JSON。

阶段 2 标准/高清、运动与遮挡矩阵：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Tools\run_renderer_output_matrix_phase2.ps1 `
  -EngineRoot D:\UnrealEngine5.7.2
```

阶段 2 覆盖 640×480、1280×720，连续验证静态前景、Actor 移出无残影、移回遮挡、OpaqueProxy/Ignore、相机前移和相机复位无历史残留，并把 Accepted/Busy/Rejected、GPU/交付时延及 Readback 资源复用写入 `Saved/Acceptance/RendererOutputMatrixPhase2/UE572/RendererOutputMatrixPhase2Summary.json`。
