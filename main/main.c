/* Project entry point. The whole wake -- bring-up, fetch, draw, sleep --
 * is app_cycle's, so this stays one line. */
#include "app_cycle.h"

void app_main(void)
{
    app_cycle_run();
}
