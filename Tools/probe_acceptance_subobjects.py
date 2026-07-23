import unreal


def report(name, value):
    unreal.log(f"[AcceptanceProbe] {name}={value}")


subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
report("SubobjectDataSubsystem.methods", [name for name in dir(subsystem) if "subobject" in name.lower()])
report("gather_instance.doc", subsystem.gather_subobject_data_for_instance.__doc__)
report("add_new_subobject.doc", subsystem.add_new_subobject.__doc__)
report("AddNewSubobjectParams.doc", unreal.AddNewSubobjectParams.__doc__)
report("EditorActorSubsystem.methods", [name for name in dir(unreal.EditorActorSubsystem) if "actor" in name.lower()])
