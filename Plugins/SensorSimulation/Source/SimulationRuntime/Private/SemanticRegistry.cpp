#include "SemanticRegistry.h"
#include "SemanticImageLabel.h"
#include "SemanticObjectComponent.h"
#include "GameFramework/Actor.h"

/** 为语义对象分配唯一实例编号并建立 Actor 到组件的弱引用映射。 */
uint32 FSemanticRegistry::Register(USemanticObjectComponent& Component)
{
    const uint32 RequiredCount = Component.GetRequiredInstanceIdCount();
    const uint64 EndExclusive = NextInstanceId + RequiredCount;
    if (RequiredCount == 0 || EndExclusive > static_cast<uint64>(MAX_uint32) + 1u)
    {
        UE_LOG(LogTemp, Error,
            TEXT("InstanceId namespace exhausted while registering '%s' (%u IDs requested)."),
            *GetNameSafe(Component.GetOwner()),
            RequiredCount);
        Component.SetAssignedInstanceId(0);
        return 0;
    }

    const uint32 InstanceId = static_cast<uint32>(NextInstanceId);
    NextInstanceId = EndExclusive;
    Component.SetAssignedInstanceId(InstanceId, RequiredCount);
    Entries.Add(Component.GetOwner(), &Component);
    return InstanceId;
}

/** 从语义注册表移除指定组件所属 Actor。 */
void FSemanticRegistry::Unregister(const USemanticObjectComponent& Component)
{
    Entries.Remove(Component.GetOwner());
}

/** 查询 Actor 对应且仍然有效的语义组件。 */
const USemanticObjectComponent* FSemanticRegistry::Find(const AActor* Actor) const
{
    const TWeakObjectPtr<USemanticObjectComponent>* Found = Entries.Find(Actor);
    return Found && Found->IsValid() ? Found->Get() : nullptr;
}

/** 收集背景 0 与所有仍有效对象的 8 位图像语义标签。 */
void FSemanticRegistry::GetImageSemanticIds(TSet<uint8>& OutIds) const
{
    OutIds.Reset();
    OutIds.Add(static_cast<uint8>(UE::SensorSimulation::SemanticLabels::BackgroundId));
    for (const TPair<TWeakObjectPtr<const AActor>, TWeakObjectPtr<USemanticObjectComponent>>& Entry : Entries)
    {
        if (const USemanticObjectComponent* Component = Entry.Value.Get())
        {
            uint8 ImageId = 0;
            if (UE::SensorSimulation::SemanticLabels::TryConvertToImageId(Component->SemanticId, ImageId))
            {
                OutIds.Add(ImageId);
            }
        }
    }
}

/** 收集背景 0 与所有仍有效对象的完整 32 位实例编号。 */
void FSemanticRegistry::GetInstanceIds(TSet<uint32>& OutIds) const
{
    OutIds.Reset();
    OutIds.Add(0);
    for (const TPair<TWeakObjectPtr<const AActor>, TWeakObjectPtr<USemanticObjectComponent>>& Entry : Entries)
    {
        if (const USemanticObjectComponent* Component = Entry.Value.Get())
        {
            const uint32 InstanceId = static_cast<uint32>(Component->InstanceId);
            const uint32 Count = Component->GetAllocatedInstanceIdCount();
            for (uint32 Offset = 0; InstanceId != 0 && Offset < Count; ++Offset)
            {
                OutIds.Add(InstanceId + Offset);
            }
        }
    }
}

/** 清空语义映射并重置实例编号生成器。 */
void FSemanticRegistry::Reset()
{
    Entries.Reset();
    NextInstanceId = 1;
}
