#include "sync_core.h"
#include "sync_app.h"


#include "cmsis_os.h"

/**
  * @brief  Function implementing the SyncTask thread.
  * @param  argument: Not used
  * @retval None
  */
void Sync_Task(void const * argument)
{
    (void)argument;

    for(;;)
    {
        // todo: fill the code
        osDelay(1);
    }
}
