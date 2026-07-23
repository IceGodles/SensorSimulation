import unreal


MAP_PATH = "/Game/Acceptance/Maps/L_SensorAcceptance"
EXPECTED_LABELS = {
    "A_AcceptanceCamera",
    "A_Semantic_010",
    "A_Semantic_020",
    "A_Semantic_100",
    "A_Semantic_200",
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

log("SUCCESS: all acceptance actors and components are present")
