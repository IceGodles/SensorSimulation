import unreal


def report(name, value):
    unreal.log(f"[AcceptanceProbe] {name}={value}")


report("Actor.add_component_by_class", hasattr(unreal.Actor, "add_component_by_class"))
report("EditorLevelLibrary", hasattr(unreal, "EditorLevelLibrary"))
report("EditorActorSubsystem", hasattr(unreal, "EditorActorSubsystem"))
report("EditorLoadingAndSavingUtils", hasattr(unreal, "EditorLoadingAndSavingUtils"))

for class_path in (
    "/Script/SimulationRenderer.CameraRigComponent",
    "/Script/SimulationRuntime.SimCameraSensorComponent",
    "/Script/SimulationRuntime.SemanticObjectComponent",
):
    report(class_path, unreal.load_class(None, class_path))

for type_name in ("CameraChannelConfig", "CameraChannelType"):
    report(type_name, getattr(unreal, type_name, None))
