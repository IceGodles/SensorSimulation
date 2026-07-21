#include "SemanticRegistry.h"
#include "SemanticObjectComponent.h"
#include "GameFramework/Actor.h"

/** 为语义对象分配唯一实例编号并建立 Actor 到组件的弱引用映射。 */
uint32 FSemanticRegistry::Register(USemanticObjectComponent& Component)
{
    const uint32 InstanceId = NextInstanceId++;
    Component.SetAssignedInstanceId(InstanceId);
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

/** 清空语义映射并重置实例编号生成器。 */
void FSemanticRegistry::Reset()
{
    Entries.Reset();
    NextInstanceId = 1;
}
