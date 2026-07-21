#include "SemanticCaptureViewExtension.h"

#include "GlobalShader.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

/**
 * 从场景的 CustomStencil 纹理读取离散标签的全屏像素着色器。
 * 着色器不采样 SceneColor，也不执行任何颜色空间或后处理运算。
 */
class FSemanticCapturePS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FSemanticCapturePS);
    SHADER_USE_PARAMETER_STRUCT(FSemanticCapturePS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()

    /** Semantic Pass 依赖桌面级场景纹理和 CustomStencil。 */
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(
    FSemanticCapturePS,
    "/Plugin/SensorSimulation/Private/SemanticCapture.usf",
    "MainPS",
    SF_Pixel);

FSemanticCaptureViewExtension::FSemanticCaptureViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}

void FSemanticCaptureViewExtension::SubscribeToPostProcessingPass(
    EPostProcessingPass Pass,
    const FSceneView& InView,
    FPostProcessingPassDelegateArray& InOutPassCallbacks,
    bool bIsPassEnabled)
{
    // CameraRig 将 FinalToneCurveHDR 保留为 Semantic 专用捕获源；该枚举会稳定传递到渲染线程的 ViewFamily。
    const bool bIsSemanticView = InView.Family &&
        InView.Family->SceneCaptureSource == ESceneCaptureSource::SCS_FinalToneCurveHDR;
    if (bIsSemanticView && Pass == EPostProcessingPass::Tonemap)
    {
        InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
            this,
            &FSemanticCaptureViewExtension::RenderSemanticLabels));
    }
}

FScreenPassTexture FSemanticCaptureViewExtension::RenderSemanticLabels(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessMaterialInputs& Inputs) const
{
    const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
        GraphBuilder,
        Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!SceneColor.IsValid())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    // 最后一个后处理回调可能直接获得 SceneCapture 的目标；否则创建同尺寸的独立输出。
    FScreenPassRenderTarget Output = Inputs.OverrideOutput;
    if (!Output.IsValid())
    {
        Output = FScreenPassRenderTarget::CreateFromInput(
            GraphBuilder,
            SceneColor,
            ERenderTargetLoadAction::ENoAction,
            TEXT("SemanticCaptureOutput"));
    }

    FSemanticCapturePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSemanticCapturePS::FParameters>();
    PassParameters->View = View.ViewUniformBuffer;
    PassParameters->SceneTextures = Inputs.SceneTextures;
    PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();

    const FScreenPassTextureViewport OutputViewport(Output);
    TShaderMapRef<FScreenPassVS> VertexShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    TShaderMapRef<FSemanticCapturePS> PixelShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    AddDrawScreenPass(
        GraphBuilder,
        RDG_EVENT_NAME("SemanticCapture(CustomStencil)"),
        FScreenPassViewInfo(View),
        OutputViewport,
        OutputViewport,
        VertexShader,
        PixelShader,
        PassParameters);

    return Output;
}
