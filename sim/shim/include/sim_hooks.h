/* sim-only control surface: how the SDL side injects "speech" into the
 * simulated volc_asr session. Typing = speaking; a >1s pause (or Enter) reads
 * as VAD silence, which triggers pi_screen's key-mode auto-send unchanged. */
#ifndef SIM_HOOKS_H
#define SIM_HOOKS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sim_asr_session_active(void);
void sim_asr_type(const char* utf8);  /* append text, fires on_delta with the full line */
void sim_asr_backspace(void);         /* drop the last UTF-8 codepoint, fires on_delta */
void sim_asr_end_of_speech(void);     /* force VAD silence now (Enter key) */
bool sim_asr_voice_detected(void);    /* true while typing (last input < 1s ago) */

#ifdef __cplusplus
}
#endif
#endif /* SIM_HOOKS_H */
