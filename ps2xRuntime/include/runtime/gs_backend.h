#pragma once

// GSRasterBackend: the rasterizer-side interface introduced by upstream PR
// #204's frontend/backend split. The frontend (GIF-packet parsing, register
// dispatch -- currently ps2_gs_gpu.cpp) will drive an implementation of this
// interface (currently ps2_gs_rasterizer.cpp's logic, to be ported into a
// GSCpuBackend) instead of calling rasterizer functions directly.

#include "runtime/gs_types.h"

#include <cstdint>
#include <vector>

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // SDBZ-only extensions beyond upstream PR #204's interface (upstream's
    // backend never exposes texture-page/CLUT caching to the frontend -- it
    // has no CLUT cache at all). GS::writeRegister/writeRegisterPacked call
    // these directly as immediate side effects of TEX0/TEXFLUSH register
    // writes (see ps2_gs_gpu.cpp: GS_REG_TEX0_1/2 -> ReloadClutCache,
    // GS_REG_TEXFLUSH -> InvalidateTexturePageCache), so the frontend needs a
    // way to trigger them on the backend that now privately owns the caches.
    virtual uint32_t ReadTexturePageCache(uint32_t psm, uint32_t tbp0, uint32_t tbw, uint32_t u, uint32_t v) = 0;
    virtual void ReloadTexturePageCache(uint32_t psm, uint32_t baseBlock) = 0;
    virtual void InvalidateTexturePageCache() = 0;

    virtual uint32_t ReadClutCache(uint32_t psm, uint8_t index, uint32_t csa) = 0;
    virtual void ReloadClutCacheCSM1(uint32_t psm, uint32_t cpsm, uint32_t cbp, uint32_t csa) = 0;
    virtual void ReloadClutCacheCSM2(uint32_t psm, uint32_t cbp) = 0;
    virtual void ReloadClutCache(uint32_t psm, uint32_t cpsm, uint32_t cbp, uint8_t csm, uint8_t csa, uint8_t cld) = 0;
};
