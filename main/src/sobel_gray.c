#include "sobel_gray.h"
#define SOBEL_GAIN 2

#define SOBEL_BLACK_LEVEL 15
#define SOBEL_CONTRAST 2

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline uint8_t sample(const uint8_t *src, int w, int h, int x, int y) {
    x = clampi(x, 0, w - 1);
    y = clampi(y, 0, h - 1);
    return src[y * w + x];
}

static inline uint8_t combine(int p00,int p01,int p02,int p10,int p12,int p20,int p21,int p22,int threshold) {
    int gx = (p02 + 2*p12 + p22) - (p00 + 2*p10 + p20);
    int gy = (p20 + 2*p21 + p22) - (p00 + 2*p01 + p02);
    int mag =( (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy) )*SOBEL_GAIN;
/*intentando mejorar la nitidez*/
    if (mag <= SOBEL_BLACK_LEVEL) {
    mag = 0;
} else {
    mag = (mag - SOBEL_BLACK_LEVEL) * SOBEL_CONTRAST;
}
/*fin intento*/

    if (mag > 255) mag = 255;
    if (threshold >= 0) return (mag >= threshold) ? 255 : 0;
    return (uint8_t)mag;
}


void gaussian3x3_gray8(
    const uint8_t *src,
    uint8_t *dst,
    int width,
    int height
)
{
    if (src == NULL ||
        dst == NULL ||
        width <= 0 ||
        height <= 0) {
        return;
    }

    for (int y = 0; y < height; y++) {

        int ym1 = (y > 0) ? y - 1 : y;
        int yp1 = (y < height - 1) ? y + 1 : y;

        for (int x = 0; x < width; x++) {

            int xm1 = (x > 0) ? x - 1 : x;
            int xp1 = (x < width - 1) ? x + 1 : x;

            int p00 = src[ym1 * width + xm1];
            int p01 = src[ym1 * width + x];
            int p02 = src[ym1 * width + xp1];

            int p10 = src[y * width + xm1];
            int p11 = src[y * width + x];
            int p12 = src[y * width + xp1];

            int p20 = src[yp1 * width + xm1];
            int p21 = src[yp1 * width + x];
            int p22 = src[yp1 * width + xp1];

            int sum =
                  p00
                + 2 * p01
                + p02
                + 2 * p10
                + 4 * p11
                + 2 * p12
                + p20
                + 2 * p21
                + p22;

            dst[y * width + x] =
                (uint8_t)((sum + 8) >> 4);
        }
    }
}


void sobel_gray8_threshold(const uint8_t *src, uint8_t *dst, int width, int height, int threshold)
{
    // --- Núcleo interior: sin clamp, punteros de fila ---
    for (int y = 1; y < height - 1; y++) {
        const uint8_t *rp = src + (y - 1) * width; // row prev
        const uint8_t *rc = src + y * width;       // row curr
        const uint8_t *rn = src + (y + 1) * width; // row next
        uint8_t *d = dst + y * width;

        // borde izquierdo (x=0) con clamp
        d[0] = combine(sample(src,width,height,-1,y-1), rp[0], rp[1],
                        sample(src,width,height,-1,y),          rc[1],
                        sample(src,width,height,-1,y+1), rn[0], rn[1], threshold);

        for (int x = 1; x < width - 1; x++) {
            d[x] = combine(rp[x-1], rp[x], rp[x+1],
                            rc[x-1],        rc[x+1],
                            rn[x-1], rn[x], rn[x+1], threshold);
        }

        // borde derecho (x=width-1) con clamp
        int xl = width - 1;
        d[xl] = combine(rp[xl-1], rp[xl], sample(src,width,height,width,y-1),
                         rc[xl-1],         sample(src,width,height,width,y),
                         rn[xl-1], rn[xl], sample(src,width,height,width,y+1), threshold);
    }

    // --- Fila superior e inferior: sí necesitan clamp en todo x ---
    for (int y = 0; y < height; y += (height > 1 ? height - 1 : 1)) {
        for (int x = 0; x < width; x++) {
            int p00=sample(src,width,height,x-1,y-1), p01=sample(src,width,height,x,y-1), p02=sample(src,width,height,x+1,y-1);
            int p10=sample(src,width,height,x-1,y),                                        p12=sample(src,width,height,x+1,y);
            int p20=sample(src,width,height,x-1,y+1), p21=sample(src,width,height,x,y+1), p22=sample(src,width,height,x+1,y+1);
            dst[y*width+x] = combine(p00,p01,p02,p10,p12,p20,p21,p22,threshold);
        }
        if (height == 1) break;
    }
}

void sobel_gray8(const uint8_t *src, uint8_t *dst, int width, int height)
{
    sobel_gray8_threshold(src, dst, width, height, -1);
}