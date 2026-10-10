#pragma once

// VU1 capture recorder: the runtime side of build_scripts/vucap.py.
// See VuCapRecorder.cpp for the env vars and how the file differs from PCSX2's.

#include <atomic>
#include <cstdint>

struct VU1State;
struct VIFRegisters;

namespace vucap
{
    // True while a capture is armed, running or waiting to finish. Every hook
    // below checks it first, so an unset PS2X_VUCAP costs one relaxed load.
    extern std::atomic<bool> g_hot;

    inline bool hot()
    {
        return g_hot.load(std::memory_order_relaxed);
    }

    // The VU1 whose registers go into SNAP. Set once at runtime init.
    void setStateSource(const VU1State *state);

    // Vsync worker thread, every vblank tick.
    void onVSync(uint64_t tick);

    // EE executor thread.
    void vifCallBegin(const uint8_t *data, uint32_t sizeBytes, const uint8_t *micro, const uint8_t *mem,
                      const VIFRegisters &regs);
    void vifCallEnd(const uint8_t *micro, const uint8_t *mem);
    void runStart(uint32_t tpc, uint32_t top, uint32_t itop, const uint8_t *micro, const uint8_t *mem,
                  const VU1State &state, uint32_t vpuStat, uint32_t fbrst);
    void runEnd(const VU1State &state, bool forced, uint32_t vpuStat, uint32_t fbrst);
    void kick(uint32_t sourceAddress, const uint8_t *bytes, uint32_t sizeBytes);

    // One processVIF1Data call. Must be a NAMED local so the destructor runs at
    // the end of the call, not at the end of the statement.
    class VifCallScope
    {
    public:
        VifCallScope(const uint8_t *data, uint32_t sizeBytes, const uint8_t *micro, const uint8_t *mem,
                     const VIFRegisters &regs)
            : m_micro(micro), m_mem(mem), m_on(hot())
        {
            if (m_on)
                vifCallBegin(data, sizeBytes, micro, mem, regs);
        }

        ~VifCallScope()
        {
            if (m_on)
                vifCallEnd(m_micro, m_mem);
        }

        VifCallScope(const VifCallScope &) = delete;
        VifCallScope &operator=(const VifCallScope &) = delete;

    private:
        const uint8_t *m_micro;
        const uint8_t *m_mem;
        bool m_on;
    };
}
