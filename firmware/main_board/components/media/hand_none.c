/* hand.h with no hand model. */
#include "hand.h"

int hand_open(void)
{
    return -1;
}

int hand_read(struct hand *hands, int max)
{
    (void)hands;
    (void)max;
    return -1;
}
