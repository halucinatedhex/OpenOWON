/* Temperature: PT100, type-K thermocouple, and the cold junction.
 *
 * Stock converts through two 10-degree tables in flash (type K 0x0801DE58,
 * PT100 0x0801E0EC) with linear interpolation. Both tables contain errors
 * (PT100 290 C: 204.48 for 208.48 ohm, ~10 C; type K 850 C: 35.324 for
 * ~35.313 mV, ~0.3 C; the K table also drifts up to 13 uV low above 900 C),
 * so this module computes from the defining equations instead:
 *   PT100   IEC 60751 Callendar-Van Dusen, R0 = 100 ohm
 *   type K  NIST ITS-90 reference polynomials
 * Ranges match stock: K -270..1370 C, PT100 -50..850 C.
 */
#ifndef MEAS_TEMP_H
#define MEAS_TEMP_H

#include <stdbool.h>
#include <stdint.h>

#define MEAS_TC_K_MIN    -270.0
#define MEAS_TC_K_MAX    1370.0
#define MEAS_PT100_MIN    -50.0
#define MEAS_PT100_MAX    850.0

typedef enum { MEAS_DEG_C = 0, MEAS_DEG_F = 1, MEAS_KELVIN = 2 } meas_tunit_t;

double meas_pt100_ohm(double t_c);
double meas_tc_k_mv(double t_c);

/* Inverses. *overrange (optional) is set outside the stock range; the result
 * is still computed (clamped to the polynomial's domain for type K). */
double meas_pt100_temp(double r_ohm, bool *overrange);
double meas_tc_k_temp(double emf_mv, bool *overrange);

/* Thermocouple with cold-junction compensation: measured EMF in mV plus the
 * EMF of the junction temperature, inverted (0x0800A1C8). */
double meas_tc_k_compensated(double emf_mv, double t_cj_c, bool *overrange);

/* Cold junction, 0x08008DD4: two DCV 500 mV readings (mV) under the two bias
 * settings. g_raw is factory record +0x56C (G x 1e4); outside 0.10-0.18 stock
 * falls back to G = 0.169465. */
double meas_cj_gain(int32_t g_raw);
double meas_cold_junction(double v1_mv, double v2_mv, int32_t g_raw);

/* Display unit, prefs +0x99. */
double meas_temp_unit(double t_c, meas_tunit_t unit);

#endif
