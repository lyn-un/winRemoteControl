#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1 };
enum { NV_ENC_PARAMS_RC_CONSTQP = 1, NV_ENC_SUCCESS = 0, NV_ENC_RECONFIGURE_PARAMS_VER = 1, AV_LOG_VERBOSE = 1 };
typedef int NVENCSTATUS;
typedef struct AVFrame { int unused; } AVFrame;
typedef struct Rate { uint32_t averageBitRate, maxBitRate, vbvBufferSize; } Rate;
typedef struct Config { Rate rcParams; } Config;
typedef struct Init { int darWidth, darHeight; Config *encodeConfig; } Init;
typedef struct NV_ENC_RECONFIGURE_PARAMS {
    int version, resetEncoder, forceIDR;
    Init reInitEncodeParams;
} NV_ENC_RECONFIGURE_PARAMS;
typedef struct NV_ENCODE_API_FUNCTION_LIST {
    NVENCSTATUS (*nvEncReconfigureEncoder)(void *, NV_ENC_RECONFIGURE_PARAMS *);
} NV_ENCODE_API_FUNCTION_LIST;
typedef struct NvencContext {
    struct { NV_ENCODE_API_FUNCTION_LIST nvenc_funcs; } nvenc_dload_funcs;
    Init init_encode_params;
    Config encode_config;
    void *nvencoder;
    int rc, support_dyn_bitrate, bitrate_reconfig_reset, bitrate_reconfig_idr;
} NvencContext;
typedef struct AVCodecContext {
    void *priv_data;
    int codec_id, rc_buffer_size;
    int64_t bit_rate, rc_max_rate;
} AVCodecContext;
static int calls, errors, status, dar_width = 16, dar_height = 9;
static NV_ENC_RECONFIGURE_PARAMS recorded;
static NVENCSTATUS record(void *encoder, NV_ENC_RECONFIGURE_PARAMS *params)
{
    assert(encoder);
    recorded = *params;
    ++calls;
    return status;
}
static void compute_dar(AVCodecContext *avctx, int *width, int *height)
{
    *width = dar_width;
    *height = dar_height;
}
static void av_log(void *context, int level, const char *format, ...) {}
static void nvenc_print_error(void *context, int error, const char *message)
{
    assert(error && message);
    ++errors;
}
#include "nvenc-reconfiguration-under-test.h"

static void run_case(int codec, int reset, int idr, int dar, int change, int supported, int constqp, int failure)
{
    NvencContext ctx = {0};
    AVCodecContext avctx = {0};
    ctx.nvenc_dload_funcs.nvenc_funcs.nvEncReconfigureEncoder = record;
    ctx.encode_config.rcParams = (Rate){1000000, 1000000, 250000};
    ctx.init_encode_params = (Init){16, 9, &ctx.encode_config};
    ctx.nvencoder = &ctx;
    ctx.support_dyn_bitrate = supported;
    ctx.rc = constqp ? NV_ENC_PARAMS_RC_CONSTQP : 0;
    ctx.bitrate_reconfig_reset = reset;
    ctx.bitrate_reconfig_idr = idr;
    avctx.priv_data = &ctx;
    avctx.codec_id = codec;
    avctx.bit_rate = change ? 2000000 : 1000000;
    avctx.rc_max_rate = avctx.bit_rate;
    avctx.rc_buffer_size = change ? 500000 : 250000;
    dar_width = dar ? 4 : 16;
    calls = errors = 0;
    status = failure;
    reconfig_encoder(&avctx, NULL);
    int rate_change = change && supported && !constqp;
    assert(calls == !!(rate_change || dar));
    if (calls) {
        int custom = codec == AV_CODEC_ID_H264 && !dar;
        assert(recorded.resetEncoder == (rate_change ? (custom ? reset : 1) : 0));
        assert(recorded.forceIDR == (rate_change ? (custom ? idr : 1) : 0));
        assert(!!recorded.reInitEncodeParams.encodeConfig == rate_change);
        assert(errors == !!failure);
    }
    if (!failure) {
        calls = 0;
        reconfig_encoder(&avctx, NULL);
        assert(calls == 0);
    }
}

int main(void)
{
    int count = 0;
    for (int codec = AV_CODEC_ID_H264; codec <= AV_CODEC_ID_AV1; ++codec)
        for (int reset = 0; reset <= 1; ++reset)
            for (int idr = 0; idr <= 1; ++idr)
                for (int dar = 0; dar <= 1; ++dar)
                    for (int change = 0; change <= 1; ++change)
                        for (int supported = 0; supported <= 1; ++supported)
                            for (int constqp = 0; constqp <= 1; ++constqp)
                                for (int failure = 0; failure <= 1; ++failure) {
                                    run_case(codec, reset, idr, dar, change, supported, constqp, failure);
                                    ++count;
                                }
    printf("NVENC reconfiguration: %d branch combinations passed (SDK stub, not hardware)\n", count);
    return 0;
}
