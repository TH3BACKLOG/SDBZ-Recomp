#ifndef PS2_IOP_H
#define PS2_IOP_H

#include <cstdint>

class PS2Runtime;

constexpr uint32_t IOP_SID_LOTR_CLFILE = 0x0000FF01u;
constexpr uint32_t IOP_SID_LOTR_SOUND = 0x00012345u;
constexpr uint32_t IOP_SID_LIBSD = 0x80000701u;
constexpr uint32_t IOP_SID_FATAL_FRAME_SDRDRV = 0x19740512u;

// MCSERV.IRX real memory-card RPC service SID seen on SDBZ runtime logs.
// Keep legacy SID as fallback for other module revisions.
constexpr uint32_t IOP_SID_MCSERV = 0x80000400u;
constexpr uint32_t IOP_SID_MCSERV_LEGACY = 0x80000080u;

// cdvdman S-command RPC server. EE libcdvd routes sceCdReadDvdDualInfo (and other
// S-commands) through this server; the result buffer's first word is the success flag.
// Only used as a legacy HLE fallback for games that don't route through the real IRX
// interpreter's own cdvdman RPC service.
constexpr uint32_t IOP_SID_CDVD_SCMD = 0x80000593u;

class ps2_iop
{
public:
    ps2_iop();
    ~ps2_iop() = default;

    void init(uint8_t *rdram);
    void reset();

    bool handleRPC(PS2Runtime *runtime,
                   uint32_t sid, uint32_t rpcNum,
                   uint32_t sendBufAddr, uint32_t sendSize,
                   uint32_t recvBufAddr, uint32_t recvSize,
                   uint32_t &resultPtr,
                   bool &signalNowaitCompletion);

private:
    uint8_t *m_rdram = nullptr;
};

#endif
