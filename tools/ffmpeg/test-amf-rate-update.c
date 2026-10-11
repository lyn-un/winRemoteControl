/* Exercise the actual patched helper against a deterministic AMF stub. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define AVERROR(e) (-(e))
#define AVERROR_EXTERNAL (-10000)
#define AV_CODEC_ID_H264 27
#define AV_LOG_ERROR 16
#define AV_LOG_INFO 32
#define av_log(...) ((void)0)
#define AMF_OK 0
#define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP 0
#define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR 1
#define AMF_VIDEO_ENCODER_TARGET_BITRATE L"TargetBitrate"
#define AMF_VIDEO_ENCODER_PEAK_BITRATE L"PeakBitrate"
#define AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE L"VBVBufferSize"
typedef int AMF_RESULT;
typedef struct AMFEncoderContext {
    void *encoder;
    int64_t applied_bitrate, applied_peak_bitrate, applied_buffer_size;
    int bitrate_error, rate_control_mode;
} AMFEncoderContext;
typedef struct AVCodecContext {
    void *priv_data;
    int codec_id;
    int64_t bit_rate, rc_max_rate;
    int rc_buffer_size;
} AVCodecContext;

static unsigned calls, fail_at;
static const wchar_t *properties[2048];
static int64_t values[2048];
static int mock_set(void *encoder, const wchar_t *property, int64_t value)
{
    assert(encoder && calls < 2048);
    properties[calls] = property;
    values[calls++] = value;
    return calls == fail_at ? 7 : AMF_OK;
}
#define AMF_ASSIGN_PROPERTY_INT64(result, object, name, value) \
    ((result) = mock_set((object), (name), (value)))
#include "amf-rate-update-under-test.h"

static void reset(AVCodecContext *avctx, AMFEncoderContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->encoder = ctx;
    ctx->rate_control_mode = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR;
    *avctx = (AVCodecContext){ctx, AV_CODEC_ID_H264, 1000000, 1000000, 500000};
    calls = fail_at = 0;
}

int main(void)
{
    AMFEncoderContext ctx;
    AVCodecContext avctx;
    unsigned before;
    reset(&avctx, &ctx);
    assert(ff_amf_update_h264_bitrate(&avctx, 1) == 0 && calls == 3);
    assert(wcscmp(properties[0], L"PeakBitrate") == 0);
    assert(wcscmp(properties[2], L"TargetBitrate") == 0);
    assert(ctx.applied_bitrate == 1000000);
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0 && calls == 3);
    avctx.bit_rate = 0;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0 && calls == 3);
    avctx.bit_rate = -1;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0 && calls == 3);
    avctx.bit_rate = avctx.rc_max_rate = 6000000;
    avctx.rc_buffer_size = 3000000;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0 && calls == 6);
    assert(wcscmp(properties[3], L"PeakBitrate") == 0);
    avctx.bit_rate = avctx.rc_max_rate = 1000000;
    avctx.rc_buffer_size = 500000;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0 && calls == 9);
    assert(wcscmp(properties[7], L"TargetBitrate") == 0);
    assert(wcscmp(properties[8], L"PeakBitrate") == 0);
    for (int i = 0; i < 200; ++i) {
        avctx.bit_rate = avctx.rc_max_rate = 2000000 + i;
        assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0);
        assert(ctx.applied_bitrate == avctx.bit_rate);
        before = calls;
        assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0 && calls == before);
    }
    avctx.bit_rate = avctx.rc_max_rate = INT64_MAX;
    avctx.rc_buffer_size = INT_MAX;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0);
    assert(ctx.applied_bitrate == INT64_MAX && values[calls - 1] == INT64_MAX);

    /* Fail each position in an update: no partial success or repeated retries. */
    for (unsigned position = 1; position <= 3; ++position) {
        reset(&avctx, &ctx);
        assert(ff_amf_update_h264_bitrate(&avctx, 1) == 0);
        before = calls;
        fail_at = calls + position;
        avctx.bit_rate = avctx.rc_max_rate = 6000000;
        avctx.rc_buffer_size = 3000000;
        assert(ff_amf_update_h264_bitrate(&avctx, 0) == AVERROR_EXTERNAL);
        assert(calls == before + position && ctx.applied_bitrate == 1000000);
        before = calls;
        assert(ff_amf_update_h264_bitrate(&avctx, 0) == AVERROR_EXTERNAL && calls == before);
    }
    reset(&avctx, &ctx);
    fail_at = 1;
    assert(ff_amf_update_h264_bitrate(&avctx, 1) < 0 && ctx.applied_bitrate == 0);
    reset(&avctx, &ctx);
    avctx.rc_max_rate = 999999;
    assert(ff_amf_update_h264_bitrate(&avctx, 1) == AVERROR(EINVAL) && calls == 0);
    reset(&avctx, &ctx);
    avctx.rc_buffer_size = -1;
    assert(ff_amf_update_h264_bitrate(&avctx, 1) == AVERROR(EINVAL) && calls == 0);
    reset(&avctx, &ctx);
    avctx.rc_max_rate = 0; /* CBR derives peak from target. */
    assert(ff_amf_update_h264_bitrate(&avctx, 1) == 0 && ctx.applied_peak_bitrate == 1000000);
    reset(&avctx, &ctx);
    ctx.rate_control_mode = 2; /* VBR must preserve an explicit peak. */
    avctx.rc_max_rate = 2000000;
    assert(ff_amf_update_h264_bitrate(&avctx, 1) == 0 && ctx.applied_peak_bitrate == 2000000);
    avctx.rc_max_rate = 0;
    before = calls;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) < 0 && calls == before);
    reset(&avctx, &ctx);
    ctx.rate_control_mode = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == AVERROR(ENOSYS) && calls == 0);
    reset(&avctx, &ctx);
    avctx.codec_id = 999;
    assert(ff_amf_update_h264_bitrate(&avctx, 0) == 0 && calls == 0);
    puts("AMF bitrate helper: all checks passed (stub, not hardware acceptance)");
    return 0;
}
