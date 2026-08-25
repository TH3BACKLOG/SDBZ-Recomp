#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "ps2_log.h"
#include <atomic>
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    // Byte 0 is the SIO2 frame's validity byte: 0 = this frame carries data,
    // 0xFF = no pad answered. The game gates its whole button decode on it --
    // sub_109900 @ 0x109900 does `if ( scePadRead(...) && !buf[0] )` -- so a
    // non-zero byte 0 makes every read silently produce no buttons.
    data[0] = 0x00;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    else
    {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

// ---------------------------------------------------------------------------
// padman per-frame push emulation (Stage 5.12)
//
// SDBZ statically links its own libpad, so it never reaches the Kernel/Stubs/
// Pad.cpp path above via the SDK -- it goes guest libpad -> SIF RPC -> IOP
// padman. Our HLE in ps2_iop.cpp answers modversion/init/portopen, but real
// padman pushes the per-frame pad state IOP->EE *asynchronously* on vsync
// (SIF CMD 0x80000019), which we never did. The game's 256-byte pad buffer
// therefore kept its portopen-time init forever: state=5 (EXECCMD), reqState=2
// (BUSY), length=0, buttons=0xFF -- so scePadRead returned 0 and every button
// read as released. No input path to the guest existed at all.
//
// This writes that push directly into the guest's buffer. Every offset below
// was decoded from the guest's own libpad in ida_scripts/decompiles_SLUS_214_42.txt,
// not guessed:
//
//   0x568990 + port*112 + slot*28          = per-port/slot entry (scePadPortOpen
//                                            @ 0x188080 builds it)
//     +0   -> the game's 64B-aligned 256-byte pad data buffer
//     +16  -> open flag (== 1 once portopen succeeded)
//
//   The 256-byte buffer is two 128-byte halves. The internal reader
//   (0x188320) picks a half with `*(int*)(buf+88) < *(int*)(buf+216)`:
//   STRICTLY-less, so equal counters select half 0. We fill the stale half,
//   then bump its counter strictly past the other one so the flip is atomic
//   from the guest's point of view.
//
//   Within a 128-byte half:
//     +0..31 -> 32-byte pad status; exactly the format readState() emits
//       +0   -> frame validity: 0 = data present, 0xFF = no pad answered
//       +1   -> pad id byte: (id << 4) | halfwords. 0x73 = DualShock2 analog,
//               6 data bytes, no pressure. NOT 0x79 -- that id makes the game
//               overwrite its own "button held" flags with our zeroed pressures
//     +88    -> frame counter (signed int, selects the newer half)
//     +96    -> data length; also scePadRead's (0x1884D0) return value
//     +100   -> ex-mode info flag. scePadInfoMode (0x188970) returns 0 for
//               MODECUREXID/MODECUROFFS/MODETABLE when this equals +114, which
//               is the honest answer for an HLE pad with no mode table
//     +101   -> current pad id byte; scePadInfoMode(MODECURID) returns it >> 4.
//               Leaving it 0 strands the game's pad manager (sub_109900 @
//               0x109900) in state 0 forever, so scePadRead is never called
//     +112   -> state; 6 = PAD_STATE_STABLE
//     +113   -> reqState; 0 = COMPLETE, 2 = BUSY
//     +114   -> pad present / info valid (scePadSetActDirect @ 0x188B60 and
//               scePadInfoMode both require == 1)
//
//   Guest pad manager sub_109900 @ 0x109900, decoded:
//     scePadGetState -> 2/6 enters the state machine, 1/5 waits, else resets.
//     State 0 caches padId = scePadInfoMode(MODECURID); id 7 -> state 70,
//     id 4 -> state 40, anything else (including 0) -> state 99. States 70/71/72
//     negotiate DS2 mode and funnel to 99 on any failure. State 99 is the
//     steady read state; it requires cachedId == (readBuf[1] >> 4).
//
//   scePadGetState (0x188548) special-cases `state==6 && reqState==2` and
//   returns 5 (EXECCMD/busy) -- so writing +112=6 alone is NOT enough,
//   +113 must be cleared to 0 as well.
//
// registerFunction() is not usable here: the game reaches scePadRead by `jal`,
// which the recompiler emits as a direct C++ fn_ call that bypasses the
// dispatch registry.
// ---------------------------------------------------------------------------

namespace
{
    constexpr uint32_t kPadTableBase = 0x00568990u; // libpad per-port entry table
    constexpr uint32_t kPadPortStride = 112u;
    constexpr uint32_t kPadSlotStride = 28u;
    constexpr int kPadMaxPorts = 2;
    constexpr int kPadMaxSlots = 4;

    constexpr uint32_t kRdramMask = 0x1FFFFFFFu;
    constexpr uint32_t kRdramSize = 0x02000000u;

    // Counters are compared as signed int by the guest; wrap well short of
    // overflow. At 60 Hz this point is never reached in a real session, but a
    // wrapped pair must still satisfy "target is strictly greater".
    constexpr int32_t kPadCounterWrap = 0x40000000;

    PSPadBackend g_padPushBackend; // PSPadBackend is stateless / default-constructible

    inline uint32_t padRead32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (addr & kRdramMask), sizeof(v));
        return v;
    }

    inline void padWrite32(uint8_t *rdram, uint32_t addr, uint32_t v)
    {
        std::memcpy(rdram + (addr & kRdramMask), &v, sizeof(v));
    }

    // A guest pointer is only usable if the whole 256-byte buffer is inside
    // RDRAM and it carries libpad's documented 64-byte alignment.
    inline bool padBufferLooksValid(uint32_t buf)
    {
        const uint32_t phys = buf & kRdramMask;
        if (phys == 0 || (phys & 0x3Fu) != 0)
            return false;
        return phys + 256u <= kRdramSize;
    }
}

// Emulates one padman vsync push for every port/slot the guest has opened.
// Called once per presented host frame from PS2Runtime's present loop -- the
// same thread raylib polls input on, so IsKeyDown()/IsGamepadButtonDown() are
// read on the thread that owns them.
extern "C" void ps2x_pad_push_frame(uint8_t *rdram)
{
    if (!rdram)
        return;

    static int s_pushLogged = 0;
    static int s_changeLogged = 0;
    static uint16_t s_lastButtons[kPadMaxPorts][kPadMaxSlots] = {};
    static bool s_seenPort[kPadMaxPorts][kPadMaxSlots] = {};
    constexpr int kPushLogCap = 8;
    constexpr int kChangeLogCap = 64;

    for (int port = 0; port < kPadMaxPorts; ++port)
    {
        for (int slot = 0; slot < kPadMaxSlots; ++slot)
        {
            const uint32_t entry =
                kPadTableBase + static_cast<uint32_t>(port) * kPadPortStride + static_cast<uint32_t>(slot) * kPadSlotStride;

            if (padRead32(rdram, entry + 16) != 1u) // open flag
                continue;

            const uint32_t buf = padRead32(rdram, entry + 0);
            if (!padBufferLooksValid(buf))
            {
                if (s_pushLogged < kPushLogCap)
                {
                    ++s_pushLogged;
                    RUNTIME_LOG("[pad] port=" << port << " slot=" << slot
                                              << " REJECTED buffer=0x" << std::hex << buf << std::dec << "\n");
                }
                continue;
            }

            const int32_t c0 = static_cast<int32_t>(padRead32(rdram, buf + 88));
            const int32_t c1 = static_cast<int32_t>(padRead32(rdram, buf + 216));

            // Guest picks half1 only when c0 < c1 (ties -> half0). Fill the
            // half it is NOT currently reading.
            const uint32_t liveHalf = (c0 < c1) ? 1u : 0u;
            const uint32_t targetHalf = 1u - liveHalf;
            const uint32_t block = buf + targetHalf * 128u;

            uint8_t status[32];
            // Port 1+ gets a valid but idle pad: leaving an opened port in its
            // portopen BUSY state would strand any guest loop that waits for
            // every opened pad to reach PAD_STATE_STABLE.
            const bool isHostPad = (port == 0 && slot == 0);
            if (isHostPad)
            {
                if (!g_padPushBackend.readState(port, slot, status, sizeof(status)))
                    continue;
            }
            else
            {
                std::memset(status, 0, sizeof(status));
                status[0] = 0x00; // valid frame, nothing pressed
                status[1] = kPadAnalogMarker;
                status[2] = 0xFF;
                status[3] = 0xFF;
                status[4] = status[5] = status[6] = status[7] = kPadStickCenter;
            }

            std::memcpy(rdram + ((block + 0) & kRdramMask), status, sizeof(status));
            padWrite32(rdram, block + 96, 32u);        // length / scePadRead return
            rdram[(block + 100) & kRdramMask] = 1;     // ex-mode info unavailable
            rdram[(block + 101) & kRdramMask] = kPadAnalogMarker; // current pad id byte
            rdram[(block + 112) & kRdramMask] = 6;     // state    = PAD_STATE_STABLE
            rdram[(block + 113) & kRdramMask] = 0;     // reqState = COMPLETE
            rdram[(block + 114) & kRdramMask] = 1;     // pad present / info valid

            int32_t next = ((c0 > c1) ? c0 : c1) + 1;
            if (next >= kPadCounterWrap)
            {
                // Restart the pair rather than let the signed compare wrap.
                padWrite32(rdram, buf + (liveHalf * 128u) + 88, 0u);
                next = 1;
            }

            // Publish the filled half last: the guest EE thread runs on a
            // different host thread, so the data stores must land first.
            std::atomic_thread_fence(std::memory_order_release);
            padWrite32(rdram, block + 88, static_cast<uint32_t>(next));

            const uint16_t buttons = static_cast<uint16_t>(status[2] | (status[3] << 8));

            if (!s_seenPort[port][slot])
            {
                s_seenPort[port][slot] = true;
                s_lastButtons[port][slot] = buttons;
                if (s_pushLogged < kPushLogCap)
                {
                    ++s_pushLogged;
                    RUNTIME_LOG("[pad] first push port=" << port << " slot=" << slot
                                                         << " buf=0x" << std::hex << buf
                                                         << " half=" << std::dec << targetHalf
                                                         << " ctr=" << next
                                                         << " btns=0x" << std::hex << buttons << std::dec << "\n");
                    if (s_pushLogged == kPushLogCap)
                        RUNTIME_LOG("[pad] cap: first-push log capped at " << kPushLogCap << "\n");
                }
            }
            else if (buttons != s_lastButtons[port][slot])
            {
                s_lastButtons[port][slot] = buttons;
                if (s_changeLogged < kChangeLogCap)
                {
                    ++s_changeLogged;
                    RUNTIME_LOG("[pad] change port=" << port << " slot=" << slot
                                                     << " btns=0x" << std::hex << buttons << std::dec
                                                     << " half=" << targetHalf << " ctr=" << next << "\n");
                    if (s_changeLogged == kChangeLogCap)
                        RUNTIME_LOG("[pad] cap: change log capped at " << kChangeLogCap << "\n");
                }
            }
        }
    }
}
