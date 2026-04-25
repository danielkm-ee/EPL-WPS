/* pwm_control.h
 *
 * PWM drivers for the output stage (four shared-slice channels) and the
 * EDM feedback signal to the motion controller.
 */

#ifndef EPL_WPS_PWM_CONTROL_H
#define EPL_WPS_PWM_CONTROL_H

#include <stdint.h>

// EDM_FEEDBACK PWM: configured once at startup, duty cycle updated at runtime.
void pwm_setup_feedback(void);
void pwm_set_feedback_duty(double duty);   // clamped to [0.0, 1.0]

// Output stage: full reconfigure. Sets up all four slices, loads levels,
// applies the high-voltage-phase offset to prevent shoot-through, then
// selectively enables slices per the flag arguments.
void pwm_setup_output_stage(double   machining_duty_cycle,
                            int      machining_frequency_hz,
                            int      output_current_threshold_a,
                            bool     enable_switch,
                            bool     high_current_phase,
                            bool     high_voltage_phase,
                            int      hv_pulse_on_time_us,
                            double   hv_pwm_offset);

// Drives all output switches to their safe-OFF state and disables PWM.
void pwm_disable_output_stage(void);

#endif // EPL_WPS_PWM_CONTROL_H
