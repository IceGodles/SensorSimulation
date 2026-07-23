import os
import unreal


MAP_PATH = "/Game/Acceptance/Maps/L_SensorAcceptance"
CAMERA_LABEL = "A_AcceptanceCamera"


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


width = int(os.environ.get("ACCEPTANCE_WIDTH", "641"))
height = int(os.environ.get("ACCEPTANCE_HEIGHT", "359"))
require(width > 0 and height > 0, "Resolution must be positive")

require(unreal.EditorLevelLibrary.load_level(MAP_PATH), f"Failed to load {MAP_PATH}")
actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
camera = next(
    (actor for actor in actor_subsystem.get_all_level_actors() if actor.get_actor_label() == CAMERA_LABEL),
    None,
)
require(camera, f"Missing {CAMERA_LABEL}")

rig_class = unreal.load_class(None, "/Script/SimulationRenderer.CameraRigComponent")
rig_components = camera.get_components_by_class(rig_class)
require(len(rig_components) == 1, f"Expected one CameraRigComponent, found {len(rig_components)}")
rig = rig_components[0]

channels = list(rig.get_editor_property("channels"))
require(channels, "Camera has no configured channels")
for channel in channels:
    channel.set_editor_property("resolution", unreal.IntPoint(width, height))
rig.set_editor_property("channels", channels)

rig.save_semantic_debug_image()
output_path = rig.get_editor_property("last_semantic_debug_image_path")
require(output_path, "Semantic debug capture did not produce an output path")
unreal.log(f"[AcceptanceResolution] SUCCESS {width}x{height}: {output_path}")
unreal.SystemLibrary.execute_console_command(None, "QUIT_EDITOR")
