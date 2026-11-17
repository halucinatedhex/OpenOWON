/* The xdm1241-ui build runs lib/sim's HY3131 model on the chip, in place of
 * the real front end. lib/sim is host-only as a library (it also holds file
 * output for the PC simulator), so only the model is compiled in, here. */
#if defined(HAL_SIM_AFE)
#include "../lib/sim/sim_hy3131.c"
#else
typedef int sim_afe_not_in_this_build;
#endif
