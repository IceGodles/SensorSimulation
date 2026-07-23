from pathlib import Path


source_path = Path(__file__).with_name("setup_sensor_acceptance_map.py")
source = source_path.read_text(encoding="utf-8")
source = source.replace(
    "    data_found, data = subsystem.k2_find_subobject_data_from_handle(handle)\n"
    "    require(data_found, f\"Failed to resolve {component_name}: {fail_reason}\")\n",
    "    data = subsystem.k2_find_subobject_data_from_handle(handle)\n",
)
exec(compile(source, str(source_path), "exec"), {"__name__": "__main__"})
