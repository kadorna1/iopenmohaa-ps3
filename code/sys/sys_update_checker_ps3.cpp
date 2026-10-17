/* OpenMoHAA PS3/PSL1GHT update-checker stub. */

#include "sys_update_checker.h"

UpdateChecker updateChecker;

extern "C" void Sys_UpdateChecker_Init()
{
    updateChecker.Init();
}

extern "C" void Sys_UpdateChecker_Process()
{
    updateChecker.Process();
}

extern "C" void Sys_UpdateChecker_Shutdown()
{
    updateChecker.Shutdown();
}
