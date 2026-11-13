/* Runs the hand-written corner cases in edge.S; prints the failure mask. */
#include "test.h"
#ifdef __arm__
uint32_t edge_cases(void);
#else
static uint32_t edge_cases(void) { return 0; }
#endif
int main(void) {
    put_hex("edge.failures", edge_cases());
    return 0;
}
