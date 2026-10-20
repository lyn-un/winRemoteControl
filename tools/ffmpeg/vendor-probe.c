/* Build-capability probe. Enumeration is not a hardware availability test. */
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>

int main(void)
{
    const char *names[] = { "libx264", "h264_mf", "h264_qsv", "h264_amf", "h264_nvenc" };
    int missing = 0;
    printf("ffmpeg=%s\n", av_version_info());
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const AVCodec *codec = avcodec_find_encoder_by_name(names[i]);
        printf("encoder=%s compiled=%d hardwareVerified=0\n", names[i], codec != NULL);
        missing |= codec == NULL;
    }
    const AVCodec *nvenc = avcodec_find_encoder_by_name("h264_nvenc");
    AVCodecContext *context = avcodec_alloc_context3(nvenc);
    if (!context)
        return 1;
    int64_t reset, idr;
    int has_reset = av_opt_get_int(context->priv_data, "bitrate-reconfig-reset", 0, &reset) >= 0;
    int has_idr = av_opt_get_int(context->priv_data, "bitrate-reconfig-idr", 0, &idr) >= 0;
    printf("nvencReconfigurationOptions=%d\n", has_reset && has_idr);
    if (has_reset || has_idr) {
        missing |= !has_reset || !has_idr || reset != 1 || idr != 1;
        for (int r = 0; r <= 1; ++r)
            for (int i = 0; i <= 1; ++i) {
                missing |= av_opt_set_int(context->priv_data, "bitrate-reconfig-reset", r, 0) < 0;
                missing |= av_opt_set_int(context->priv_data, "bitrate-reconfig-idr", i, 0) < 0;
                missing |= av_opt_get_int(context->priv_data, "bitrate-reconfig-reset", 0, &reset) < 0 || reset != r;
                missing |= av_opt_get_int(context->priv_data, "bitrate-reconfig-idr", 0, &idr) < 0 || idr != i;
            }
        missing |= av_opt_set_int(context->priv_data, "bitrate-reconfig-reset", 2, 0) >= 0;
        missing |= av_opt_set_int(context->priv_data, "bitrate-reconfig-idr", -1, 0) >= 0;
        missing |= av_opt_set_int(context->priv_data, "unknown-reconfig-option", 0, 0) != AVERROR_OPTION_NOT_FOUND;
    }
    avcodec_free_context(&context);
    return missing;
}
