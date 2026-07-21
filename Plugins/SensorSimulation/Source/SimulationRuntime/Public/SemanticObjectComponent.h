#pragma once

#include "Components/ActorComponent.h"
#include "SemanticObjectComponent.generated.h"

UCLASS(ClassGroup=(SensorSimulation), meta=(BlueprintSpawnableComponent))
/** 语义组件：
 *  为 Actor 提供语义类别、实例编号和自定义深度标记。 */
class SIMULATIONRUNTIME_API USemanticObjectComponent : public UActorComponent
{
    GENERATED_BODY()

public:
/** 构造并初始化 USemanticObjectComponent 的默认状态。 */
    USemanticObjectComponent();

    /** 对象或回波点所属的语义类别编号。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Semantic", meta=(ClampMin="0", ClampMax="65535"))
    int32 SemanticId = 0;

    /** 对象在当前仿真会话中唯一的实例编号。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Semantic")
    int32 InstanceId = 0;

    /** 控制是否把所属 Actor 的图元写入自定义深度/模板缓冲。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Semantic")
    bool bRenderToSemanticCapture = true;

/** 在组件开始运行时完成注册或初始化工作。 */
    virtual void BeginPlay() override;
/** 在组件停止运行时注销自身并释放关联关系。 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

/** 保存语义注册表分配的会话内实例编号。 */
    void SetAssignedInstanceId(uint32 InInstanceId);
};
