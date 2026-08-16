#include "CameraRigComponent.h"
#include "SemanticObjectComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

namespace UE::SensorSimulation::RenderOutputMatrixTests
{
/** 用于在复制后的 PIE World 中定位矩阵测试 Camera Rig。 */
static const FName CameraTag(TEXT("SensorSimulationRenderOutputMatrixCamera"));
/** 用于定位同时提供 Semantic 和 Instance 真值的测试物体。 */
static const FName ObjectTag(TEXT("SensorSimulationRenderOutputMatrixObject"));
/** Semantic 图像中必须出现的合法 8 位类别。 */
static constexpr uint8 ExpectedSemanticId = 17;
/** Instance 图像中必须完整保留的大于 8 位的对象编号。 */
static constexpr uint32 ExpectedInstanceId = 0x01030000u;

/** 返回测试对指定模态期望的 CPU 规范化像素格式。 */
EImagePixelFormat GetExpectedPixelFormat(const EPayloadType PayloadType)
{
    switch (PayloadType)
    {
    case EPayloadType::Rgb:
    case EPayloadType::Semantic:
        return EImagePixelFormat::Rgba8;
    case EPayloadType::Depth:
        return EImagePixelFormat::R32Float;
    case EPayloadType::Instance:
        return EImagePixelFormat::R32Uint;
    default:
        return EImagePixelFormat::Unknown;
    }
}

/** 返回正式协议为指定模态声明的颜色空间。 */
EImageColorSpace GetExpectedColorSpace(const EPayloadType PayloadType)
{
    switch (PayloadType)
    {
    case EPayloadType::Rgb:
        return EImageColorSpace::SRgb;
    case EPayloadType::Semantic:
    case EPayloadType::Depth:
    case EPayloadType::Instance:
        return EImageColorSpace::Data;
    default:
        return EImageColorSpace::Unknown;
    }
}

/** 返回正式协议为指定模态声明的数值单位。 */
EImageValueUnit GetExpectedValueUnit(const EPayloadType PayloadType)
{
    switch (PayloadType)
    {
    case EPayloadType::Semantic:
    case EPayloadType::Instance:
        return EImageValueUnit::Identifier;
    case EPayloadType::Depth:
        return EImageValueUnit::Meters;
    default:
        return EImageValueUnit::None;
    }
}

/**
 * 在一次真实 PIE 捕获中验证四种图像模态和两种分辨率。
 *
 * 测试由外部命令分别使用 D3D11/D3D12 启动，因此本类只关心当前 RHI 的像素结果；
 * 两份运行日志共同构成跨 RHI 矩阵证据。
 */
class FCaptureOutputMatrixCommand final : public IAutomationLatentCommand
{
public:
    explicit FCaptureOutputMatrixCommand(FAutomationTestBase* InTest)
        : Test(InTest)
        , StartSeconds(FPlatformTime::Seconds())
    {
    }

    virtual bool Update() override
    {
        UWorld* PieWorld = GEditor ? GEditor->PlayWorld : nullptr;
        if (!PieWorld)
        {
            return WaitOrFail(TEXT("PIE World was not created within 45 seconds."));
        }

        UCameraRigComponent* Rig = nullptr;
        USemanticObjectComponent* Semantic = nullptr;
        for (TActorIterator<AActor> It(PieWorld); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->ActorHasTag(CameraTag))
            {
                Rig = Actor->FindComponentByClass<UCameraRigComponent>();
            }
            if (Actor->ActorHasTag(ObjectTag))
            {
                Semantic = Actor->FindComponentByClass<USemanticObjectComponent>();
            }
        }
        if (!Rig || !Semantic)
        {
            Test->AddError(TEXT("Output matrix Camera Rig or semantic object is missing in PIE."));
            return true;
        }

        if (!bCaptureSubmitted)
        {
            // 等待 SceneProxy、CustomDepth 和 GPU Scene 数据完成首轮上传。
            if (WarmupFrames++ < 5)
            {
                return false;
            }

            Semantic->SetAssignedInstanceId(ExpectedInstanceId, 1);
            for (const FCameraChannelConfig& Channel : Rig->Channels)
            {
                ExpectedChannels.Add(Channel.ChannelGuid, Channel);
            }

            FCaptureRequest Request;
            Request.Header.FrameId = 2001;
            Request.Header.SequenceId = 1;
            Request.SensorName = Rig->SensorName;
            Request.SensorGuid = FGuid(0x100, 0x200, 0x300, 0x400);
            Request.ExpectedPayloads = Rig->GetEnabledPayloadTypes();
            Request.ExpectedImageChannels = Rig->GetEnabledImageChannels();
            Test->TestEqual(TEXT("Matrix contains four modalities at two resolutions"),
                Request.ExpectedImageChannels.Num(), 8);
            Test->TestEqual(TEXT("Matrix capture is accepted"),
                Rig->SubmitCapture(Request), ECaptureRequestResult::Accepted);
            bCaptureSubmitted = true;
            return false;
        }

        FImagePayload Payload;
        while (Rig->PollCompletedImage(Payload))
        {
            if (Payload.Header.FrameId != 2001)
            {
                continue;
            }
            if (ReceivedChannels.Contains(Payload.ChannelGuid))
            {
                Test->AddError(FString::Printf(TEXT("Channel %s produced a duplicate Payload."),
                    *Payload.ChannelGuid.ToString()));
                continue;
            }

            const FCameraChannelConfig* Config = ExpectedChannels.Find(Payload.ChannelGuid);
            if (!Config)
            {
                Test->AddError(FString::Printf(TEXT("Unexpected output ChannelGuid %s."),
                    *Payload.ChannelGuid.ToString()));
                continue;
            }
            ReceivedChannels.Add(Payload.ChannelGuid);
            ValidatePayload(Payload, *Config);
        }

        if (ReceivedChannels.Num() < ExpectedChannels.Num())
        {
            return WaitOrFail(TEXT("Not all output-matrix Payloads completed within 45 seconds."));
        }

        Test->TestEqual(TEXT("Every matrix channel delivered exactly once"),
            ReceivedChannels.Num(), ExpectedChannels.Num());
        return true;
    }

private:
    /** 校验通用协议字段以及模态专属像素不变量。 */
    void ValidatePayload(const FImagePayload& Payload, const FCameraChannelConfig& Config)
    {
        const EPayloadType ExpectedType = Config.ToPayloadType();
        const FString Prefix = FString::Printf(TEXT("%s %dx%d"),
            *UEnum::GetValueAsString(Config.ChannelType), Config.Resolution.X, Config.Resolution.Y);
        Test->TestEqual(*FString::Printf(TEXT("%s payload type"), *Prefix),
            Payload.PayloadType, ExpectedType);
        Test->TestEqual(*FString::Printf(TEXT("%s image size"), *Prefix),
            Payload.ImageSize, Config.Resolution);
        Test->TestEqual(*FString::Printf(TEXT("%s ViewRect"), *Prefix),
            Payload.ViewRect, FIntRect(FIntPoint::ZeroValue, Config.Resolution));
        Test->TestEqual(*FString::Printf(TEXT("%s pixel format"), *Prefix),
            Payload.PixelFormat, GetExpectedPixelFormat(ExpectedType));
        Test->TestEqual(*FString::Printf(TEXT("%s color space"), *Prefix),
            Payload.ColorSpace, GetExpectedColorSpace(ExpectedType));
        Test->TestEqual(*FString::Printf(TEXT("%s value unit"), *Prefix),
            Payload.ValueUnit, GetExpectedValueUnit(ExpectedType));
        Test->TestEqual(*FString::Printf(TEXT("%s bytes per pixel"), *Prefix),
            Payload.BytesPerPixel, 4);
        Test->TestEqual(*FString::Printf(TEXT("%s tight row stride"), *Prefix),
            Payload.RowStrideBytes, Config.Resolution.X * 4);
        Test->TestEqual(*FString::Printf(TEXT("%s byte count"), *Prefix),
            Payload.Bytes.Num(), Config.Resolution.X * Config.Resolution.Y * 4);

        if (ExpectedType == EPayloadType::Rgb)
        {
            bool bSawLitColor = false;
            bool bAllAlphaOpaque = true;
            for (int32 Offset = 0; Offset + 3 < Payload.Bytes.Num(); Offset += 4)
            {
                bSawLitColor |= Payload.Bytes[Offset] != 0 ||
                    Payload.Bytes[Offset + 1] != 0 || Payload.Bytes[Offset + 2] != 0;
                bAllAlphaOpaque &= Payload.Bytes[Offset + 3] == 255;
            }
            Test->TestTrue(*FString::Printf(TEXT("%s contains rendered RGB color"), *Prefix), bSawLitColor);
            Test->TestTrue(*FString::Printf(TEXT("%s has canonical opaque alpha"), *Prefix), bAllAlphaOpaque);
        }
        else if (ExpectedType == EPayloadType::Semantic)
        {
            bool bSawExpectedLabel = false;
            bool bOnlyLegalPixels = true;
            for (int32 Offset = 0; Offset + 3 < Payload.Bytes.Num(); Offset += 4)
            {
                const uint8 Label = Payload.Bytes[Offset];
                bSawExpectedLabel |= Label == ExpectedSemanticId;
                bOnlyLegalPixels &= (Label == 0 || Label == ExpectedSemanticId) &&
                    Payload.Bytes[Offset + 1] == 0 && Payload.Bytes[Offset + 2] == 0 &&
                    Payload.Bytes[Offset + 3] == 255;
            }
            Test->TestTrue(*FString::Printf(TEXT("%s contains semantic label 17"), *Prefix), bSawExpectedLabel);
            Test->TestTrue(*FString::Printf(TEXT("%s contains no polluted labels"), *Prefix), bOnlyLegalPixels);
        }
        else if (ExpectedType == EPayloadType::Depth)
        {
            bool bSawObjectDepthMeters = false;
            bool bAllDepthFinite = true;
            for (int32 Offset = 0; Offset + 3 < Payload.Bytes.Num(); Offset += sizeof(float))
            {
                float DepthMeters = 0.0f;
                FMemory::Memcpy(&DepthMeters, Payload.Bytes.GetData() + Offset, sizeof(float));
                bAllDepthFinite &= FMath::IsFinite(DepthMeters);
                bSawObjectDepthMeters |= DepthMeters > 1.0f && DepthMeters < 4.0f;
            }
            Test->TestTrue(*FString::Printf(TEXT("%s depth values are finite"), *Prefix), bAllDepthFinite);
            Test->TestTrue(*FString::Printf(TEXT("%s contains meter-scale object depth"), *Prefix),
                bSawObjectDepthMeters);
        }
        else if (ExpectedType == EPayloadType::Instance)
        {
            bool bSawExpectedInstance = false;
            bool bOnlyLegalInstances = true;
            TMap<uint32, int32> ObservedIds;
            for (int32 Offset = 0; Offset + 3 < Payload.Bytes.Num(); Offset += sizeof(uint32))
            {
                uint32 InstanceId = 0;
                FMemory::Memcpy(&InstanceId, Payload.Bytes.GetData() + Offset, sizeof(uint32));
                bSawExpectedInstance |= InstanceId == ExpectedInstanceId;
                bOnlyLegalInstances &= InstanceId == 0 || InstanceId == ExpectedInstanceId;
                ObservedIds.FindOrAdd(InstanceId)++;
            }
            FString ObservedSummary;
            for (const TPair<uint32, int32>& Pair : ObservedIds)
            {
                ObservedSummary += FString::Printf(TEXT("%u:%d "), Pair.Key, Pair.Value);
            }
            Test->AddInfo(FString::Printf(TEXT("%s Instance pixels: %s"), *Prefix, *ObservedSummary));
            Test->TestTrue(*FString::Printf(TEXT("%s contains full uint32 InstanceId"), *Prefix),
                bSawExpectedInstance);
            Test->TestTrue(*FString::Printf(TEXT("%s contains only registered InstanceIds"), *Prefix),
                bOnlyLegalInstances);
        }
    }

    /** 超时前继续等待；超时后结束潜伏命令并报告确定性错误。 */
    bool WaitOrFail(const TCHAR* Error)
    {
        if (FPlatformTime::Seconds() - StartSeconds < 45.0)
        {
            return false;
        }
        Test->AddError(Error);
        return true;
    }

    FAutomationTestBase* Test = nullptr;
    double StartSeconds = 0.0;
    int32 WarmupFrames = 0;
    bool bCaptureSubmitted = false;
    TMap<FGuid, FCameraChannelConfig> ExpectedChannels;
    TSet<FGuid> ReceivedChannels;
};

/** PIE 关闭后同步渲染线程，确保矩阵中的全部临时资源完成释放。 */
class FFlushOutputMatrixCommand final : public IAutomationLatentCommand
{
public:
    virtual bool Update() override
    {
        FlushRenderingCommands();
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRenderOutputMatrixTest,
    "SensorSimulation.Rendering.OutputMatrix.AllModalities",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderOutputMatrixTest::RunTest(const FString& Parameters)
{
    AddExpectedError(
        TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains,
        1);

    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("An output-matrix test World is created"), World))
    {
        return false;
    }

    AActor* CameraActor = World->SpawnActor<AActor>();
    CameraActor->Tags.Add(UE::SensorSimulation::RenderOutputMatrixTests::CameraTag);
    UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(CameraActor, TEXT("OutputMatrixCameraRig"));
    CameraActor->AddInstanceComponent(Rig);
    Rig->SensorName = TEXT("OutputMatrixCamera");
    Rig->HorizontalFovDegrees = 90.0f;
    Rig->MaxPendingReadbacks = 16;
    Rig->Channels.Empty();

    const FIntPoint Resolutions[] = { FIntPoint(32, 24), FIntPoint(17, 11) };
    const ECameraChannelType Modalities[] = {
        ECameraChannelType::Rgb,
        ECameraChannelType::Semantic,
        ECameraChannelType::Depth,
        ECameraChannelType::Instance
    };
    for (const FIntPoint Resolution : Resolutions)
    {
        for (const ECameraChannelType Modality : Modalities)
        {
            FCameraChannelConfig& Channel = Rig->Channels.AddDefaulted_GetRef();
            Channel.ChannelType = Modality;
            Channel.Resolution = Resolution;
            Channel.bForceLinearGamma =
                Modality == ECameraChannelType::Semantic || Modality == ECameraChannelType::Instance;
        }
    }
    Rig->RegisterComponent();
    TestEqual(TEXT("Eight runtime channels are active"), Rig->GetEnabledImageChannels().Num(), 8);

    for (const FCameraChannelConfig& Channel : Rig->Channels)
    {
        TestTrue(TEXT("Every matrix channel receives a stable ChannelGuid"), Channel.ChannelGuid.IsValid());
        if (Channel.ChannelType == ECameraChannelType::Instance)
        {
            TestEqual(TEXT("Instance matrix channel uses PF_R32_UINT"),
                Rig->GetChannelPixelFormat(Channel.ChannelGuid), PF_R32_UINT);
        }
        else
        {
            UTextureRenderTarget2D* Target = Rig->GetChannelRenderTarget(Channel.ChannelGuid);
            if (TestNotNull(TEXT("Matrix RenderTarget is created"), Target))
            {
                const bool bExpectedLinear =
                    Channel.ChannelType == ECameraChannelType::Semantic;
                TestEqual(TEXT("RenderTarget Gamma policy matches modality"),
                    Target->bForceLinearGamma != 0, bExpectedLinear);
            }
        }
    }

    // 点光源位于相机附近，使空测试关卡中的默认材质 Cube 产生可验证的 RGB 非黑像素。
    AActor* LightActor = World->SpawnActor<AActor>();
    UPointLightComponent* Light = NewObject<UPointLightComponent>(LightActor, TEXT("OutputMatrixPointLight"));
    LightActor->SetRootComponent(Light);
    LightActor->AddInstanceComponent(Light);
    Light->SetIntensity(5000.0f);
    Light->SetAttenuationRadius(1000.0f);
    Light->RegisterComponent();

    AActor* ObjectActor = World->SpawnActor<AActor>();
    ObjectActor->Tags.Add(UE::SensorSimulation::RenderOutputMatrixTests::ObjectTag);
    UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(ObjectActor, TEXT("OutputMatrixCube"));
    ObjectActor->SetRootComponent(Mesh);
    ObjectActor->AddInstanceComponent(Mesh);
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    // Keep a stable foreground footprint even at the odd 17x11 matrix resolution.
    Mesh->SetWorldScale3D(FVector(2.5));
    Mesh->RegisterComponent();
    ObjectActor->SetActorLocation(FVector(300.0, 0.0, 0.0));

    USemanticObjectComponent* Semantic =
        NewObject<USemanticObjectComponent>(ObjectActor, TEXT("OutputMatrixSemantic"));
    Semantic->SemanticId = UE::SensorSimulation::RenderOutputMatrixTests::ExpectedSemanticId;
    ObjectActor->AddInstanceComponent(Semantic);
    Semantic->RegisterComponent();

    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::RenderOutputMatrixTests::FCaptureOutputMatrixCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::RenderOutputMatrixTests::FFlushOutputMatrixCommand());
    return true;
}

#endif
