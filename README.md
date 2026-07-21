# SensorSimulationHost

Independent lightweight UE5 host project for synthetic RGB, annotation, LiDAR and ground-truth dataset generation.

## Start here

1. Open `SensorSimulationHost.uproject` with the engine associated as `AutomotiveStudio_v2.0`.
2. Build the Editor target.
3. Read `Plugins/SensorSimulation/Docs/CosysDesignExtraction.md`.
4. Follow `Plugins/SensorSimulation/Docs/ImplementationRoadmap.md`.

The code is a framework, not a finished dataset generator. Interfaces that would otherwise hide blocking GPU readback or file I/O are intentionally left as explicit implementation tasks.
