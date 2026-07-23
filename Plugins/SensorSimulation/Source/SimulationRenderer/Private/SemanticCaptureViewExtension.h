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

	// 当 UE 构建某个 View 的后处理流程时，判断它是不是语义相机；
	// 如果是，并且当前正在配置 Tonemap 阶段，就把 RenderSemanticLabels() 注册为该阶段的额外渲染回调。
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