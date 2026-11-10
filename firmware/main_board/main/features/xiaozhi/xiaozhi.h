#ifndef XIAOZHI_H
#define XIAOZHI_H

/* For mcp.c: the xiaozhi side of a photo question. */
#include <stddef.h>
#include <stdint.h>

/* Asks the server's vision model (url and token from MCP initialize) about a JPEG; the HTTP status, or -1. */
int xiaozhi_explain(const char *url, const char *token, const char *question, const uint8_t *jpeg, size_t len,
                    char *resp, size_t size);

#endif
