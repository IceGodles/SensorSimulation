#include "SensorVehicleActor.h"

#include "CameraChannel.h"
#include "CameraRigComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "SimCameraSensorComponent.h"
#include "SimLidarSensorComponent.h"

ASensorVehicleActor::ASensorVehicleActor()
{
    PrimaryActorTick.bCanEverTick = false;
    VehicleRoot = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
    SetRootComponent(VehicleRoot);
    VehicleMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VehicleMesh"));
    VehicleMesh->SetupAttachment(VehicleRoot);

    CameraMount = CreateDefaultSubobject<USceneComponent>(TEXT("CameraMount"));
    CameraMount->SetupAttachment(VehicleRoot);
    CameraMount->SetRelativeLocation(FVector(150.0, 0.0, 140.0));
    CameraRig = CreateDefaultSubobject<UCameraRigComponent>(TEXT("CameraRig"));
    CameraRig->SetupAttachment(CameraMount);
    CameraRig->SensorName = TEXT("VehicleFrontCamera");
    CameraRig->HorizontalFovDegrees = 90.0f;
    CameraRig->MaxPendingReadbacks = 8;
    // UCameraRigComponent ships with RGB + Semantic defaults for standalone use.
    // This platform declares its complete channel set explicitly, so discard them
    // before adding the four validation channels.
    CameraRig->Channels.Reset();
    auto AddChannel = [this](const ECameraChannelType Type, const bool bLinear)
    {
        FCameraChannelConfig& Channel = CameraRig->Channels.AddDefaulted_GetRef();
        Channel.ChannelType = Type;
        Channel.Resolution = FIntPoint(1280, 720);
        Channel.bEnabled = true;
        Channel.bForceLinearGamma = bLinear;
    };
    AddChannel(ECameraChannelType::Rgb, false);
    AddChannel(ECameraChannelType::Semantic, true);
    AddChannel(ECameraChannelType::Depth, true);
    AddChannel(ECameraChannelType::Instance, true);
    CameraSensor = CreateDefaultSubobject<USimCameraSensorComponent>(TEXT("CameraSensor"));
    CameraSensor->CameraRig = CameraRig;
    CameraSensor->UpdateFrequencyHz = 20.0f;

    LidarMount = CreateDefaultSubobject<USceneComponent>(TEXT("LidarMount"));
    LidarMount->SetupAttachment(VehicleRoot);
    LidarMount->SetRelativeLocation(FVector(0.0, 0.0, 180.0));
    LidarSensor = CreateDefaultSubobject<USimLidarSensorComponent>(TEXT("LidarSensor"));
    LidarSensor->SensorName = TEXT("VehicleRoofLidar");
    LidarSensor->UpdateFrequencyHz = 10.0f;
    LidarSensor->SensorMount = LidarMount;
    LidarSensor->Config.Channels = 16;
    LidarSensor->Config.HorizontalSamples = 512;
    LidarSensor->Config.RaysPerTick = 2048;
}
