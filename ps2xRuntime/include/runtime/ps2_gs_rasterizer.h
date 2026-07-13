#ifndef PS2_GS_RASTERIZER_H
#define PS2_GS_RASTERIZER_H

#include <cstdint>

class GS;

class GSRasterizer
{
public:
    void drawPrimitive(GS *gs);
    void writePixel(GS *gs, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    uint32_t sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v);

private:
    void drawSprite(GS *gs);
    void drawTriangle(GS *gs);
    void drawLine(GS *gs);
};

#endif
