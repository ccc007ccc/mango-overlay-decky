#include "mango_overlay/client.h"

#include <stddef.h>

int main(void)
{
    mango_overlay_client* client = NULL;
    mango_overlay_client_config config = { 0 };
    config.struct_size = sizeof(config);
    return mango_overlay_client_open(&config, &client) == MANGO_OVERLAY_INVALID_ARGUMENT
        && client == NULL
        ? 0
        : 1;
}
