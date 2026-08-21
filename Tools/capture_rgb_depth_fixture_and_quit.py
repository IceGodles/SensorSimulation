from pathlib import Path

import unreal


MAP_PATH = "/Game/Acceptance/Maps/L_SensorAcceptance"
CAMERA_LABEL = "A_AcceptanceCamera"
OUTPUT_DIR = Path(unreal.Paths.project_saved_dir()) / "Acceptance" / "RgbDepthFixture"


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


require(unreal.EditorLevelLibrary.load_level(MAP_PATH), f"Failed to load {MAP_PATH}")
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
camera = require(
    next((actor for actor in actors if actor.get_actor_label() == CAMERA_LABEL), None),
    f"Missing {CAMERA_LABEL}",
)
rig_class = require(
    unreal.load_class(None, "/Script/SimulationRenderer.CameraRigComponent"),
    "CameraRigComponent class was not found",
)
rig = require(camera.get_components_by_class(rig_class)[0], "CameraRigComponent is missing")

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
rig.save_rgb_debug_image()
rig.save_depth_debug_image()
rig.save_semantic_debug_image()

rgb_path = rig.get_editor_property("last_rgb_debug_image_path")
depth_path = rig.get_editor_property("last_depth_debug_image_path")
semantic_path = rig.get_editor_property("last_semantic_debug_image_path")
require(rgb_path, "RGB debug image was not saved")
require(depth_path, "Depth debug image was not saved")
require(semantic_path, "Semantic debug image was not saved")
unreal.log(
    f"[RgbDepthFixture] SUCCESS rgb={rgb_path} depth={depth_path} semantic={semantic_path}"
)
unreal.SystemLibrary.execute_console_command(None, "QUIT_EDITOR")
