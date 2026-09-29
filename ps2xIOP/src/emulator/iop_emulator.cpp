#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

// Host pad state for the SIO2 pad HLE below (Part 165). Written by the runtime's
// present loop (ps2_pad.cpp, the thread that owns raylib input), read on the IOP
// thread. Defined here so IOP-only binaries still link.
//   buttons: libpad wire order, active-low -- low byte = first digital byte
//            (Select..Left), high byte = second (L2..Square).
//   analog : rx | ry << 8 | lx << 16 | ly << 24.
//   served : count of port-0 pad polls answered; the runtime stops its own
//            direct buffer push once this is non-zero (PADMAN owns the buffer).
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad_buttons{0xFFFFu};
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad_analog{0x7F7F7F7Fu};
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad_served{0u};

namespace ps2x::iop::detail
{
    namespace
    {
        // DualShock2 on SIO2 port 0, ported from PCSX2 pcsx2/SIO/Pad/PadDualshock2.cpp
        // (SendCommandByte and the per-command handlers). commandBytesReceived
        // starts at 1 after the SIO2 mode byte (PadBase::SoftReset), so the reply
        // to a PAD packet is 0xFF, mode, 0x5A, data...
        struct Ds2Pad
        {
            uint8_t mode = 0x41; // DIGITAL until the game negotiates
            bool inConfig = false;
            bool analogLight = false;
            bool analogLocked = false;
            bool commandStage = false;
            uint32_t responseBytes = 0u;
            uint8_t smallMotorCfg = 0xFFu;
            uint8_t largeMotorCfg = 0xFFu;
            uint8_t command = 0u;
            uint32_t n = 1u;

            void softReset() { n = 1u; }

            // PCSX2 bit k of its 16-bit button word (first wire byte = bits 8..15)
            // -> our libpad-order word (first wire byte = bits 0..7). Active-low.
            static bool pressed(uint16_t buttons, int k)
            {
                const int ours = k >= 8 ? k - 8 : k + 8;
                return ((buttons >> ours) & 1u) == 0u;
            }

            uint8_t send(uint8_t b, uint16_t buttons, uint32_t analog)
            {
                uint8_t ret = 0u;
                if (n == 1u)
                {
                    command = b;
                    ret = inConfig ? 0xF3u : mode;
                }
                else if (n == 2u)
                {
                    ret = 0x5Au;
                }
                else
                {
                    ret = handle(b, buttons, analog);
                }
                ++n;
                return ret;
            }

            uint8_t handle(uint8_t b, uint16_t buttons, uint32_t analog)
            {
                switch (command)
                {
                case 0x40: // Mystery
                    return n == 5u ? 0x02u : n == 8u ? 0x5Au : 0x00u;
                case 0x41: // ButtonQuery
                    if (mode == 0x73u || mode == 0x79u)
                        return (n == 3u || n == 4u) ? 0xFFu : n == 5u ? 0x03u : n == 8u ? 0x5Au : 0x00u;
                    return 0x00u;
                case 0x42: // Poll
                {
                    static constexpr int kPressureBits[12] = {13, 15, 12, 14, 4, 5, 6, 7, 2, 3, 0, 1};
                    if (n == 3u)
                        return static_cast<uint8_t>(buttons & 0xFFu);
                    if (n == 4u)
                        return static_cast<uint8_t>(buttons >> 8);
                    if (n >= 5u && n <= 8u)
                        return static_cast<uint8_t>(analog >> ((n - 5u) * 8u));
                    if (n >= 9u && n <= 20u)
                        return pressed(buttons, kPressureBits[n - 9u]) ? 0xFFu : 0x00u;
                    return 0x00u;
                }
                case 0x43: // Config
                    if (n == 3u)
                        inConfig = b != 0u;
                    return 0x00u;
                case 0x44: // ModeSwitch
                    if (n == 3u)
                    {
                        analogLight = b != 0u;
                        mode = analogLight ? 0x73u : 0x41u;
                    }
                    else if (n == 4u)
                    {
                        analogLocked = b == 0x03u;
                    }
                    return 0x00u;
                case 0x45: // StatusInfo
                    switch (n)
                    {
                    case 3u: return 0x03u; // PhysicalType::STANDARD
                    case 4u: return 0x02u;
                    case 5u: return analogLight ? 0x01u : 0x00u;
                    case 6u: return 0x02u;
                    case 7u: return 0x01u;
                    default: return 0x00u;
                    }
                case 0x46: // Constant1
                    switch (n)
                    {
                    case 3u: commandStage = b != 0u; return 0x00u;
                    case 5u: return 0x01u;
                    case 6u: return commandStage ? 0x01u : 0x02u;
                    case 7u: return commandStage ? 0x01u : 0x00u;
                    case 8u: return commandStage ? 0x14u : 0x0Au;
                    default: return 0x00u;
                    }
                case 0x47: // Constant2
                    return n == 5u ? 0x02u : n == 7u ? 0x01u : 0x00u;
                case 0x4C: // Constant3
                    if (n == 3u)
                        commandStage = b != 0u;
                    return n == 6u ? (commandStage ? 0x07u : 0x04u) : 0x00u;
                case 0x4D: // VibrationMap
                {
                    uint8_t prev = 0xFFu;
                    if (n == 3u)
                    {
                        prev = smallMotorCfg;
                        smallMotorCfg = b;
                    }
                    else if (n == 4u)
                    {
                        prev = largeMotorCfg;
                        largeMotorCfg = b;
                    }
                    return prev;
                }
                case 0x4F: // ResponseBytes
                    if (n == 3u)
                        responseBytes = b;
                    else if (n == 4u)
                        responseBytes |= static_cast<uint32_t>(b) << 8;
                    else if (n == 5u)
                    {
                        responseBytes |= static_cast<uint32_t>(b) << 16;
                        if (responseBytes == 0x3Fu)
                        {
                            analogLight = true;
                            mode = 0x73u;
                        }
                        else if (responseBytes == 0x3FFFFu)
                        {
                            analogLight = true;
                            mode = 0x79u;
                        }
                        else
                        {
                            analogLight = false;
                            mode = 0x41u;
                        }
                    }
                    return n == 8u ? 0x5Au : 0x00u;
                default:
                    return 0x00u;
                }
            }
        };

        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            ds2Pad = {};
            intrman.reset();
            timrman.reset();
            vblank.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sio2man") && dispatchSio2man(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        // sio2man (v2 export table, ps2sdk iop/sio/sio2man/src/exports.tab) as
        // used by the disc PADMAN.IRX: 11 stat70_get, 46 pad_transfer_init2,
        // 51 transfer2, 52 transfer_reset2, 57 mtap_change_slot, 58
        // mtap_get_slot_max, 60 mtap_update_slots. The SIO2 bus is not emulated,
        // so transfer2 answers the packet directly the way PCSX2's Sio2::Write /
        // Sio2::Pad do. Before this, every call returned 0: PADMAN got empty
        // transfers and SIF-DMAed id-0 frames into libpad's buffer, racing the
        // runtime's pad push, so most button presses never reached the game.
        bool dispatchSio2man(uint16_t ordinal, CpuState &cpu)
        {
            switch (ordinal)
            {
            case 11: // sio2_stat70_get -- PortStat::DEFAULT
                cpu.gpr[2] = 0xFu;
                return true;
            case 46: // sio2_pad_transfer_init2 (transfer lock; single IOP user here)
            case 52: // sio2_transfer_reset2 (unlock)
            case 60: // sio2_mtap_update_slots (default callback is empty)
                cpu.gpr[2] = 0u;
                return true;
            case 57: // sio2_mtap_change_slot -- sio2man default callback
            {
                const uint32_t arg = cpu.gpr[4];
                uint32_t sum = 0u;
                for (uint32_t i = 0; i < 4u; ++i)
                {
                    const int32_t slot = static_cast<int32_t>(memory.read32(arg + i * 4u));
                    const uint32_t ok = (slot + 1) < 2 ? 1u : 0u;
                    memory.write32(arg + (i + 4u) * 4u, ok);
                    sum += ok;
                }
                cpu.gpr[2] = sum == 4u ? 1u : 0u;
                return true;
            }
            case 58: // sio2_mtap_get_slot_max -- default: one slot
                cpu.gpr[2] = 1u;
                return true;
            case 51: // sio2_transfer2
                sio2Transfer(cpu.gpr[4]);
                cpu.gpr[2] = 1u;
                return true;
            default:
                return false;
            }
        }

        // sio2_transfer_data_t: stat6c +0x00, port_ctrl1[4] +0x04, port_ctrl2[4]
        // +0x14, stat70 +0x24, regdata[16] +0x28, stat74 +0x68, in_size +0x6C,
        // out_size +0x70, in +0x74, out +0x78 (layout confirmed by PADMAN's own
        // accesses at 0x9310..0x9388).
        void sio2Transfer(uint32_t td)
        {
            const uint32_t inSize = memory.read32(td + 0x6Cu);
            const uint32_t outSize = memory.read32(td + 0x70u);
            const uint32_t inPtr = memory.read32(td + 0x74u);
            const uint32_t outPtr = memory.read32(td + 0x78u);
            const uint16_t buttons = static_cast<uint16_t>(g_ps2x_sio2_pad_buttons.load(std::memory_order_relaxed));
            const uint32_t analog = g_ps2x_sio2_pad_analog.load(std::memory_order_relaxed);

            std::vector<uint8_t> out;
            uint32_t cmdStat = 0u;
            uint32_t inPos = 0u;
            const auto portOpened = [&]()
            {
                if (cmdStat & 0x100u)
                {
                    cmdStat &= ~0x100u;
                    cmdStat |= 0x200u;
                }
                else
                {
                    cmdStat |= 0x100u;
                }
            };
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t reg = memory.read32(td + 0x28u + i * 4u);
                const uint32_t length = (reg >> 8) & 0x3FFu;
                if (reg == 0u || length == 0u)
                    break;
                const uint32_t port = reg & 1u;
                const auto inByte = [&](uint32_t k) -> uint8_t
                {
                    return inPos + k < inSize ? memory.read8(inPtr + inPos + k) : 0u;
                };
                const uint8_t sioMode = inByte(0u);
                if (sioMode == 0x01u) // PAD
                {
                    portOpened();
                    cmdStat |= 0x1000u; // NO_DEVICES_MISSING, always set
                    out.push_back(0xFFu);
                    if (port == 0u)
                    {
                        ds2Pad.softReset();
                        for (uint32_t k = 1; k < length; ++k)
                            out.push_back(ds2Pad.send(inByte(k), buttons, analog));
                        if (ds2Pad.command == 0x42u)
                            g_ps2x_sio2_pad_served.fetch_add(1u, std::memory_order_relaxed);
                    }
                    else
                    {
                        cmdStat |= 0x2D000u; // PORT_2_MISSING
                        for (uint32_t k = 1; k < length; ++k)
                            out.push_back(0xFFu);
                    }
                }
                else if (sioMode == 0x21u) // MULTITAP, none attached
                {
                    portOpened();
                    cmdStat |= 0x1000u | 0x1D000u;
                    for (uint32_t k = 0; k < length; ++k)
                        out.push_back(0xFFu);
                }
                else
                {
                    cmdStat = 0x1D100u; // DISCONNECTED
                    for (uint32_t k = 0; k < length; ++k)
                        out.push_back(0xFFu);
                }
                inPos += length;
            }

            for (uint32_t k = 0; k < outSize; ++k)
                memory.write8(outPtr + k, k < out.size() ? out[k] : 0xFFu);
            memory.write32(td + 0x00u, cmdStat);
            memory.write32(td + 0x24u, 0xFu);
            memory.write32(td + 0x68u, 0u);
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (const auto import = imports.decode(cpu.pc))
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            if (cpu.pc != kCallReturnSentinel)
            {
                // The call did not return; its CpuState is discarded here. Never silent.
                static uint32_t s_abandonLogs = 0u;
                if (s_abandonLogs++ < 32u)
                {
                    std::ostringstream warn;
                    warn << "[IOP] WARN guest call abandoned fn=0x" << std::hex << address << " pc=0x" << cpu.pc
                         << std::dec << " budget=" << budget << (cpu.yielded ? " reason=yield" : " reason=budget");
                    log(LogLevel::Warning, warn.str());
                }
            }
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    vblank.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        nextWake = vblank.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            // Module start runs to completion before LoadModule returns on real
            // hardware; don't cap it at the default 2M-instruction call budget.
            static constexpr uint32_t kModuleStartBudget = 400'000'000u;
            const uint64_t startInstructions = totalInstructions;
            const uint32_t startResult =
                callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp, kModuleStartBudget);
            const uint64_t startUsed = totalInstructions - startInstructions;
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult << " instr=" << startUsed;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        Ds2Pad ds2Pad;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        // EeScheduler::accountCycles() calls this on every guest checkpoint,
        // usually with a handful of EE cycles. Running the full IOP service
        // loop for 1-2 IOP cycles each time was 13.5% of the game thread
        // (09-27 host profile, all in IopKernel::nextWakeCycle). Bank the EE
        // cycles and run the IOP in batches instead. 2048 EE = 256 IOP cycles,
        // well under 0.1% of a frame. PS2X_IOP_BATCH (EE cycles) overrides;
        // 8 restores the old per-checkpoint behaviour.
        static const uint64_t kBatchEe = []
        {
            if (const char *v = std::getenv("PS2X_IOP_BATCH"))
            {
                const long long n = std::atoll(v);
                if (n >= 8)
                    return static_cast<uint64_t>(n);
            }
            return uint64_t{2048};
        }();
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        if (total < kBatchEe)
        {
            m_impl->eeCycleCarry = total;
            return;
        }
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
