#pragma once

#include "SceneViewExtension.h"

/**
 * 仅为带有 Semantic Capture 标记的 SceneCapture 注入标签输出 Pass。
 * Pass 在 Tonemap 完成后覆盖场景颜色，因此标签不会再受到曝光、光照或色调映射影响。
 */
class FSemanticCaptureViewExtension final : public FSceneViewExtensionBase
{
public:
    explicit FSemanticCaptureViewExtension(const FAutoRegister& AutoRegister);

    /** 在语义视图的 Tonemap 节点后注册全屏标签输出回调。 */
    virtual void SubscribeToPostProcessingPass(
        EPostProcessingPass Pass,
        const FSceneView& InView,
        FPostProcessingPassDelegateArray& InOutPassCallbacks,
        bool bIsPassEnabled) override;

private:
    /** 从 CustomStencil 逐像素读取标签并覆盖后处理输出。 */
    FScreenPassTexture RenderSemanticLabels(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FPostProcessMaterialInputs& Inputs) const;
};