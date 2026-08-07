import unreal


MAP_PATH = "/Game/Acceptance/Maps/L_SensorAcceptance"
CAMERA_LABEL = "A_AcceptanceCamera"
CUBE_LABELS = {
    "A_Semantic_010",
    "A_Semantic_020",
    "A_Semantic_100",
    "A_Semantic_200",
}


def log(message):
    unreal.log(f"[FixtureInspect] {message}")


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


require(unreal.EditorLevelLibrary.load_level(MAP_PATH), f"Failed to load {MAP_PATH}")
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

for actor in actors:
    if actor.get_actor_label() not in CUBE_LABELS:
        continue
    mesh = actor.get_editor_property("static_mesh_component")
    material = mesh.get_material(0)
    log(
        f"cube={actor.get_actor_label()} "
        f"material={material.get_path_name() if material else 'None'}"
    )
    if isinstance(material, unreal.Material):
        log(
            f"material={material.get_name()} "
            f"domain={material.get_editor_property('material_domain')} "
            f"shading={material.get_editor_property('shading_model')}"
        )

camera = require(
    next((actor for actor in actors if actor.get_actor_label() == CAMERA_LABEL), None),
    f"Missing {CAMERA_LABEL}",
)
rig_class = require(
    unreal.load_class(None, "/Script/SimulationRenderer.CameraRigComponent"),
    "CameraRigComponent class was not found",
)
rig = require(camera.get_components_by_class(rig_class)[0], "CameraRigComponent is missing")
for channel_type in (
    unreal.CameraChannelType.RGB,
    unreal.CameraChannelType.SEMANTIC,
    unreal.CameraChannelType.DEPTH,
):
    target = rig.get_channel_render_target(channel_type)
    require(target, f"Missing target for {channel_type}")
    log(
        f"channel={channel_type} size={target.get_editor_property('size_x')}x"
        f"{target.get_editor_property('size_y')} "
        f"rt_format={target.get_editor_property('render_target_format')} "
        f"pixel_format={target.get_format()} "
        f"linear={target.get_editor_property('force_linear_gamma')}"
    )

log("SUCCESS")
unreal.SystemLibrary.execute_console_command(None, "QUIT_EDITOR")
