import unreal


MAP_PATH = "/Game/Acceptance/Maps/L_SensorAcceptance"
CAMERA_LABEL = "A_AcceptanceCamera"
SEMANTIC_OBJECTS = (
    ("A_Semantic_010", 10, unreal.Vector(0.0, -225.0, 50.0)),
    ("A_Semantic_020", 20, unreal.Vector(0.0, -75.0, 50.0)),
    ("A_Semantic_100", 100, unreal.Vector(0.0, 75.0, 50.0)),
    ("A_Semantic_200", 200, unreal.Vector(0.0, 225.0, 50.0)),
)


def log(message):
    unreal.log(f"[AcceptanceSetup] {message}")


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def add_component(actor, component_class, component_name):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = subsystem.k2_gather_subobject_data_for_instance(actor)
    require(handles, f"No subobject handles found for {actor.get_actor_label()}")

    params = unreal.AddNewSubobjectParams()
    params.set_editor_property("parent_handle", handles[0])
    params.set_editor_property("new_class", component_class)
    params.set_editor_property("conform_transform_to_parent", True)

    handle, fail_reason = subsystem.add_new_subobject(params)
    data_found, data = subsystem.k2_find_subobject_data_from_handle(handle)
    require(data_found, f"Failed to resolve {component_name}: {fail_reason}")
    component = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
    require(component, f"Component object is null for {component_name}: {fail_reason}")
    subsystem.rename_subobject(handle, unreal.Text(component_name))
    return component


def remove_previous_fixture(actor_subsystem):
    labels = {CAMERA_LABEL, *(entry[0] for entry in SEMANTIC_OBJECTS)}
    for actor in actor_subsystem.get_all_level_actors():
        if actor.get_actor_label() in labels:
            actor_subsystem.destroy_actor(actor)
            log(f"Removed previous actor {actor.get_actor_label()}")


def make_channel(channel_type, linear_gamma):
    channel = unreal.CameraChannelConfig()
    channel.set_editor_property("channel_type", channel_type)
    channel.set_editor_property("resolution", unreal.IntPoint(1280, 720))
    channel.set_editor_property("enabled", True)
    channel.set_editor_property("force_linear_gamma", linear_gamma)
    return channel


def create_camera(actor_subsystem):
    camera_actor = actor_subsystem.spawn_actor_from_class(
        unreal.Actor,
        unreal.Vector(-600.0, 0.0, 150.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        False,
    )
    require(camera_actor, "Failed to spawn acceptance camera actor")
    camera_actor.set_actor_label(CAMERA_LABEL)

    rig_class = require(
        unreal.load_class(None, "/Script/SimulationRenderer.CameraRigComponent"),
        "CameraRigComponent class was not found",
    )
    sensor_class = require(
        unreal.load_class(None, "/Script/SimulationRuntime.SimCameraSensorComponent"),
        "SimCameraSensorComponent class was not found",
    )

    rig = add_component(camera_actor, rig_class, "CameraRig")
    add_component(camera_actor, sensor_class, "SimCameraSensor")

    rig.set_editor_property("sensor_name", "FrontCamera")
    rig.set_editor_property("horizontal_fov_degrees", 90.0)
    rig.set_editor_property("max_pending_readbacks", 8)
    rig.set_editor_property(
        "channels",
        [
            make_channel(unreal.CameraChannelType.RGB, False),
            make_channel(unreal.CameraChannelType.SEMANTIC, True),
        ],
    )
    log("Created A_AcceptanceCamera with RGB and Semantic channels")


def create_semantic_objects(actor_subsystem):
    semantic_class = require(
        unreal.load_class(None, "/Script/SimulationRuntime.SemanticObjectComponent"),
        "SemanticObjectComponent class was not found",
    )
    cube_mesh = require(
        unreal.load_asset("/Engine/BasicShapes/Cube.Cube"),
        "Engine cube mesh was not found",
    )

    for label, semantic_id, location in SEMANTIC_OBJECTS:
        actor = actor_subsystem.spawn_actor_from_class(
            unreal.StaticMeshActor,
            location,
            unreal.Rotator(0.0, 0.0, 0.0),
            False,
        )
        require(actor, f"Failed to spawn {label}")
        actor.set_actor_label(label)

        mesh = actor.get_editor_property("static_mesh_component")
        mesh.set_static_mesh(cube_mesh)
        mesh.set_render_custom_depth(True)
        mesh.set_custom_depth_stencil_value(semantic_id)

        semantic = add_component(actor, semantic_class, "SemanticObject")
        semantic.set_editor_property("semantic_id", semantic_id)
        semantic.set_editor_property("render_to_semantic_capture", True)
        log(f"Created {label} with SemanticId={semantic_id}")


def main():
    require(unreal.EditorLevelLibrary.load_level(MAP_PATH), f"Failed to load {MAP_PATH}")
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    remove_previous_fixture(actor_subsystem)
    create_camera(actor_subsystem)
    create_semantic_objects(actor_subsystem)
    require(unreal.EditorLevelLibrary.save_current_level(), "Failed to save acceptance level")
    log("SUCCESS: acceptance fixture saved")


main()
