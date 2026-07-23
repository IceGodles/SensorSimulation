import unreal


def report(name, value):
    text = str(value).replace("\n", " | ")
    unreal.log(f"[AcceptanceProbe] {name}={text}")


subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
for name in ("k2_gather_subobject_data_for_instance", "add_new_subobject", "rename_subobject"):
    report(f"{name}.doc", getattr(subsystem, name).__doc__)
report("AddNewSubobjectParams.doc", unreal.AddNewSubobjectParams.__doc__)
actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
report("spawn_actor.doc", actor_subsystem.spawn_actor_from_class.__doc__)
