/* ctx.c */

#include <string.h>
#include "ctx.h"
#include "../config.h"

void ctx_init(main_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->state         = STARTUP;
    ctx->mode          = EDM_ISOFREQUENCY_MODE;
    ctx->mode_prep_done = false;

    ctx->params.discharge_count_target = 0;
    ctx->params.duty_cycle             = 0.10f;
    ctx->params.frequency_hz           = 10000.0f;
    ctx->params.init_voltage           = 80.0f;

    ctx->allow_success_rate_pulse_skip  = ALLOW_DISCHARGE_SUCCESS_RATE_PULSE_SKIP;
    ctx->allow_power_setpoint_pulse_skip = ALLOW_POWER_SETPOINT_PULSE_SKIP;
    ctx->periodic_telemetry_enabled     = PERIODIC_TELEMETRY_ENABLED;
}

void ctx_set_state(main_ctx_t *ctx, device_state_t new_state)
{
    ctx->state = new_state;
}

void ctx_set_mode(main_ctx_t *ctx, mode_of_operation_t new_mode)
{
    ctx->mode           = new_mode;
    ctx->mode_prep_done = false;
}

void ctx_reset_discharge_stats(main_ctx_t *ctx)
{
    ctx->new_discharge_detected       = false;
    ctx->new_discharge_current_adc    = 0;
    ctx->new_discharge_voltage_adc    = 0;
    ctx->avg_discharge_current        = 0.0;
    ctx->avg_discharge_voltage        = 0.0;
    ctx->avg_discharge_success_rate   = 0.0;
    ctx->discharge_success_rate       = 0.0;
    ctx->current_discharge_count      = 0;
    ctx->discharges_since_op_start    = 0;
    ctx->requested_discharges_reached = false;
    ctx->last_success_rate_calc_us    = 0;
    ctx->mode_prep_done               = false;
}
