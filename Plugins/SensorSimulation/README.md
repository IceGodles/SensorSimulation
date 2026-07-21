# SensorSimulation

Lightweight UE5 synthetic sensor framework derived from design study of Cosys-AirSim. No Cosys-AirSim source is vendored into this plugin.

## Modules

- `SensorSimulationCore`: stable plain-data protocol, coordinate conversion and scan contracts.
- `SensorSimulationRenderer`: camera channel organization, render targets and asynchronous readback boundary.
- `SensorSimulationRuntime`: world clock, sensor components, semantic registry, LiDAR scan assembly and export boundary.
- `SensorSimulationEditor`: editor-only visualization and future authoring tools.

See `Docs/CosysDesignExtraction.md` and `Docs/ImplementationRoadmap.md`.
