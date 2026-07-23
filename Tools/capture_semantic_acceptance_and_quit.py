import runpy
import unreal


runpy.run_path(
    r"D:\ueprojects\SensorSimulationHost\Tools\capture_semantic_acceptance.py",
    run_name="__main__",
)
unreal.SystemLibrary.execute_console_command(None, "QUIT_EDITOR")
