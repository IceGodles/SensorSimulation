#pragma once

#include "Components/ActorComponent.h"
#include "SemanticObjectComponent.generated.h"

class UPrimitiveComponent;

/** 半透明对象在单层 Semantic/Instance 标签中的产品策略。 */
UENUM(BlueprintType)
enum class ETranslucentLabelPolicy : uint8
{
    /** 忽略透明表面，让其后的最近受支持不透明对象获得标签。 */
    Ignore UMETA(DisplayName="Ignore Translucent Surface"),
    /** 使用显式不透明代理表示玻璃、灯罩或透明防护罩自身。 */
    OpaqueProxy UMETA(DisplayName="Use Opaque Label Proxy")
};

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

    /** 决定透明表面是被忽略，还是由显式不透明代理写入标签。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Semantic|Translucent")
    ETranslucentLabelPolicy TranslucentLabelPolicy = ETranslucentLabelPolicy::Ignore;

    /**
     * OpaqueProxy 策略使用的同 Actor 不透明图元。
     * 代理继承 SemanticId/InstanceId，只参与 Semantic/Instance 标签捕获。
     */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Semantic|Translucent", meta=(UseComponentPicker, AllowedClasses="/Script/Engine.PrimitiveComponent", EditCondition="TranslucentLabelPolicy==ETranslucentLabelPolicy::OpaqueProxy"))
    TObjectPtr<UPrimitiveComponent> OpaqueLabelProxy = nullptr;

    /** 代理与透明源包围盒允许的相对偏差；超出后编辑器数据验证给出警告。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Semantic|Translucent", meta=(ClampMin="0.0", ClampMax="1.0", EditCondition="TranslucentLabelPolicy==ETranslucentLabelPolicy::OpaqueProxy"))
    float OpaqueProxyBoundsTolerance = 0.2f;

    /** 返回当前 OpaqueProxy 配置是否可安全参与标签捕获。 */
    UFUNCTION(BlueprintPure, Category="Semantic|Translucent")
    bool HasValidOpaqueLabelProxy() const;

    /** 应用运行时修改后的标签、透明策略和代理配置，供蓝图或 C++ 热切换调用。 */
    UFUNCTION(BlueprintCallable, Category="Semantic")
    void ApplyCaptureConfiguration();

#if WITH_EDITOR
    /** 在编辑器保存、提交或人工校验时检查 OpaqueProxy 产品配置。 */
    virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif

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
    /** 应用或撤销 OpaqueProxy 的标签专用可见性与 Renderer 登记。 */
    void ApplyOpaqueLabelProxyState();
    /** 恢复上一个代理被接管前的可见性并从 Renderer 注销。 */
    void ReleaseOpaqueLabelProxyState();
    /** Actor 和 ISM/HISM 内部实例共同占用的连续 InstanceId 数量。 */
    uint32 AllocatedInstanceIdCount = 1;
    /** 最近一次由本组件接管的代理，用于热更新时精确恢复旧对象。 */
    TWeakObjectPtr<UPrimitiveComponent> AppliedOpaqueLabelProxy;
    /** 代理接管前是否已经是“仅 SceneCapture 可见”。 */
    bool bAppliedProxyWasCaptureOnly = false;
};
