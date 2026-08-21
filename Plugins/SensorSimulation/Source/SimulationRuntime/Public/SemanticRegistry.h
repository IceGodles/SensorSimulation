#pragma once

#include "CoreMinimal.h"

class AActor;
class USemanticObjectComponent;

/** 维护 Actor 到语义组件映射并分配实例编号的注册表。 */
class SIMULATIONRUNTIME_API FSemanticRegistry
{
public:
/** 为语义对象分配唯一实例编号并建立 Actor 到组件的弱引用映射。 */
    uint32 Register(USemanticObjectComponent& Component);
/** 从语义注册表移除指定组件所属 Actor。 */
    void Unregister(const USemanticObjectComponent& Component);
/** 查询 Actor 对应且仍然有效的语义组件。 */
    const USemanticObjectComponent* Find(const AActor* Actor) const;
/** 清空语义映射并重置实例编号生成器。 */
    void Reset();
    /** 收集当前注册对象可由 8 位 Semantic 图像编码的合法标签集合。 */
    void GetImageSemanticIds(TSet<uint8>& OutIds) const;
    /** 收集背景 0 与 LiDAR 可编码的完整 16 位语义标签集合。 */
    void GetLidarSemanticIds(TSet<uint16>& OutIds) const;
    /** 收集背景 0 与当前注册对象的完整 32 位合法 InstanceId 集合。 */
    void GetInstanceIds(TSet<uint32>& OutIds) const;

private:
    /** 下一次注册语义对象时分配的实例编号。 */
    uint64 NextInstanceId = 1;
    /** Actor 到语义组件的弱引用映射，避免注册表延长 UObject 生命周期。 */
    TMap<TWeakObjectPtr<const AActor>, TWeakObjectPtr<USemanticObjectComponent>> Entries;
};
