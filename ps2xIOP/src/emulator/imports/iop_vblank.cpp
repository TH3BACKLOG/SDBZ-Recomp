#include "iop_vblank.h"

#include "../core/iop_cpu.h"
#include "../iop_emulator_const.h"
#include "../core/iop_kernel.h"
#include "../services/iop_rpc.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    IopVblank::IopVblank(IopKernel &kernel) noexcept
        : m_kernel(kernel)
    {
        reset();
    }

    void IopVblank::reset() noexcept
    {
        m_handlers.clear();
        m_nextStartCycle = kVblankPeriodCycles;
        m_nextEndCycle = kVblankPeriodCycles + kVblankEndPhaseCycles;
        m_servicing = false;
    }

    bool IopVblank::dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        switch (ordinal)
        {
        case 4: // WaitVblankStart
        case 5: // WaitVblankEnd
        case 6: // WaitVblank
        case 7: // WaitNonVblank
        {
            const bool waitForEnd = ordinal == 5u || ordinal == 7u;
            const uint64_t phase = waitForEnd ? kVblankEndPhaseCycles : 0u;
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            uint64_t wakeCycle = fieldStart + phase;
            if (wakeCycle <= currentCycle)
                wakeCycle += kVblankPeriodCycles;
            m_kernel.delayCurrentUntil(wakeCycle, cpu);
            cpu.gpr[2] = 0u;
            return true;
        }
        case 8: // RegisterVblankHandler(edge, priority, handler, arg)
        {
            Handler handler;
            handler.edge = cpu.gpr[4] != 0u ? 1u : 0u;
            handler.priority = cpu.gpr[5];
            handler.function = cpu.gpr[6];
            handler.argument = cpu.gpr[7];
            handler.gp = cpu.gpr[28];
            if (handler.function != 0u)
            {
                // Lower priority value runs first; equal priorities keep registration order.
                const auto pos = std::upper_bound(m_handlers.begin(), m_handlers.end(), handler,
                                                  [](const Handler &a, const Handler &b)
                                                  { return a.priority < b.priority; });
                m_handlers.insert(pos, handler);
            }
            cpu.gpr[2] = 0u;
            return true;
        }
        case 9: // ReleaseVblankHandler(edge, handler)
        {
            const uint32_t edge = cpu.gpr[4] != 0u ? 1u : 0u;
            const uint32_t function = cpu.gpr[5];
            std::erase_if(m_handlers, [&](const Handler &h)
                          { return h.edge == edge && h.function == function; });
            cpu.gpr[2] = 0u;
            return true;
        }
        default:
            return false;
        }
    }

    void IopVblank::fire(uint32_t edge, IopGuestExecutor &executor)
    {
        // Copy: a handler may register/release handlers while we iterate.
        const std::vector<Handler> handlers = m_handlers;
        for (const Handler &handler : handlers)
        {
            if (handler.edge != edge)
                continue;
            // Return value is ignored: handlers stay registered until released.
            (void)executor.executeGuestFunctionWithBudget(handler.function, handler.argument, 0u, 0u, 0u,
                                                          handler.gp, 100000u);
        }
    }

    void IopVblank::serviceDue(uint64_t currentCycle, IopGuestExecutor &executor)
    {
        if (m_servicing)
            return;
        m_servicing = true;
        struct ServiceGuard
        {
            bool &flag;
            ~ServiceGuard() { flag = false; }
        } guard{m_servicing};

        // Deliver at most one edge of each kind per call and resynchronise, so a
        // long idle skip does not replay a burst of stale vblanks.
        if (currentCycle >= m_nextStartCycle)
        {
            fire(0u, executor);
            m_nextStartCycle = currentCycle - (currentCycle % kVblankPeriodCycles) + kVblankPeriodCycles;
        }
        if (currentCycle >= m_nextEndCycle)
        {
            fire(1u, executor);
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            m_nextEndCycle = fieldStart + kVblankEndPhaseCycles;
            if (m_nextEndCycle <= currentCycle)
                m_nextEndCycle += kVblankPeriodCycles;
        }
    }

    uint64_t IopVblank::nextEventCycle(uint64_t fallback) const noexcept
    {
        if (m_handlers.empty())
            return fallback;
        return std::min({fallback, m_nextStartCycle, m_nextEndCycle});
    }
}
