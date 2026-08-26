import unreal


SOURCE_MAP = "/Game/Downtown_West/Maps/Demo_Environment"
TARGET_MAP = "/Game/RealisticValidation/Maps/L_RealisticUrbanValidation"
VEHICLE_CLASS = "/Script/SimulationRuntime.SensorVehicleActor"
VEHICLE_LABEL = "Realistic_EgoSensorVehicle"

SEMANTIC_CLASSES = {
    "road": 1,
    "building": 5,
    "vegetation": 20,
    "street_furniture": 30,
}


def require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def log(message):
    unreal.log(f"[RealisticSetup] {message}")


def add_component(actor, component_class, name):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = subsystem.k2_gather_subobject_data_for_instance(actor)
    params = unreal.AddNewSubobjectParams()
    params.set_editor_property("parent_handle", handles[0])
    params.set_editor_property("new_class", component_class)
    params.set_editor_property("conform_transform_to_parent", True)
    handle, reason = subsystem.add_new_subobject(params)
    result = subsystem.k2_find_subobject_data_from_handle(handle)
    data = result[1] if isinstance(result, tuple) else result
    component = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
    require(component, f"Unable to add {name}: {reason}")
    subsystem.rename_subobject(handle, unreal.Text(name))
    return component


def classify_mesh_path(mesh_path):
    path = mesh_path.lower()
    if any(token in path for token in ("/ground/", "/walkway/", "/roads/", "road_", "street_")):
        return "road", SEMANTIC_CLASSES["road"]
    if "/buildings/" in path or "/building_" in path:
        return "building", SEMANTIC_CLASSES["building"]
    if any(token in path for token in ("/foliage/", "tree", "shrub", "plant", "grass")):
        return "vegetation", SEMANTIC_CLASSES["vegetation"]
    if "/props/" in path:
        return "street_furniture", SEMANTIC_CLASSES["street_furniture"]
    return None


def apply_semantic_labels(actors):
    semantic_class = require(
        unreal.load_class(None, "/Script/SimulationRuntime.SemanticObjectComponent"),
        "SemanticObjectComponent unavailable",
    )
    counts = {name: 0 for name in SEMANTIC_CLASSES}
    for actor in actors:
        if isinstance(actor, unreal.Landscape):
            semantic = actor.get_component_by_class(semantic_class)
            if not semantic:
                semantic = add_component(actor, semantic_class, "SemanticObject")
            semantic.set_editor_property("semantic_id", SEMANTIC_CLASSES["road"])
            semantic.set_editor_property("render_to_semantic_capture", True)
            # Landscape is labelled semantically but is not submitted to the
            # static-mesh-only 32-bit instance pass.
            semantic.set_editor_property("render_to_instance_capture", False)
            semantic.apply_capture_configuration()
            counts["road"] += 1
            continue
        mesh_component = actor.get_component_by_class(unreal.StaticMeshComponent)
        if not mesh_component:
            continue
        mesh = mesh_component.get_editor_property("static_mesh")
        if not mesh:
            continue
        classification = classify_mesh_path(mesh.get_path_name())
        if not classification:
            continue
        class_name, semantic_id = classification
        semantic = actor.get_component_by_class(semantic_class)
        if not semantic:
            semantic = add_component(actor, semantic_class, "SemanticObject")
        semantic.set_editor_property("semantic_id", semantic_id)
        semantic.set_editor_property("render_to_semantic_capture", True)
        semantic.set_editor_property("render_to_instance_capture", True)
        semantic.apply_capture_configuration()
        counts[class_name] += 1
    return counts


def bootstrap_map():
    if unreal.EditorAssetLibrary.does_asset_exist(TARGET_MAP):
        return True
    require(
        unreal.EditorAssetLibrary.duplicate_asset(SOURCE_MAP, TARGET_MAP),
        f"Unable to duplicate {SOURCE_MAP}",
    )
    require(
        unreal.EditorAssetLibrary.save_asset(TARGET_MAP, False),
        f"Unable to save {TARGET_MAP}",
    )
    # UE may retain both UWorld objects until Python returns. Edit on a second run.
    log(f"BOOTSTRAP: created {TARGET_MAP}; run this script once more to populate it")
    return False


def main():
    if not bootstrap_map():
        return

    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    require(level_subsystem.load_level(TARGET_MAP), f"Unable to load {TARGET_MAP}")
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = actor_subsystem.get_all_level_actors()

    for actor in actors:
        if actor.get_actor_label() == VEHICLE_LABEL:
            actor_subsystem.destroy_actor(actor)

    # Marketplace environments often reuse CustomDepth/Stencil for visual effects.
    # SensorSimulation interprets the same stencil byte as a semantic class ID, so
    # clear unregistered authoring values in this validation copy. Objects explicitly
    # tagged with SemanticObjectComponent can be added later without changing source assets.
    sanitized_components = 0
    for actor in actors:
        for component in actor.get_components_by_class(unreal.PrimitiveComponent):
            component.set_render_custom_depth(False)
            component.set_custom_depth_stencil_value(0)
            sanitized_components += 1

    player_start = next(
        (actor for actor in actors if actor.get_class().get_name() == "PlayerStart"),
        None,
    )
    spawn_location = (
        player_start.get_actor_location() if player_start else unreal.Vector(0.0, 0.0, 102.0)
    )
    spawn_rotation = (
        player_start.get_actor_rotation() if player_start else unreal.Rotator(0.0, 0.0, 0.0)
    )

    # The Downtown West pack contains no suitable realistic ego vehicle. Keep the
    # visual mesh empty: the actor still provides physically meaningful camera and
    # roof-LiDAR extrinsics without mixing the previous stylised Kenney vehicle in.
    vehicle_class = require(unreal.load_class(None, VEHICLE_CLASS), "SensorVehicleActor unavailable")
    vehicle = require(
        actor_subsystem.spawn_actor_from_class(
            vehicle_class, spawn_location, spawn_rotation, False
        ),
        "Unable to spawn SensorVehicleActor",
    )
    vehicle.set_actor_label(VEHICLE_LABEL)

    if player_start:
        actor_subsystem.destroy_actor(player_start)

    semantic_counts = apply_semantic_labels(actor_subsystem.get_all_level_actors())

    require(unreal.EditorLevelLibrary.save_current_level(), "Unable to save realistic map")
    log(
        f"SUCCESS: vehicle={spawn_location} rotation={spawn_rotation} "
        f"actors={len(actor_subsystem.get_all_level_actors())} "
        f"sanitized_custom_depth={sanitized_components} semantic={semantic_counts}"
    )


main()
