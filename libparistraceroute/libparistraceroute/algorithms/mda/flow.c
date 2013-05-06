#include <stdlib.h>
#include <errno.h>
#include "flow.h"

mda_flow_t * mda_flow_create(uintmax_t flow_id, mda_flow_state_t state)
{
    mda_flow_t * flow;

    if ((flow = malloc(sizeof(mda_flow_t)))) {
        flow->flow_id = flow_id;
        flow->state = state;
    }
    return flow;
}

void mda_flow_free(mda_flow_t * flow)
{
    if (flow) free(flow);
}
