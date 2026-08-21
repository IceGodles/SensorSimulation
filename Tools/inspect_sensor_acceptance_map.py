import unreal


MAP_PATH = "/Game/Acceptance/Maps/L_SensorAcceptance"
EXPECTED_LABELS = {
    "A_AcceptanceCamera",
    "A_Semantic_010",
    "A_Semantic_020",
    "A_Semantic_100",
    "A_Semantic_200",
    "A_Depth_0550",
    "A_Depth_0950",
    "A_Depth_1450",
    "A_AcceptanceRoad",
    "A_AcceptanceBackWall",
    "A_AcceptanceLeftWall",
    "A_AcceptanceRightWall",
    "A_AcceptanceHISM",
}


def log(message):
    unreal.log(f"[AcceptanceInspect] {message}")


if not unreal.EditorLevelLibrary.load_level(MAP_PATH):
    raise RuntimeError(f"Failed to load {MAP_PATH}")

actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
found = {}
for actor in actor_subsystem.get_all_level_actors():
    label = actor.get_actor_label()
    if label in EXPECTED_LABELS:
        component_classes = [component.get_class().get_name() for component in actor.get_components_by_class(unreal.ActorComponent)]
        found[label] = component_classes
        log(f"{label}: components={component_classes}")

missing = sorted(EXPECTED_LABELS - set(found))
if missing:
    raise RuntimeError(f"Missing acceptance actors: {missing}")

camera_components = set(found["A_AcceptanceCamera"])
required_camera_components = {
    "CameraRigComponent",
    "SimCameraSensorComponent",
    "SimLidarSensorComponent",
}
missing_components = sorted(required_camera_components - camera_components)
if missing_components:
    raise RuntimeError(f"Acceptance camera is missing components: {missing_components}")

if "HierarchicalInstancedStaticMeshComponent" not in found["A_AcceptanceHISM"]:
    raise RuntimeError("A_AcceptanceHISM has no HISM component")

log("SUCCESS: all acceptance actors and components are present")
