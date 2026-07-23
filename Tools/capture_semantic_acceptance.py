import unreal


MAP_PATH = "/Game/Acceptance/Maps/L_SensorAcceptance"
CAMERA_LABEL = "A_AcceptanceCamera"


def log(message):
    unreal.log(f"[AcceptanceCapture] {message}")


if not unreal.EditorLevelLibrary.load_level(MAP_PATH):
    raise RuntimeError(f"Failed to load {MAP_PATH}")

actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
camera = next(
    (actor for actor in actor_subsystem.get_all_level_actors() if actor.get_actor_label() == CAMERA_LABEL),
    None,
)
if camera is None:
    raise RuntimeError(f"Missing {CAMERA_LABEL}")

rig_class = unreal.load_class(None, "/Script/SimulationRenderer.CameraRigComponent")
rig_components = camera.get_components_by_class(rig_class)
if len(rig_components) != 1:
    raise RuntimeError(f"Expected one CameraRigComponent, found {len(rig_components)}")

rig = rig_components[0]
rig.save_semantic_debug_image()
output_path = rig.get_editor_property("last_semantic_debug_image_path")
if not output_path:
    raise RuntimeError("Semantic debug capture did not produce an output path")

log(f"SUCCESS: {output_path}")
