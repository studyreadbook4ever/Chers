#ifndef CHERS_CLIENT_H
#define CHERS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI v1: the only observation is the complete rendered RGBA image.
 * No chart, game clock, stage, score, or note metadata crosses this API.
 * A client owns a connection; only one client may connect to a benchmark.
 * Calls may be made concurrently, except cl_close needs exclusive ownership.
 */
#define CL_ABI_VERSION 1u
#define CL_WIDTH 640u
#define CL_HEIGHT 360u
#define CL_STRIDE (CL_WIDTH * 4u)
#define CL_FRAME_BYTES (CL_STRIDE * CL_HEIGHT)

typedef struct cl_client cl_client;
typedef struct cl_frame_info {
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t sequence;
} cl_frame_info;

enum cl_result {
    CL_OK = 0,
    CL_TIMEOUT = 1,
    CL_WOULD_BLOCK = 2,
    CL_ERROR_ARGUMENT = -1,
    CL_ERROR_SYSTEM = -2,
    CL_ERROR_PROTOCOL = -3,
    CL_ERROR_DISCONNECTED = -4
};

/* NULL endpoint uses $XDG_RUNTIME_DIR/chers-UID.sock, or
 * /tmp/chers-UID/bench.sock. The server must be polling while connecting.
 * timeout_ms >= 0; successful connect creates *out. */
int cl_connect(const char *endpoint, int timeout_ms, cl_client **out);
/* Copies the next complete frame newer than the last returned frame into
 * caller memory. Intermediate frames may be skipped. Pixels stay valid after
 * return. On timeout the same outstanding request resumes on the next call.
 * timeout_ms >= 0; capacity must be at least CL_FRAME_BYTES. */
int cl_read_frame(cl_client *client, void *rgba, size_t capacity,
                  cl_frame_info *info, int timeout_ms);
/* One call is one tap, never a key hold/repeat state. lane=0 is A, 15 is P.
 * CL_WOULD_BLOCK means NOT sent; callers must explicitly retry/report failure.
 * No timestamps or future scheduling commands are accepted from the client. */
int cl_tap(cl_client *client, uint32_t lane);
int cl_start(cl_client *client);
/* Stops/invalidates an unfinished run; this is never a successful completion. */
int cl_stop(cl_client *client);
void cl_close(cl_client *client);
const char *cl_result_string(int result);

#ifdef __cplusplus
}
#endif
#endif
