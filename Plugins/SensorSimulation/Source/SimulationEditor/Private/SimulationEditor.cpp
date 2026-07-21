#include "Modules/ModuleManager.h"

/** SensorSimulation 编辑器扩展的模块入口。 */
class FSimulationEditorModule final : public IModuleInterface
{
};

IMPLEMENT_MODULE(FSimulationEditorModule, SimulationEditor);
