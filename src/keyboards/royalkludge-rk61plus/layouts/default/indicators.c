// The RGB matrix is a later phase. Until then the indicators hooks are no-ops,
// so the tick/matrix/sleep paths link and run with no LED hardware driven.
#include "indicators.h"

void indicators_init() {}
void indicators_start() {}
void indicators_render() {}
void indicators_pre_update() {}

bool indicators_update_step(keyboard_state_t *keyboard, uint8_t current_step)
{
    keyboard;
    current_step;
    return true;
}

void indicators_post_update() {}
void indicators_pwm_enable() {}
void indicators_pwm_disable() {}
void indicators_apply_defaults() {}
void indicators_validate_settings() {}
