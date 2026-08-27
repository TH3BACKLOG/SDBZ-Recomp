#pragma once

// New orchestration types introduced by upstream PR #204's GS frontend/backend
// split (GSRasterBackend interface). Deliberately does NOT redefine the
// register/context types PR #204's own gs_types.h would introduce -- SDBZ
// already has richer bitfield-union versions of all of them (GSContext in
// ps2_gs_gpu.h; GSPrimReg/GSTexaReg/GSTexClutReg/GSFrameReg/GSBitBltBufReg/
// GSTrxPosReg/GSTrxReg/etc in ps2_gs_gpr.h, ported in ahead of this PR). This
// header only adds the NEW batch/transfer/presentation types the backend
// interface needs, built on top of those existing types.

#include "ps2_gs_gpu.h"
#include "ps2_gs_gpr.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct GSVertex
{
    float x = 0.0f;
    float y = 0.0f;
    double z = 0.0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
    float q = 1.0f;
    float s = 0.0f;
    float t = 0.0f;
    uint16_t u = 0;
    uint16_t v = 0;
    uint8_t fog = 0;
};

struct GSDrawState
{
    GSContext context{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    bool pabe = false;
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    uint8_t fogR = 0;
    uint8_t fogG = 0;
    uint8_t fogB = 0;
    uint16_t textureWidth = 1;
    uint16_t textureHeight = 1;
    bool linearFilter = false;
};

struct GSPrimitiveBatch
{
    std::array<GSVertex, 3> vertices{};
    uint8_t vertexCount = 0;
    GSDrawState state{};
};

struct GSTransferCommand
{
    GSBitBltBufReg bitbltbuf{};
    GSTrxPosReg trxpos{};
    GSTrxReg trxreg{};
    uint32_t direction = 3;
};

struct GSTransferSnapshot
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t totalPixels = 0;
    uint32_t copiedPixels = 0;
    uint32_t direction = 3;
    size_t localToHostPendingBytes = 0;
};

struct GSPresentationRequest
{
    uint64_t pmode = 0;
    uint64_t smode2 = 0;
    uint64_t dispfb1 = 0;
    uint64_t display1 = 0;
    uint64_t dispfb2 = 0;
    uint64_t display2 = 0;
    uint64_t bgcolor = 0;
    uint64_t vsyncTick = 0;
    GSFrameReg contextFrames[2]{};
    GSFrameReg preferredSource{};
    uint32_t preferredDestFbp = 0;
    bool hasPreferredSource = false;
};

struct PresentationFrame
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    bool usedPreferred = false;

    explicit operator bool() const
    {
        return !pixels.empty() && width != 0u && height != 0u;
    }
};

enum class GSSyncReason : uint8_t
{
    Finish,
    LocalToHost,
    Presentation,
    DebugReadback,
    Reset,
};
