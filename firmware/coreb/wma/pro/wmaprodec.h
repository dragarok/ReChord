#include "codeclib.h"
#include "wma.h"
#include "wma_wfx.h"

/* ReChord: no IRAM tiers on core B */
#define IBSS_ATTR_WMAPRO_LARGE_IRAM
#define ICODE_ATTR_WMAPRO_LARGE_IRAM
#define ICONST_ATTR_WMAPRO_LARGE_IRAM
#define IBSS_ATTR_WMAPRO_VLC_TABLES
#define ICONST_ATTR_WMAPRO_WIN_VS_TMP

int decode_init(asf_waveformatex_t *wfx);
int decode_packet(asf_waveformatex_t *wfx,
                  int32_t *dec[2], int *data_size, void* pktdata, int size);
