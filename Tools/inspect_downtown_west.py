import collections
import unreal

MAP_PATH = "/Game/Downtown_West/Maps/Demo_Environment"

level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not level_subsystem.load_level(MAP_PATH):
    raise RuntimeError(f"Unable to load {MAP_PATH}")

actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = actor_subsystem.get_all_level_actors()
classes = collections.Counter(actor.get_class().get_name() for actor in actors)
unreal.log(f"[DowntownInspect] actors={len(actors)}")
for class_name, count in classes.most_common():
    unreal.log(f"[DowntownInspect] class={class_name} count={count}")
for actor in actors:
    if "player" in actor.get_actor_label().lower() or "start" in actor.get_actor_label().lower():
        unreal.log(
            f"[DowntownInspect] anchor={actor.get_actor_label()} "
            f"location={actor.get_actor_location()} rotation={actor.get_actor_rotation()}"
        )

anchor = next(
    (actor.get_actor_location() for actor in actors if actor.get_class().get_name() == "PlayerStart"),
    unreal.Vector(0.0, 0.0, 0.0),
)
nearby = []
for actor in actors:
    mesh_component = actor.get_component_by_class(unreal.StaticMeshComponent)
    sample_location = (
        mesh_component.get_world_location() if mesh_component else actor.get_actor_location()
    )
    distance = (sample_location - anchor).length()
    if distance < 5000.0:
        nearby.append((distance, actor, mesh_component, sample_location))

nearby.sort(key=lambda item: item[0])
for distance, actor, mesh_component, sample_location in nearby[:120]:
    mesh_path = ""
    if mesh_component and mesh_component.get_editor_property("static_mesh"):
        mesh_path = mesh_component.get_editor_property("static_mesh").get_path_name()
    unreal.log(
        f"[DowntownNearby] distance={distance:.1f} label={actor.get_actor_label()} "
        f"class={actor.get_class().get_name()} location={sample_location} mesh={mesh_path}"
    )
unreal.SystemLibrary.quit_editor()
