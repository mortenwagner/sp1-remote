/* Line-based JSON console for editing the control surface over CDC.
 * See config_console.c for the protocol. */
#ifndef SP1_CONFIG_CONSOLE_H
#define SP1_CONFIG_CONSOLE_H

#include <stdbool.h>

/* Drain any received bytes and dispatch complete lines. Non-blocking; call
 * once per control pass. */
void config_console_poll(void);

/* Set by the {"t":"quiet"} command. The diagnostic stream is noise to a
 * browser trying to read JSON replies, so the editor turns it off on
 * connect and back on when it disconnects. */
extern bool g_diag_quiet;

#endif /* SP1_CONFIG_CONSOLE_H */
