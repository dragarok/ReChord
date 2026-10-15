/*
 * alac_dec.c -- ALAC frame decoding (alac_dec.h). Follows Apple's
 * ALACDecoder::Decode element by element (SCE/LFE, CPE, DSE, FIL, END); the
 * unmixing writes 32-bit words straight away. Sample counts from the stream are
 * clamped to the configured frame length (Apple's code trusts them).
 */
#include <string.h>

#include "alac_dec.h"
#include "ALACAudioTypes.h"
#include "ALACBitUtilities.h"
#include "aglib.h"
#include "dplib.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int alac_init(alac_t *a, const uint8_t *c, void *(*alloc)(size_t))
{
    memset(a, 0, sizeof *a);
    a->frame_len = be32(c);
    a->bits = c[5];
    a->pb = c[6];
    a->mb = c[7];
    a->kb = c[8];
    a->channels = c[9];
    a->max_run = (uint16_t)(c[10] << 8 | c[11]);
    if (c[4] > kALACVersion || !a->frame_len || a->frame_len > 16384u || a->channels < 1 || a->channels > 2 ||
        (a->bits != 16 && a->bits != 20 && a->bits != 24 && a->bits != 32))
        return -1;
    a->u = alloc(a->frame_len * 4u);
    a->v = alloc(a->frame_len * 4u);
    a->pred = alloc(a->frame_len * 4u);
    a->shift = alloc(a->frame_len * 4u);
    return a->u && a->v && a->pred && a->shift ? 0 : -1;
}

/* Prediction residuals -> samples for one channel (mode 0, or the doubled filter). */
static int channel(alac_t *a, BitBuffer *bits, int32_t *dst, uint32_t n, uint32_t chan_bits,
                   uint32_t mode, uint32_t den, uint32_t pbf, int16_t *coefs, uint32_t num)
{
    AGParamRec ag;
    uint32_t used;

    set_ag_params(&ag, a->mb, (a->pb * pbf) / 4u, a->kb, n, n, a->max_run);
    if (dyn_decomp(&ag, bits, a->pred, (int32_t)n, (int32_t)chan_bits, &used))
        return -1;
    if (mode != 0)
        unpc_block(a->pred, a->pred, (int32_t)n, NULL, 31, chan_bits, 0);
    unpc_block(a->pred, dst, (int32_t)n, coefs, (int32_t)num, chan_bits, den);
    return 0;
}

static int32_t escaped(BitBuffer *bits, uint32_t chan_bits)
{
    uint32_t shift = 32u - chan_bits;
    int32_t v;

    if (chan_bits <= 16u)
        return (int32_t)(BitBufferRead(bits, (uint8_t)chan_bits) << shift) >> shift;
    v = (int32_t)(BitBufferRead(bits, 16) << 16) >> shift;
    return v | (int32_t)BitBufferRead(bits, (uint8_t)(chan_bits - 16u));
}

/* One channel element: SCE (1) or CPE (2). Writes its channels into out. */
static int element(alac_t *a, BitBuffer *bits, int pair, int32_t *out, uint32_t stride, uint32_t *n)
{
    uint32_t hdr, shifted, esc, chan_bits, mix_bits = 0, nn = a->frame_len, wshift = 32u - a->bits;
    int32_t mix_res = 0;
    int16_t coefs[2][32];
    uint32_t mode[2], den[2], pbf[2], num[2];
    BitBuffer sbits;
    int k = pair ? 2 : 1;

    BitBufferReadSmall(bits, 4);                 /* element instance tag */
    if (BitBufferRead(bits, 12) != 0)
        return -1;
    hdr = BitBufferRead(bits, 4);
    shifted = (hdr >> 1) & 3u;
    esc = hdr & 1u;
    if (shifted == 3u)
        return -1;
    if (hdr >> 3) {                              /* partial frame */
        nn = BitBufferRead(bits, 16) << 16;
        nn |= BitBufferRead(bits, 16);
        if (nn > a->frame_len)
            return -1;
    }
    chan_bits = a->bits - shifted * 8u + (pair ? 1u : 0u);
    if (!esc) {
        mix_bits = BitBufferRead(bits, 8);
        mix_res = (int8_t)BitBufferRead(bits, 8);
        for (int c = 0; c < k; c++) {
            uint32_t b = BitBufferRead(bits, 8);

            mode[c] = b >> 4;
            den[c] = b & 15u;
            b = BitBufferRead(bits, 8);
            pbf[c] = b >> 5;
            num[c] = b & 31u;
            for (uint32_t i = 0; i < num[c]; i++)
                coefs[c][i] = (int16_t)BitBufferRead(bits, 16);
        }
        if (shifted) {
            sbits = *bits;
            BitBufferAdvance(bits, shifted * 8u * (uint32_t)k * nn);
        }
        if (channel(a, bits, a->u, nn, chan_bits, mode[0], den[0], pbf[0], coefs[0], num[0]))
            return -1;
        if (pair && channel(a, bits, a->v, nn, chan_bits, mode[1], den[1], pbf[1], coefs[1], num[1]))
            return -1;
        if (shifted) {
            for (uint32_t i = 0; i < nn * (uint32_t)k; i++)
                a->shift[i] = (uint16_t)BitBufferRead(&sbits, (uint8_t)(shifted * 8u));
        }
    } else {
        chan_bits = a->bits;
        for (uint32_t i = 0; i < nn; i++) {
            a->u[i] = escaped(bits, chan_bits);
            if (pair)
                a->v[i] = escaped(bits, chan_bits);
        }
        shifted = 0;
    }
    for (uint32_t i = 0; i < nn; i++) {
        int32_t l = a->u[i], r = 0;

        if (pair) {
            r = a->v[i];
            if (mix_res) {
                l = a->u[i] + a->v[i] - ((mix_res * a->v[i]) >> mix_bits);
                r = l - a->v[i];
            }
        }
        if (shifted) {
            uint32_t s = shifted * 8u;

            l = (int32_t)((uint32_t)l << s) | a->shift[i * (uint32_t)k];
            if (pair)
                r = (int32_t)((uint32_t)r << s) | a->shift[i * 2u + 1u];
        }
        out[i * stride] = (int32_t)((uint32_t)l << wshift);
        if (pair)
            out[i * stride + 1u] = (int32_t)((uint32_t)r << wshift);
    }
    *n = nn;
    return 0;
}

int alac_frame(alac_t *a, uint8_t *pkt, uint32_t len, int32_t *out, uint32_t *n)
{
    BitBuffer bits;
    uint32_t ch = 0;

    BitBufferInit(&bits, pkt, len);
    *n = a->frame_len;
    while (ch < a->channels) {
        uint32_t tag, cnt;

        if (bits.cur >= bits.end)
            return -1;
        tag = BitBufferReadSmall(&bits, 3);
        switch (tag) {
        case ID_SCE:
        case ID_LFE:
            if (element(a, &bits, 0, out + ch, a->channels, n))
                return -1;
            ch += 1;
            break;
        case ID_CPE:
            if (ch + 2u > a->channels || element(a, &bits, 1, out + ch, a->channels, n))
                return -1;
            ch += 2;
            break;
        case ID_DSE:
            BitBufferReadSmall(&bits, 4);
            tag = BitBufferReadOne(&bits);
            cnt = BitBufferReadSmall(&bits, 8);
            if (cnt == 255)
                cnt += BitBufferReadSmall(&bits, 8);
            if (tag)
                BitBufferByteAlign(&bits, 0);
            BitBufferAdvance(&bits, cnt * 8u);
            break;
        case ID_FIL:
            cnt = BitBufferReadSmall(&bits, 4);
            if (cnt == 15)
                cnt += BitBufferReadSmall(&bits, 8) - 1u;
            BitBufferAdvance(&bits, cnt * 8u);
            break;
        case ID_END:
            return ch == a->channels ? 0 : -1;
        default:
            return -1;                           /* CCE, PCE */
        }
        if (bits.cur > bits.end)
            return -1;
    }
    return 0;
}
