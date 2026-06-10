/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Explicit state model for UML vector networking v2.
 */
#ifndef __UM_VECTOR2_MODEL_H
#define __UM_VECTOR2_MODEL_H

#include <linux/types.h>

enum um_vec2_dev_state {
	UM_VEC2_DEV_NEW,
	UM_VEC2_DEV_CONFIGURED,
	UM_VEC2_DEV_REGISTERED,
	UM_VEC2_DEV_OPENING,
	UM_VEC2_DEV_RUNNING,
	UM_VEC2_DEV_QUIESCING,
	UM_VEC2_DEV_DEAD,
};

enum um_vec2_chan_state {
	UM_VEC2_CHAN_UNINIT,
	UM_VEC2_CHAN_ALLOCATED,
	UM_VEC2_CHAN_FD_ATTACHED,
	UM_VEC2_CHAN_IRQ_ATTACHED,
	UM_VEC2_CHAN_NAPI_ENABLED,
	UM_VEC2_CHAN_ACTIVE,
	UM_VEC2_CHAN_QUIESCING,
	UM_VEC2_CHAN_CLOSED,
};

struct um_vec2_dev_lifecycle {
	enum um_vec2_dev_state state;
};

struct um_vec2_chan_lifecycle {
	enum um_vec2_chan_state state;
};

void um_vec2_dev_lifecycle_init(struct um_vec2_dev_lifecycle *life);
void um_vec2_chan_lifecycle_init(struct um_vec2_chan_lifecycle *life);

const char *um_vec2_dev_state_name(enum um_vec2_dev_state state);
const char *um_vec2_chan_state_name(enum um_vec2_chan_state state);

bool um_vec2_dev_can_transition(enum um_vec2_dev_state from,
				enum um_vec2_dev_state to);
bool um_vec2_chan_can_transition(enum um_vec2_chan_state from,
				 enum um_vec2_chan_state to);

int um_vec2_dev_transition(struct um_vec2_dev_lifecycle *life,
			   enum um_vec2_dev_state to);
int um_vec2_chan_transition(struct um_vec2_chan_lifecycle *life,
			    enum um_vec2_chan_state to);

static inline bool um_vec2_dev_can_open(const struct um_vec2_dev_lifecycle *life)
{
	return life->state == UM_VEC2_DEV_REGISTERED;
}

static inline bool um_vec2_dev_can_xmit(const struct um_vec2_dev_lifecycle *life)
{
	return life->state == UM_VEC2_DEV_RUNNING;
}

static inline bool
um_vec2_chan_is_active(const struct um_vec2_chan_lifecycle *life)
{
	return life->state == UM_VEC2_CHAN_ACTIVE;
}

#endif /* __UM_VECTOR2_MODEL_H */
