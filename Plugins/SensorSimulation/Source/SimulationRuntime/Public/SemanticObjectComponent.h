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

    /**
     * 对象在当前仿真会话中唯一的实例编号。
     *
     * UPROPERTY 使用 int64 是因为反射系统没有 uint32 属性类型；有效范围仍严格为
     * 0..UINT32_MAX，渲染、协议、LiDAR 和 Ground Truth 边界均转换为 uint32。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Semantic", meta=(ClampMin="0", ClampMax="4294967295"))
    int64 InstanceId = 0;

    /** 控制是否把所属 Actor 的图元写入自定义深度/模板缓冲。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Semantic")
    bool bRenderToSemanticCapture = true;

/**
     * 控制是否把所属 Actor 的图元登记到独立 32 位 Instance Mesh Pass。
     *
     * 此开关不使用 CustomStencil；关闭后该对象在 Instance 图中表现为背景 0。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Semantic")
    bool bRenderToInstanceCapture = true;

/** 组件注册时立即把语义/实例捕获状态同步到所属 Actor 的图元。 */
    virtual void OnRegister() override;
/** 组件注销前移除图元到 InstanceId 的非拥有型映射，防止 SceneProxy 身份悬空。 */
    virtual void OnUnregister() override;
/** 在组件开始运行时向世界子系统注册语义对象。 */
    virtual void BeginPlay() override;
/** 在组件停止运行时注销自身并释放关联关系。 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

/** 保存语义注册表分配的会话内实例编号。 */
    void SetAssignedInstanceId(uint32 InInstanceId, uint32 InAllocatedInstanceIdCount = 1);
    /** 返回 Actor 本体和全部 ISM/HISM 内部实例所需的连续 ID 数量。 */
    uint32 GetRequiredInstanceIdCount() const;
    /** 返回当前已分配区间长度，供 Payload 合法值校验使用。 */
    uint32 GetAllocatedInstanceIdCount() const { return AllocatedInstanceIdCount; }

#if WITH_EDITOR
    /** 编辑器修改语义属性后立即刷新图元的 CustomStencil 状态。 */
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    /** 将 Semantic CustomStencil 与独立 Instance Mesh Pass 身份同步到全部图元。 */
    void ApplyCaptureRenderState();
    /** 从独立 Instance 注册表注销所属 Actor 的全部图元。 */
    void UnregisterInstancePrimitives();
    /** Actor 和 ISM/HISM 内部实例共同占用的连续 InstanceId 数量。 */
    uint32 AllocatedInstanceIdCount = 1;
};
