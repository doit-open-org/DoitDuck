#ifndef MCP_H
#define MCP_H

/*
 * The duck as an MCP server for the xiaozhi backend (JSON-RPC 2.0 inside the
 * "mcp" message): initialize, tools/list, tools/call.
 */
#include "cJSON.h"

/* Handles one request payload; returns the response payload, or NULL for a notification. */
cJSON *mcp_handle(const cJSON *req);

#endif
