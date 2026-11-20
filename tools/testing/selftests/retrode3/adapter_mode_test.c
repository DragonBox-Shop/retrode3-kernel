// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include "../../../../drivers/misc/retrode3/retrode3_adapter_mode.h"

int main(void)
{
    assert(retrode3_adapter_mode_is_read(20));
    assert(retrode3_adapter_mode_is_read(21));
    assert(!retrode3_adapter_mode_is_read(0));
    assert(!retrode3_adapter_mode_is_read(10));
    assert(!retrode3_adapter_mode_is_read(22));
    assert(retrode3_adapter_lane_allowed(20, 16, 1, 0));
    assert(retrode3_adapter_lane_allowed(21, 16, 1, 0));
    assert(retrode3_adapter_lane_allowed(20, 8, 0, 1));
    assert(!retrode3_adapter_lane_allowed(21, 8, 0, 1));
    assert(!retrode3_adapter_lane_allowed(20, 8, 0, 0));
    assert(!retrode3_adapter_lane_allowed(21, 8, 0, 0));
    assert(!retrode3_adapter_lane_allowed(20, 16, 0, 0));
    assert(!retrode3_adapter_lane_allowed(21, 16, 0, 0));
    assert(!retrode3_adapter_lane_allowed(20, 32, 1, 0));
    assert(!retrode3_adapter_lane_allowed(21, 8, 1, 0));
    assert(!retrode3_adapter_lane_allowed(22, 16, 1, 0));
    return 0;
}
