#include "InstanceCaptureViewExtension.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "MeshMaterialShader.h"
#include "MeshPassProcessor.h"
#include "MeshPassProcessor.inl"
#include "Nanite/NaniteCullRaster.h"
#include "Nanite/NaniteShared.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RendererCompatibility.h"
#include "PrimitiveSceneProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "ScenePrivate.h"
#include "SceneRendering.h"
#include "SimpleMeshDrawCommandPass.h"
#include "StaticMeshBatch.h"

DEFINE_LOG_CATEGORY_STATIC(LogInstanceCapturePass, Log, All);

namespace
{
/** Decodes Nanite visibility and exports SensorSimulation instance IDs. */
class FNaniteInstanceCapturePS final : public FNaniteGlobalShader
{
    DECLARE_GLOBAL_SHADER(FNaniteInstanceCapturePS);
    SHADER_USE_PARAMETER_STRUCT(FNaniteInstanceCapturePS, FNaniteGlobalShader);

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return FNaniteGlobalShader::ShouldCompilePermutation(Parameters);
    }

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
        SHADER_PARAMETER(FIntVector4, ViewRect)
        SHADER_PARAMETER(uint32, PrimitiveBindingCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, VisibleClustersSWHW)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, PrimitiveInstanceBindings)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<UlongType>, VisBuffer64)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(
    FNaniteInstanceCapturePS,
    "/Plugin/SensorSimulation/Private/NaniteInstanceCapture.usf",
    "MainPS",
    SF_Pixel);

/** 每个 Mesh Draw 传递给 Pixel Shader 的完整实例编号。 */
struct FInstanceCaptureElementData final : public FMeshMaterialShaderElementData
{
    uint32 BaseInstanceId = 0;
    uint32 bUseInternalInstanceId = 0;
};

/** 使用当前 VertexFactory 和 View 矩阵生成与基础 Pass 一致的几何位置。 */
class FInstanceCaptureVS final : public FMeshMaterialShader
{
    DECLARE_SHADER_TYPE(FInstanceCaptureVS, MeshMaterial);

public:
    FInstanceCaptureVS() = default;
    explicit FInstanceCaptureVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FMeshMaterialShader(Initializer)
    {
    }

    /** 默认表面材质需要为所有 SM5 VertexFactory 提供 Instance 几何着色器。 */
    static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
            (Parameters.MaterialParameters.bIsSpecialEngineMaterial ||
                Parameters.MaterialParameters.bIsMasked);
    }

    /** Instance Pass 不采样场景纹理，只依赖 View、Primitive 和 VertexFactory 数据。 */
    static void ModifyCompilationEnvironment(
        const FMaterialShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("SCENE_TEXTURES_DISABLED"), 1);
    }


};

/** 把每个 Draw 绑定的 uint32 InstanceId 原样写入 R32_UINT。 */
class FInstanceCapturePS final : public FMeshMaterialShader
{
    DECLARE_SHADER_TYPE(FInstanceCapturePS, MeshMaterial);
    LAYOUT_FIELD(FShaderParameter, BaseInstanceIdParameter);
    LAYOUT_FIELD(FShaderParameter, UseInternalInstanceIdParameter);

public:
    FInstanceCapturePS() = default;
    explicit FInstanceCapturePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FMeshMaterialShader(Initializer)
    {
        BaseInstanceIdParameter.Bind(Initializer.ParameterMap, TEXT("BaseInstanceId"));
        UseInternalInstanceIdParameter.Bind(
            Initializer.ParameterMap,
            TEXT("UseInternalInstanceId"));
    }

    /** 与 VS 使用相同的默认表面材质排列，避免为业务材质生成额外颜色变体。 */
    static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
            (Parameters.MaterialParameters.bIsSpecialEngineMaterial ||
                Parameters.MaterialParameters.bIsMasked);
    }

    /** 显式声明整数 RenderTarget 格式，防止 D3D12 创建浮点/整数不兼容的 PSO。 */
    static void ModifyCompilationEnvironment(
        const FMaterialShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32_UINT);
        OutEnvironment.SetDefine(TEXT("SCENE_TEXTURES_DISABLED"), 1);
    }

    /** 把 MeshProcessor 选择的实例编号绑定到本次 Draw 的 Pixel Shader。 */
    void GetShaderBindings(
        const FScene* Scene,
        const ERHIFeatureLevel::Type FeatureLevel,
        const FPrimitiveSceneProxy* PrimitiveSceneProxy,
        const FMaterialRenderProxy& MaterialRenderProxy,
        const FMaterial& Material,
        const FInstanceCaptureElementData& ShaderElementData,
        FMeshDrawSingleShaderBindings& ShaderBindings) const
    {
        FMeshMaterialShader::GetShaderBindings(
            Scene,
            FeatureLevel,
            PrimitiveSceneProxy,
            MaterialRenderProxy,
            Material,
            ShaderElementData,
            ShaderBindings);
        ShaderBindings.Add(BaseInstanceIdParameter, ShaderElementData.BaseInstanceId);
        ShaderBindings.Add(
            UseInternalInstanceIdParameter,
            ShaderElementData.bUseInternalInstanceId);
    }
};

IMPLEMENT_MATERIAL_SHADER_TYPE(
    ,
    FInstanceCaptureVS,
    TEXT("/Plugin/SensorSimulation/Private/InstanceCapture.usf"),
    TEXT("MainVS"),
    SF_Vertex);

IMPLEMENT_MATERIAL_SHADER_TYPE(
    ,
    FInstanceCapturePS,
    TEXT("/Plugin/SensorSimulation/Private/InstanceCapture.usf"),
    TEXT("MainPS"),
    SF_Pixel);

/**
 * 为可见且已登记的 Opaque Primitive 构建 Instance Mesh DrawCommand。
 *
 * R15 之前只承诺普通非 Nanite Opaque Mesh。Masked、Translucent、Nanite、
 * ISM/HISM 的逐实例语义需要独立产品规则，不能在这里静默输出错误轮廓。
 */
class FInstanceCaptureMeshProcessor final : public FMeshPassProcessor
{
public:
    FInstanceCaptureMeshProcessor(
        const FScene* Scene,
        const FSceneView* View,
        FMeshPassDrawListContext* DrawListContext)
        : FMeshPassProcessor(
            TEXT("SensorSimulationInstanceCapture"),
            Scene,
            View->GetFeatureLevel(),
            View,
            DrawListContext)
    {
        // A pass-local reversed-Z depth target keeps the nearest visible instance.
        DrawRenderState.SetDepthStencilState(
            TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
        DrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());
    }

    /** 过滤未注册或当前不支持的 Mesh，并把完整 InstanceId 传入 DrawCommand。 */
    virtual void AddMeshBatch(
        const FMeshBatch& RESTRICT MeshBatch,
        const uint64 BatchElementMask,
        const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
        const int32 StaticMeshId = -1) override
    {
        // 查询完整 InstanceId
        const UE::SensorSimulation::InstanceCapture::FPrimitiveInstanceBinding Binding =
            UE::SensorSimulation::InstanceCapture::FindInstanceBinding_RenderThread(
                PrimitiveSceneProxy);
        
        // 0 表示背景或没有注册实例，因此 ID 为 0 的 Primitive 不会生成 DrawCommand
        // bUseForMaterial ： 这个 MeshBatch 是否允许参与材质渲染 Pass
        // MaterialRenderProxy ： 是否存在渲染线程可用的材质代理
        if (Binding.BaseInstanceId == 0 ||
            !MeshBatch.bUseForMaterial ||
            !MeshBatch.MaterialRenderProxy)
        {
            return;
        }

        UE_LOG(LogInstanceCapturePass, VeryVerbose,
            TEXT("Submitting primitive %u with InstanceId %u."),
            PrimitiveSceneProxy->GetPrimitiveComponentId().PrimIDValue,
            Binding.BaseInstanceId);

        // 获得物体原来的业务材质
        const FMaterial& SourceMaterial =
            MeshBatch.MaterialRenderProxy->GetIncompleteMaterialWithFallback(FeatureLevel);
        
        // Instance 产品支持 Opaque 与 Masked；Translucent 由注册阶段按既定策略排除。
        // 不能使用 IsOpaqueBlendMode：UE 将 BLEND_Masked 与 BLEND_Opaque 分开，
        // 否则下方 bIsMasked 分支永远不可达，foliage 的不透明像素也不会写入标签。
        if (!IsOpaqueOrMaskedBlendMode(SourceMaterial))
        {
            return;
        }
        
        const bool bIsMasked = SourceMaterial.IsMasked();
        const FMaterialRenderProxy* CaptureMaterialProxy = MeshBatch.MaterialRenderProxy;
        // Instance Pass 真正拿来选择和绑定 Shader 的材质
        const FMaterial* CaptureMaterial = &SourceMaterial;
        if (!bIsMasked)
        {
            // 非Masked的Opaque材质 统一使用默认材质进行 Instance Pass
            // 减少UE需要编译的shader组合
            // 默认材质在这里相当于给自定义 Instance Shader 提供一个稳定的载体
            // 不绕过材质shader,是因为可以直接复用 UE 已经成熟的 Mesh 绘制系统
            CaptureMaterialProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
            CaptureMaterial = CaptureMaterialProxy->GetMaterialNoFallback(FeatureLevel);
        }

        if (!CaptureMaterialProxy || !CaptureMaterial)
        {
            return;
        }
        Process(
            MeshBatch,
            BatchElementMask,
            StaticMeshId,
            PrimitiveSceneProxy,
            Binding,
            SourceMaterial,
            *CaptureMaterialProxy,
            *CaptureMaterial);
    }

private:
    /** Build a DrawCommand that writes both the uint InstanceId and pass-local depth. */
    bool Process(
        const FMeshBatch& MeshBatch,
        const uint64 BatchElementMask,
        const int32 StaticMeshId,
        const FPrimitiveSceneProxy* PrimitiveSceneProxy,
        const UE::SensorSimulation::InstanceCapture::FPrimitiveInstanceBinding& Binding,
        const FMaterial& SourceMaterial,
        const FMaterialRenderProxy& MaterialRenderProxy,
        const FMaterial& MaterialResource)
    {
        FMaterialShaderTypes ShaderTypes;
        ShaderTypes.AddShaderType<FInstanceCaptureVS>();
        ShaderTypes.AddShaderType<FInstanceCapturePS>();

        FMaterialShaders MaterialShaders;
        if (!MaterialResource.TryGetShaders(
                ShaderTypes,
                MeshBatch.VertexFactory->GetType(),
                MaterialShaders))
        {
            UE_LOG(LogInstanceCapturePass, VeryVerbose,
                TEXT("Missing Instance shaders for VertexFactory %s."),
                MeshBatch.VertexFactory->GetType()->GetName());
            return false;
        }

        TMeshProcessorShaders<FInstanceCaptureVS, FInstanceCapturePS> Shaders;
        MaterialShaders.TryGetVertexShader(Shaders.VertexShader);
        MaterialShaders.TryGetPixelShader(Shaders.PixelShader);

        // Keep this VeryVerbose diagnostic close to command construction: it distinguishes
        // a selected primitive from a draw that UE deliberately skips or cannot shader-bind.
        UE_LOG(LogInstanceCapturePass, VeryVerbose,
            TEXT("Draw diagnostics: elements=%d mask=0x%llx skip=%d VS=%d PS=%d VF=%s."),
            MeshBatch.Elements.Num(),
            BatchElementMask,
            ShouldSkipMeshDrawCommand(MeshBatch, PrimitiveSceneProxy) ? 1 : 0,
            Shaders.VertexShader.IsValid() ? 1 : 0,
            Shaders.PixelShader.IsValid() ? 1 : 0,
            MeshBatch.VertexFactory->GetType()->GetName());

        const FMeshDrawingPolicyOverrideSettings OverrideSettings =
            ComputeMeshOverrideSettings(MeshBatch);
        const ERasterizerFillMode FillMode =
            ComputeMeshFillMode(SourceMaterial, OverrideSettings);
        const ERasterizerCullMode CullMode =
            ComputeMeshCullMode(SourceMaterial, OverrideSettings);

        // 传给 PS 的数据
        FInstanceCaptureElementData ShaderElementData;
        ShaderElementData.InitializeMeshMaterialData(
            ViewIfDynamicMeshCommand,
            PrimitiveSceneProxy,
            MeshBatch,
            StaticMeshId,
            false);
        // PS类会绑定这个ID，最终usf接收
        ShaderElementData.BaseInstanceId = Binding.BaseInstanceId;
        ShaderElementData.bUseInternalInstanceId = Binding.bUseInternalInstanceId ? 1u : 0u;

        BuildMeshDrawCommands(
            MeshBatch,
            BatchElementMask,
            PrimitiveSceneProxy,
            MaterialRenderProxy,
            MaterialResource,
            DrawRenderState,
            Shaders,
            FillMode,
            CullMode,
            FMeshDrawCommandSortKey{},
            EMeshPassFeatures::Default,
            ShaderElementData);
        return true;
    }

    FMeshPassProcessorRenderState DrawRenderState;
};

/** Bind the integer color target, pass-local depth, and VertexFactory static uniforms. */
BEGIN_SHADER_PARAMETER_STRUCT(FInstanceCapturePassParameters, )
    SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
    SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/** Merges visible Nanite pixels into the classic instance output using shared depth. */
void AddNaniteInstanceCapturePass(
    FRDGBuilder& GraphBuilder,
    const FScene& Scene,
    const FViewInfo& View,
    const Nanite::FRasterResults& RasterResults,
    FRDGTextureRef OutputTexture,
    FRDGTextureRef DepthTexture,
    const FIntRect& OutputRect)
{
    if (!RasterResults.VisBuffer64 || !RasterResults.VisibleClustersSWHW)
    {
        return;
    }

    const uint32 BindingCount = static_cast<uint32>(Scene.Primitives.Num());
    if (BindingCount == 0)
    {
        return;
    }

    TArray<FUint32Vector2> Bindings;
    Bindings.SetNumZeroed(BindingCount);
    for (uint32 PrimitiveIndex = 0; PrimitiveIndex < BindingCount; ++PrimitiveIndex)
    {
        const FPrimitiveSceneInfo* SceneInfo = Scene.Primitives[PrimitiveIndex];
        if (!SceneInfo || !SceneInfo->Proxy)
        {
            continue;
        }
        const auto Binding =
            UE::SensorSimulation::InstanceCapture::FindInstanceBinding_RenderThread(SceneInfo->Proxy);
        Bindings[PrimitiveIndex] = FUint32Vector2(
            Binding.BaseInstanceId,
            Binding.bUseInternalInstanceId ? 1u : 0u);
    }

    FRDGBufferRef BindingBuffer = CreateStructuredBuffer(
        GraphBuilder,
        TEXT("SensorSimulation.NanitePrimitiveInstanceBindings"),
        Bindings);

    auto* PassParameters = GraphBuilder.AllocParameters<FNaniteInstanceCapturePS::FParameters>();
    PassParameters->View = View.ViewUniformBuffer;
    PassParameters->Scene = View.GetSceneUniforms().GetBuffer(GraphBuilder);
    PassParameters->ViewRect = FIntVector4(
        View.ViewRect.Min.X, View.ViewRect.Min.Y,
        View.ViewRect.Max.X, View.ViewRect.Max.Y);
    PassParameters->PrimitiveBindingCount = BindingCount;
    PassParameters->VisibleClustersSWHW = GraphBuilder.CreateSRV(RasterResults.VisibleClustersSWHW);
    PassParameters->PrimitiveInstanceBindings = GraphBuilder.CreateSRV(BindingBuffer);
    PassParameters->VisBuffer64 = RasterResults.VisBuffer64;
    PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ELoad);
    PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
        DepthTexture,
        ERenderTargetLoadAction::ELoad,
        ERenderTargetLoadAction::ENoAction,
        FExclusiveDepthStencil::DepthWrite_StencilNop);

    TShaderMapRef<FNaniteInstanceCapturePS> PixelShader(View.ShaderMap);
    FPixelShaderUtils::AddFullscreenPass(
        GraphBuilder,
        View.ShaderMap,
        RDG_EVENT_NAME("SensorSimulation.NaniteInstanceCapture(R32Uint)"),
        PixelShader,
        PassParameters,
        OutputRect,
        TStaticBlendState<>::GetRHI(),
        TStaticRasterizerState<>::GetRHI(),
        TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}
}

FInstanceCaptureViewExtension::FInstanceCaptureViewExtension(
    const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}

/**
 * 选择 Tonemap 作为调度入口，是因为 UE 5.7 会稳定广播该后处理扩展点；
 * 回调复用 View 可见性；普通 Mesh Pass 与 Nanite VisBuffer Pass 共用深度并写入 PF_R32_UINT ID。
 */
void FInstanceCaptureViewExtension::SubscribeToPostProcessingPass(
    EPostProcessingPass Pass,
    const FSceneView& InView,
    FPostProcessingPassDelegateArray& InOutPassCallbacks,
    bool bIsPassEnabled)
{
    const bool bIsInstanceView = InView.Family &&
        UE::SensorSimulation::InstanceCapture::FindOutputTexture_RenderThread(
            InView.Family->RenderTarget).IsValid();
    if (bIsInstanceView && Pass == EPostProcessingPass::Tonemap)
    {
        InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
            this,
            &FInstanceCaptureViewExtension::RenderInstanceIds));
    }
}

/** 在 Tonemap 回调中绘制本 View 中所有已注册的可见普通 Opaque Mesh。 */
FScreenPassTexture FInstanceCaptureViewExtension::RenderInstanceIds(
    FRDGBuilder& GraphBuilder,
    const FSceneView& InView,
    const FPostProcessMaterialInputs& Inputs) const
{
    check(IsInRenderingThread());

    // 回调必须把 SceneColor 继续传给 UE 的后处理链；Instance Pass 只写独立整数目标。
    const auto ReturnSceneColor = [&Inputs, &GraphBuilder]()
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    };

    if (!InView.Family || !InView.Family->Scene)
    {
        return ReturnSceneColor();
    }

    const FTextureRHIRef OutputTextureRHI =
        UE::SensorSimulation::InstanceCapture::FindOutputTexture_RenderThread(
            InView.Family->RenderTarget);
    if (!OutputTextureRHI.IsValid() || OutputTextureRHI->GetFormat() != PF_R32_UINT)
    {
        return ReturnSceneColor();
    }

    const FViewInfo& View = static_cast<const FViewInfo&>(InView);
    const FScene* Scene = InView.Family->Scene->GetRenderScene();
    if (!Scene)
    {
        return ReturnSceneColor();
    }

    UE_LOG(LogInstanceCapturePass, VeryVerbose,
        TEXT("Matched Instance View: min=(%d,%d) size=%dx%d, static visible=%d, dynamic=%d."),
        View.ViewRect.Min.X,
        View.ViewRect.Min.Y,
        View.ViewRect.Width(),
        View.ViewRect.Height(),
        View.StaticMeshVisibilityMap.CountSetBits(),
        View.DynamicMeshElements.Num());

    // 把外部 RHI 纹理注册到 RDG
    FRDGTextureRef OutputTexture = GraphBuilder.RegisterExternalTexture(
        CreateRenderTarget(OutputTextureRHI, TEXT("SensorSimulation.InstanceOutput")));
    
    // Pass 专用深度纹理
    const FRDGTextureDesc InstanceDepthDesc = FRDGTextureDesc::Create2D(
        OutputTexture->Desc.Extent,
        PF_DepthStencil,
        FClearValueBinding::DepthFar,
        ETextureCreateFlags::DepthStencilTargetable);
    FRDGTextureRef InstanceDepthTexture = GraphBuilder.CreateTexture(
        InstanceDepthDesc,
        TEXT("SensorSimulation.InstanceDepth"));
    // The native integer target is never a pooled sub-rect, so rasterization must use its zero-based extent.
    const FIntRect OutputRect(FIntPoint::ZeroValue, OutputTexture->Desc.Extent);

    FInstanceCapturePassParameters* PassParameters =
        GraphBuilder.AllocParameters<FInstanceCapturePassParameters>();
    PassParameters->View = View.ViewUniformBuffer;
    PassParameters->Scene = View.GetSceneUniforms().GetBuffer(GraphBuilder);

    PassParameters->RenderTargets[0] =
        FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::EClear);
    PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
        InstanceDepthTexture,
        ERenderTargetLoadAction::EClear,
        ERenderTargetLoadAction::ENoAction,
        FExclusiveDepthStencil::DepthWrite_StencilNop);

    // Borrow the formal post-process instance-culling manager so ISM/HISM draws use GPU Scene instance lists.
    const UE::Renderer::PostProcess::FExtensionContext* ExtensionContext =
        SensorSimulation::RendererCompatibility::GetPostProcessContext_RenderThread();
    if (!ExtensionContext || !ExtensionContext->InstanceCullingManager)
    {
        UE_LOG(LogInstanceCapturePass, Warning, TEXT("Instance capture skipped: instance-culling context unavailable."));
        return ReturnSceneColor();
    }
    AddSimpleMeshPass(
        GraphBuilder,
        PassParameters,
        Scene,
        View,
        ExtensionContext->InstanceCullingManager,
        RDG_EVENT_NAME("SensorSimulation.InstanceCapture(R32Uint)"),
        OutputRect,
        [Scene, &View](FDynamicPassMeshDrawListContext* DrawListContext)
        {
            FInstanceCaptureMeshProcessor MeshProcessor(Scene, &View, DrawListContext);

            // StaticMeshVisibilityMap 已包含视锥、遮挡与 LOD 选择结果，只提交当前可见批次。
            // Instance Pass不需要重新遍历世界中所有 Actor。只处理：当前相机已经判定可见的静态 MeshBatch
            for (FSceneSetBitIterator It(View.StaticMeshVisibilityMap); It; ++It)
            {
                FStaticMeshBatch* StaticMesh = Scene->StaticMeshes[It.GetIndex()];
                if (StaticMesh && StaticMesh->PrimitiveSceneInfo)
                {
                    // 过滤
                    MeshProcessor.AddMeshBatch(
                        *StaticMesh,
                        ~0ull,
                        StaticMesh->PrimitiveSceneInfo->Proxy,
                        StaticMesh->Id);
                }
            }

            // 动态 Primitive 的 MeshBatch 已在 InitViews 阶段收集并完成可见性筛选。
            for (const FMeshBatchAndRelevance& MeshAndRelevance : View.DynamicMeshElements)
            {
                if (MeshAndRelevance.Mesh && MeshAndRelevance.PrimitiveSceneProxy)
                {
                    // 过滤
                    MeshProcessor.AddMeshBatch(
                        *MeshAndRelevance.Mesh,
                        ~0ull,
                        MeshAndRelevance.PrimitiveSceneProxy);
                }
            }
        });

    if (ExtensionContext->NaniteRasterResults)
    {
        AddNaniteInstanceCapturePass(
            GraphBuilder,
            *Scene,
            View,
            *ExtensionContext->NaniteRasterResults,
            OutputTexture,
            InstanceDepthTexture,
            OutputRect);
    }

    // InstanceTarget 是跨 RDG 图使用的外部 RHI 纹理，后续渲染命令会直接把它复制到 GPU Readback。
    // D3D12 必须显式声明图结束后的 CopySrc 状态；否则下一条 CopyTexture 可能偶发读到清屏结果，
    // 即使本图已经正确构建并提交全部 Instance Draw。D3D11 的隐式状态模型会掩盖该问题。
    GraphBuilder.SetTextureAccessFinal(OutputTexture, ERHIAccess::CopySrc);

    return ReturnSceneColor();
}
