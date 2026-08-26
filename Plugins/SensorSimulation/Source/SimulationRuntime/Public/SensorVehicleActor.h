#pragma once

#include "GameFramework/Actor.h"
#include "SensorVehicleActor.generated.h"

class UCameraRigComponent;
class USceneComponent;
class USimCameraSensorComponent;
class USimLidarSensorComponent;
class UStaticMeshComponent;

UCLASS(BlueprintType)
/** 可直接放入真实场景的车辆传感器平台，空间外参由 SceneComponent 层级定义。 */
class SIMULATIONRUNTIME_API ASensorVehicleActor : public AActor
{
    GENERATED_BODY()
public:
    ASensorVehicleActor();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Vehicle") TObjectPtr<USceneComponent> VehicleRoot;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Vehicle") TObjectPtr<UStaticMeshComponent> VehicleMesh;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sensors") TObjectPtr<USceneComponent> CameraMount;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sensors") TObjectPtr<USceneComponent> LidarMount;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sensors") TObjectPtr<UCameraRigComponent> CameraRig;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sensors") TObjectPtr<USimCameraSensorComponent> CameraSensor;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sensors") TObjectPtr<USimLidarSensorComponent> LidarSensor;
};
