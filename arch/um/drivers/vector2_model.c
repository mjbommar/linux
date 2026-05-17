// SPDX-License-Identifier: GPL-2.0
/*
 * Explicit state transitions for UML vector networking v2.
 */

#include <linux/errno.h>
#include <linux/string.h>

#include "vector2_model.h"

void um_vec2_dev_lifecycle_init(struct um_vec2_dev_lifecycle *life)
{
	life->state = UM_VEC2_DEV_NEW;
}

void um_vec2_chan_lifecycle_init(struct um_vec2_chan_lifecycle *life)
{
	life->state = UM_VEC2_CHAN_UNINIT;
}

const char *um_vec2_dev_state_name(enum um_vec2_dev_state state)
{
	switch (state) {
	case UM_VEC2_DEV_NEW:
		return "NEW";
	case UM_VEC2_DEV_CONFIGURED:
		return "CONFIGURED";
	case UM_VEC2_DEV_REGISTERED:
		return "REGISTERED";
	case UM_VEC2_DEV_OPENING:
		return "OPENING";
	case UM_VEC2_DEV_RUNNING:
		return "RUNNING";
	case UM_VEC2_DEV_QUIESCING:
		return "QUIESCING";
	case UM_VEC2_DEV_DEAD:
		return "DEAD";
	default:
		return "INVALID";
	}
}

const char *um_vec2_chan_state_name(enum um_vec2_chan_state state)
{
	switch (state) {
	case UM_VEC2_CHAN_UNINIT:
		return "UNINIT";
	case UM_VEC2_CHAN_ALLOCATED:
		return "ALLOCATED";
	case UM_VEC2_CHAN_FD_ATTACHED:
		return "FD_ATTACHED";
	case UM_VEC2_CHAN_IRQ_ATTACHED:
		return "IRQ_ATTACHED";
	case UM_VEC2_CHAN_NAPI_ENABLED:
		return "NAPI_ENABLED";
	case UM_VEC2_CHAN_ACTIVE:
		return "ACTIVE";
	case UM_VEC2_CHAN_QUIESCING:
		return "QUIESCING";
	case UM_VEC2_CHAN_CLOSED:
		return "CLOSED";
	default:
		return "INVALID";
	}
}

bool um_vec2_dev_can_transition(enum um_vec2_dev_state from,
				enum um_vec2_dev_state to)
{
	switch (from) {
	case UM_VEC2_DEV_NEW:
		return to == UM_VEC2_DEV_CONFIGURED ||
		       to == UM_VEC2_DEV_DEAD;
	case UM_VEC2_DEV_CONFIGURED:
		return to == UM_VEC2_DEV_REGISTERED ||
		       to == UM_VEC2_DEV_DEAD;
	case UM_VEC2_DEV_REGISTERED:
		return to == UM_VEC2_DEV_OPENING ||
		       to == UM_VEC2_DEV_DEAD;
	case UM_VEC2_DEV_OPENING:
		return to == UM_VEC2_DEV_RUNNING ||
		       to == UM_VEC2_DEV_QUIESCING;
	case UM_VEC2_DEV_RUNNING:
		return to == UM_VEC2_DEV_QUIESCING;
	case UM_VEC2_DEV_QUIESCING:
		return to == UM_VEC2_DEV_REGISTERED ||
		       to == UM_VEC2_DEV_DEAD;
	case UM_VEC2_DEV_DEAD:
	default:
		return false;
	}
}

bool um_vec2_chan_can_transition(enum um_vec2_chan_state from,
				 enum um_vec2_chan_state to)
{
	switch (from) {
	case UM_VEC2_CHAN_UNINIT:
		return to == UM_VEC2_CHAN_ALLOCATED ||
		       to == UM_VEC2_CHAN_CLOSED;
	case UM_VEC2_CHAN_ALLOCATED:
		return to == UM_VEC2_CHAN_FD_ATTACHED ||
		       to == UM_VEC2_CHAN_QUIESCING ||
		       to == UM_VEC2_CHAN_CLOSED;
	case UM_VEC2_CHAN_FD_ATTACHED:
		return to == UM_VEC2_CHAN_IRQ_ATTACHED ||
		       to == UM_VEC2_CHAN_QUIESCING;
	case UM_VEC2_CHAN_IRQ_ATTACHED:
		return to == UM_VEC2_CHAN_NAPI_ENABLED ||
		       to == UM_VEC2_CHAN_QUIESCING;
	case UM_VEC2_CHAN_NAPI_ENABLED:
		return to == UM_VEC2_CHAN_ACTIVE ||
		       to == UM_VEC2_CHAN_QUIESCING;
	case UM_VEC2_CHAN_ACTIVE:
		return to == UM_VEC2_CHAN_QUIESCING;
	case UM_VEC2_CHAN_QUIESCING:
		return to == UM_VEC2_CHAN_CLOSED;
	case UM_VEC2_CHAN_CLOSED:
	default:
		return false;
	}
}

int um_vec2_dev_transition(struct um_vec2_dev_lifecycle *life,
			   enum um_vec2_dev_state to)
{
	if (!um_vec2_dev_can_transition(life->state, to))
		return -EINVAL;
	life->state = to;
	return 0;
}

int um_vec2_chan_transition(struct um_vec2_chan_lifecycle *life,
			    enum um_vec2_chan_state to)
{
	if (!um_vec2_chan_can_transition(life->state, to))
		return -EINVAL;
	life->state = to;
	return 0;
}
