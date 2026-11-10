/* pad.h with no gamepad support; a Bluetooth reader (BlueZ) is reserved. */
#include "pad.h"

int pad_read(struct pad_state *s)
{
    (void)s;
    return -1;
}
