#ifndef API_H
#define API_H

/*
 * The one command interface. The web page (HTTP and WebSocket), the gamepad,
 * voice and the LLM's MCP tools all call api_call; nothing else reaches the core.
 * Errors are codes such as "web_only", which the web page translates.
 */
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "feature.h"

#define DOITDUCK_VERSION "0.2.5"

/*
 * Runs method with params (may be NULL) for source src (SRC_*). Returns the
 * result object, which the caller deletes, or NULL with *err the error code.
 */
cJSON *api_call(int src, const char *method, const cJSON *params, const char **err);
/* Adds a feature's methods; before the features start. */
void api_register(const struct api_method *methods, int n);
/* Letters, digits, '_' and '-', shorter than size: a name that can name a file. */
int api_good_name(const char *s, size_t size);
/* Decodes base64 into out; the length, or -1 when it is not base64 or longer than size. */
int api_base64_decode(const char *in, uint8_t *out, size_t size);
/* An error code in English words, for scripts, logs and the LLM; an unknown code as it is. */
const char *api_message(const char *code);
/* The web page went away: release control it held. */
void api_web_closed(void);
/* A timed move or a skill is running. */
int api_moving(void);

#endif
