#pragma once

#include "SceneViewExtension.h"

class FPrimitiveSceneProxy;
class FRenderTarget;
class FRDGBuilder;
struct FPostProcessingInputs;

namespace UE::SensorSimulation::InstanceCapture
{
inline constexpr const TCHAR* ProfilingEventName = TEXT("SensorSimulation.Instance");

/**
 * 关联 SceneCapture 使用的颜色目标与真正的 PF_R32_UINT 输出目标。
 *
 * SceneCapture 仍需要普通颜色目标来建立 ViewFamily 和 SceneDepth；独立
 * Instance Mesh Pass 把整数结果写入 OutputTarget，不让基础颜色 Pass 接触整数 RT。
 */
void RegisterCaptureTarget(
    const FRenderTarget* CaptureTarget,
    const FRenderTarget* OutputTarget);

/** 移除一组 Capture/整数输出目标关联。 */
void UnregisterCaptureTarget(const FRenderTarget* CaptureTarget);

/** 查询当前 ViewFamily 对应的整数输出纹理；只在渲染线程调用。 */
FTextureRHIRef FindOutputTexture_RenderThread(const FRenderTarget* CaptureTarget);

/** 每个 Primitive Draw 使用的 Instance 身份绑定。 */
struct FPrimitiveInstanceBinding
{
    /** 普通图元的完整 ID，或 ISM/HISM 连续 ID 区间的起点。 */
    uint32 BaseInstanceId = 0;
    /** 为 true 时，Shader 将 GPU Scene 的内部实例相对编号加到起点。 */
    bool bUseInternalInstanceId = false;
    /** 此图元本次 Draw 必须提交的内部实例数量；普通图元恒为 1。 */
    uint32 InternalInstanceCount = 1;
};

/** 查询 SceneProxy 的身份绑定；未注册时返回 BaseInstanceId=0。 */
FPrimitiveInstanceBinding FindInstanceBinding_RenderThread(
    const FPrimitiveSceneProxy* PrimitiveSceneProxy);
}

/**
 * 在 Instance SceneCapture 的基础 Pass 完成后绘制独立整数 Mesh Pass。
 *
 * The pass reuses View visibility, redraws pass-local depth, and writes uint IDs to PF_R32_UINT.
 * 每个 Draw 直接向 PF_R32_UINT 写入 uint32 InstanceId。
 */
class FInstanceCaptureViewExtension final : public FSceneViewExtensionBase
{
public:
    explicit FInstanceCaptureViewExtension(const FAutoRegister& AutoRegister);

    /** 在已验证可执行的 Tonemap 回调点为 Instance View 注入独立整数 Mesh Pass。 */
    virtual void SubscribeToPostProcessingPass(
        EPostProcessingPass Pass,
        const FSceneView& InView,
        FPostProcessingPassDelegateArray& InOutPassCallbacks,
        bool bIsPassEnabled) override;

private:
    /** Redraw visible meshes into pass-local depth and a dedicated R32Uint Instance target. */
    FScreenPassTexture RenderInstanceIds(
        FRDGBuilder& GraphBuilder,
        const FSceneView& InView,
        const FPostProcessMaterialInputs& Inputs) const;
};
