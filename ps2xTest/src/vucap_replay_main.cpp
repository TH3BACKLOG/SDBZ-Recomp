// Standalone runner for the VU1 capture replay (ps2_vu1_capture_replay_tests.cpp).
//
// Kept separate from ps2x_tests so the replay builds and runs even while other
// test files lag behind runtime API changes. Usage: set PS2X_VUCAP, then
//   ps2x_vucap_replay.exe
// Profiling: PS2X_PROFILE=1 (+ PS2X_PROFILE_MS=1) samples with the host sampler;
// PS2X_VUCAP_LOOPS=<n> replays the capture n times for more samples.
#include "MiniTest.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

void register_ps2_vu1_capture_replay_tests();
void reset_ps2_test_function_table();
extern "C" void ps2x_host_sampler_start(void);
extern "C" void ps2x_host_sampler_stop(void);

int main(int argc, char **argv)
{
    const std::string suiteFilter = (argc > 1) ? argv[1] : std::string();
    const std::string testFilter = (argc > 2) ? argv[2] : std::string();

    int loops = 1;
    if (const char *l = std::getenv("PS2X_VUCAP_LOOPS"))
        loops = (std::max)(1, std::atoi(l));

    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_ps2_vu1_capture_replay_tests();
    ps2x_host_sampler_start();
    int res = 0;
    for (int i = 0; i < loops; ++i)
        res |= MiniTest::Run(suiteFilter, testFilter);
    ps2x_host_sampler_stop();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
