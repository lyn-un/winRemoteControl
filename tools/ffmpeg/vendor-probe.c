/* Build-capability probe. Enumeration is not a hardware availability test. */
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

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
    return missing;
}
