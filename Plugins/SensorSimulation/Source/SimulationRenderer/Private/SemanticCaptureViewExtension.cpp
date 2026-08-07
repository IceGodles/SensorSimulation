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

// 当 UE 构建某个 View 的后处理流程时，判断它是不是语义相机；
// 如果是，并且当前正在配置 Tonemap 阶段，就把 RenderSemanticLabels() 注册为该阶段的额外渲染回调。
void FSemanticCaptureViewExtension::SubscribeToPostProcessingPass(
    EPostProcessingPass Pass, // 处于哪个后处理阶段
    const FSceneView& InView, // 某一次渲染所使用的“相机视图描述”
    FPostProcessingPassDelegateArray& InOutPassCallbacks,
    bool bIsPassEnabled)
{
    const bool bIsSemanticView = InView.Family &&
        UE::SensorSimulation::SemanticCapture::IsRegisteredTarget(InView.Family->RenderTarget);
    if (bIsSemanticView && Pass == EPostProcessingPass::Tonemap)
    {
        // 把 RenderSemanticLabels 加入到pass的回调数组
        InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
            this,
            &FSemanticCaptureViewExtension::RenderSemanticLabels));
    }
}

// 语义标签回调函数
// Semantic Capture 在 Tonemap 阶段调用
// 在 RDG 中添加一个全屏绘制 Pass，让语义 Pixel Shader 对输出图像的每个像素执行一次，
// 读取对应位置的 CustomStencil，然后把标签写入输出 Render Target。
FScreenPassTexture FSemanticCaptureViewExtension::RenderSemanticLabels(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessMaterialInputs& Inputs) const
{
    // 取得后处理输入 SceneColor
    const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
        GraphBuilder,
        Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!SceneColor.IsValid())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    // 确定输出 Render Target
    // 最后一个后处理回调可能直接获得 SceneCapture 的目标；否则创建同尺寸的独立输出。
    // UE 的 Screen Pass 类型提供了相应转换，输出 Render Target 可以作为完成后的 Screen Pass Texture 返回
    FScreenPassRenderTarget Output = Inputs.OverrideOutput;
    if (!Output.IsValid())
    {
        Output = FScreenPassRenderTarget::CreateFromInput(
            GraphBuilder,
            SceneColor,
            ERenderTargetLoadAction::ENoAction,
            TEXT("SemanticCaptureOutput"));
    }

    // 分配并填写 Shader 参数
    FSemanticCapturePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSemanticCapturePS::FParameters>();
    PassParameters->View = View.ViewUniformBuffer;
    PassParameters->SceneTextures = Inputs.SceneTextures;
    PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();

    // 创建输出 Viewport
    const FScreenPassTextureViewport OutputViewport(Output);
    
    // 通用全屏顶点 Shader
    TShaderMapRef<FScreenPassVS> VertexShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    TShaderMapRef<FSemanticCapturePS> PixelShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    
    // 添加一个全屏绘制 Pass
    AddDrawScreenPass(
        GraphBuilder,
        RDG_EVENT_NAME("SemanticCapture(CustomStencil)"),
        FScreenPassViewInfo(View),
        OutputViewport,
        OutputViewport,
        VertexShader,
        PixelShader,
        PassParameters);

    // 返回生成的语义标签纹理
    return Output;
}
