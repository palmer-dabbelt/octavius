/* SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0 OR GPL-2.0+ */

#include <cstdio>
#include <string>
#include <vector>

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <opencv2/objdetect.hpp>
#include <opencv2/highgui.hpp>

#include <mp4v2/mp4v2.h>

static inline AVRounding operator|(AVRounding a, AVRounding b)
{
    return static_cast<AVRounding>(static_cast<int>(a) | static_cast<int>(b));
}

/*
 * Copies with a reflink.  This comes from
 * <https://gitlab.com/rubdos/pyreflink/-/blob/master/reflink/linux.c?ref_type=heads>.
 */
static void reflink_copy(std::string input_filename, std::string output_filename)
{
    unlink(output_filename.c_str());

    int old_fd = open(input_filename.c_str(), O_RDONLY);
    int new_fd = open(output_filename.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);

    if (ioctl(new_fd, FICLONE, old_fd) != 0) {
        perror("unable to ioctl");
        abort();
    }

    close(new_fd);
    close(old_fd);

    struct stat st;
    if (stat(input_filename.c_str(), &st) != 0) {
        perror("unable to state");
        abort();
    }

    if (chmod(output_filename.c_str(), st.st_mode) != 0) {
        perror("unable to chmod");
        abort();
    }
}

class detection {
public:
    detection(std::string base_timecode, std::string qr_timecode, long frame_offset)
    {
    }
};

std::string fix_timecode(std::string timecode, const std::vector<detection>& detections)
{
    return timecode + "-fixed";
}

/*
 * Detects GoPro-formatted QR codes in the input stream, appending them to the
 * given list.
 *
 * FFmpeg decoding and demuxing, which mostly comes from
 * <https://ffmpeg.org/doxygen/trunk/demux_decode_8c-example.html>.
 */
void detect(std::string filename, std::vector<detection>& detections)
{
    /* Opens up the input file for demuxing. */
    AVFormatContext *format_context = NULL;

    if (avformat_open_input(&format_context, filename.c_str(), NULL, NULL) < 0) {
        perror("Unable to open file");
        abort();
    }

    if (avformat_find_stream_info(format_context, NULL) < 0) {
        perror("Unable to find stream info");
        abort();
    }

    /* Look for the video stream.  That's all we care about for the QR code
     * detection, so just ignore the audio. */
    int video_stream_index = -1;

    video_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO,
                                             -1, -1, NULL, 0);
    if (video_stream_index < 0) {
        perror("Unable to find best stream");
        abort();
    }

    AVStream *video_stream = format_context->streams[video_stream_index];

    /* Finds a decoder for the video stream, so we can get frames. */
    const
    AVCodec *video_decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (video_decoder == NULL) {
        perror("Unable to open video decoder");
        abort();
    }

    AVCodecContext *video_decoder_context = avcodec_alloc_context3(video_decoder);
    if (video_decoder_context == NULL) {
        perror("Unable to allocate video decoder context");
        abort();
    }

    if ((avcodec_parameters_to_context(video_decoder_context,
                                       video_stream->codecpar)) < 0) {
        perror("Unable to do avcodec parameters");
        abort();
    }

    if (avcodec_open2(video_decoder_context, video_decoder, NULL) < 0) {
        perror("Unable to open");
        abort();
    }

    /* Prints out the stream information/metadata. */
    av_dump_format(format_context, 0, filename.c_str(), 0);

    std::string timecode;
    for (size_t i = 0; i < format_context->nb_streams; ++i) {
        const AVDictionaryEntry *meta_iter = NULL;
        const AVStream *stream = format_context->streams[i];
        while (meta_iter = av_dict_iterate(stream->metadata, meta_iter))
            if (strcmp(meta_iter->key, "timecode") == 0)
                timecode = meta_iter->value;
    }

    if (timecode == "") {
        perror("No timecode for video");
        abort();
    }

    /* Allocates the FFmpeg buffers for both a packet and the frame. */
    AVFrame *input_frame = av_frame_alloc();
    if (input_frame == NULL) {
        perror("Unable to allocate frame");
        abort();
    }

    AVPacket *packet = av_packet_alloc();
    if (packet == NULL) {
        perror("Unable to allocate packet");
        abort();
    }

    /* Use the FFmpeg software scaler to do a colorspace conversion to BGR24,
     * which is what OpenCV requires. */
    SwsContext *swscaler_context =
        sws_getCachedContext(NULL,
                             video_decoder_context->width,
                             video_decoder_context->height,
                             video_decoder_context->pix_fmt,
                             video_decoder_context->width,
                             video_decoder_context->height,
                             AV_PIX_FMT_BGR24,
                             SWS_BILINEAR,
                             NULL,
                             NULL,
                             NULL);

    AVFrame *bgr_frame = av_frame_alloc();
    if (bgr_frame == NULL) {
        perror("Unable to allocate BGR frame");
        abort();
    }

    if (av_image_alloc(bgr_frame->data,
                   bgr_frame->linesize,
                   video_decoder_context->width,
                   video_decoder_context->height,
                   AV_PIX_FMT_BGR24,
                   32) < 0) {
        perror("unable to allocate BGR image");
        abort();
    }

    return;

    /* The QR code detector. */
    auto qr_decoder = cv::QRCodeDetector();

    /* Reads the entire video stream, looking for QR codes. */
    long frame_count = 0;
    while (av_read_frame(format_context, packet) >= 0) {
        if (packet->stream_index != video_stream_index)
            continue;

        if (avcodec_send_packet(video_decoder_context, packet) < 0) {
            perror("Unable to send packet to decoder");
            abort();
        }

        while (avcodec_receive_frame(video_decoder_context, input_frame) >= 0) {
            sws_scale(swscaler_context,
                      input_frame->data, input_frame->linesize,
                      0, input_frame->height,
                      bgr_frame->data, bgr_frame->linesize);
            
            cv::Mat cv_frame(video_decoder_context->height,
                             video_decoder_context->width,
                             CV_8UC3,
                             bgr_frame->data[0],
                             bgr_frame->linesize[0]);

            auto qr_string = qr_decoder.detectAndDecode(cv_frame);
            if (qr_string != "")
                detections.push_back(detection(timecode, qr_string, frame_count));
        }

        av_packet_unref(packet);
    }
}

/*
 * Corrects the timestamps of each of the given files.  For now we assume
 * they're in filming order and there's no timestamp rollover.
 */
void correct(std::string input_filename, std::vector<detection>& detections)
{
    /* Opens up the input file for demuxing. */
    AVFormatContext *input_format_context = NULL;

    if (avformat_open_input(&input_format_context, input_filename.c_str(), NULL, NULL) < 0) {
        perror("Unable to open file");
        abort();
    }

    if (avformat_find_stream_info(input_format_context, NULL) < 0) {
        perror("Unable to find stream info");
        abort();
    }

    /* Finds the timecode string in the video. */
    std::string timecode;
    for (size_t i = 0; i < input_format_context->nb_streams; ++i) {
        const AVDictionaryEntry *meta_iter = NULL;
        const AVStream *stream = input_format_context->streams[i];
        while (meta_iter = av_dict_iterate(stream->metadata, meta_iter))
            if (strcmp(meta_iter->key, "timecode") == 0)
                timecode = meta_iter->value;
    }

    if (timecode == "") {
        perror("No timecode for video");
        abort();
    }

    /* Cleans up the timecode */
    auto fixed_timecode = fix_timecode(timecode, detections);

    /* Make a reflink copy and then edit that in-place. */
    auto output_filename = input_filename + "-octavious.mp4";
    reflink_copy(input_filename, output_filename);

    MP4FileHandle output_file = MP4Modify(output_filename.c_str(), 0);
    if (output_file == MP4_INVALID_FILE_HANDLE) {
        perror("unable to open output output_file");
        abort();
    }

    for (size_t i = 0; i < MP4GetNumberOfTracks(output_file); ++i) {
        auto track_id = MP4FindTrackId(output_file, i);

        uint8_t *data;
        uint32_t data_size;
        if (!MP4GetTrackVideoMetadata(output_file, track_id, &data, &data_size))
            continue;
    }

    MP4Close(output_file, 0);
}

int main(int argc, char **argv)
{
    /* The total set of detections in a file. */
    std::vector<detection> detections;

    for (int i = 1; i < argc; ++i)
        detect(argv[i], detections);

    for (int i = 1; i < argc; ++i)
        correct(argv[i], detections);

    return 0;
}
