/* Minimal C ABI consumer: observes complete frames, accepts terminal taps.
 * Run the benchmark first, then this example. Space starts, A-P taps, Q stops.
 * This deliberately contains no chart access or built-in player. */
#include "chers/client.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    cl_client *client = NULL;
    int result = cl_connect(argc > 1 ? argv[1] : NULL, 2000, &client);
    if (result != CL_OK) {
        fprintf(stderr, "connect: %s\n", cl_result_string(result));
        return 1;
    }
    unsigned char *pixels = malloc(CL_FRAME_BYTES);
    if (!pixels) { cl_close(client); return 1; }
    puts("Space + Enter: start; A-P + Enter: tap; Q + Enter: stop and exit.");
    unsigned frames = 0;
    for (;;) {
        cl_frame_info frame;
        result = cl_read_frame(client, pixels, CL_FRAME_BYTES, &frame, 100);
        if (result == CL_OK && ++frames % 80 == 0)
            printf("frame %llu, %u x %u RGBA\n", (unsigned long long)frame.sequence, frame.width, frame.height);
        else if (result < 0) break;
        struct pollfd input = {0, POLLIN, 0};
        if (poll(&input, 1, 0) > 0 && (input.revents & POLLIN)) {
            int key = getchar();
            if (key == EOF) break;
            if (key == 'q' || key == 'Q') { result = cl_stop(client); break; }
            if (key == ' ') result = cl_start(client);
            else if (key >= 'a' && key <= 'p') result = cl_tap(client, (unsigned)(key - 'a'));
            else if (key >= 'A' && key <= 'P') result = cl_tap(client, (unsigned)(key - 'A'));
            else continue;
            if (result != CL_OK) fprintf(stderr, "input: %s\n", cl_result_string(result));
        }
    }
    free(pixels);
    cl_close(client);
    return result < 0 ? 1 : 0;
}
