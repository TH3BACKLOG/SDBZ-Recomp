// Standalone runner for the VU1 capture replay (ps2_vu1_capture_replay_tests.cpp).
//
// Kept separate from ps2x_tests so the replay builds and runs even while other
// test files lag behind runtime API changes. Usage: set PS2X_VUCAP, then
//   ps2x_vucap_replay.exe
#include "MiniTest.h"
#include <cstdlib>
#include <iostream>

void register_ps2_vu1_capture_replay_tests();
void reset_ps2_test_function_table();

int main(int argc, char **argv)
{
    const std::string suiteFilter = (argc > 1) ? argv[1] : std::string();
    const std::string testFilter = (argc > 2) ? argv[2] : std::string();

    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_ps2_vu1_capture_replay_tests();
    int res = MiniTest::Run(suiteFilter, testFilter);
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
