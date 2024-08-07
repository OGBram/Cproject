#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/timestamp.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>

void add_progress_bar(AVFrame *frame, int frame_number, int total_frames) {
    int bar_height = 10;
    int bar_width = (frame->width * frame_number) / total_frames;
    int y = frame->height - bar_height;

    for (int i = y; i < y + bar_height; i++) {
        for (int j = 0; j < bar_width; j++) {
            frame->data[0][i * frame->linesize[0] + j * 3 + 0] = 255; // Blue
            frame->data[0][i * frame->linesize[0] + j * 3 + 1] = 0;   // Green
            frame->data[0][i * frame->linesize[0] + j * 3 + 2] = 0;   // Red
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input video> <output video>\n", argv[0]);
        exit(1);
    }

    const char *input_filename = argv[1];
    const char *output_filename = argv[2];

    av_register_all();

    AVFormatContext *input_format_context = NULL;
    if (avformat_open_input(&input_format_context, input_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open input file.\n");
        exit(1);
    }

    if (avformat_find_stream_info(input_format_context, NULL) < 0) {
        fprintf(stderr, "Could not find stream information.\n");
        exit(1);
    }

    av_dump_format(input_format_context, 0, input_filename, 0);

    int video_stream_index = -1;
    for (int i = 0; i < input_format_context->nb_streams; i++) {
        if (input_format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
        fprintf(stderr, "Could not find video stream.\n");
        exit(1);
    }

    AVCodecParameters *codecpar = input_format_context->streams[video_stream_index]->codecpar;
    AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (decoder == NULL) {
        fprintf(stderr, "Could not find decoder.\n");
        exit(1);
    }

    AVCodecContext *decoder_context = avcodec_alloc_context3(decoder);
    if (avcodec_parameters_to_context(decoder_context, codecpar) < 0) {
        fprintf(stderr, "Could not copy codec context.\n");
        exit(1);
    }

    if (avcodec_open2(decoder_context, decoder, NULL) < 0) {
        fprintf(stderr, "Could not open codec.\n");
        exit(1);
    }

    AVFormatContext *output_format_context = NULL;
    avformat_alloc_output_context2(&output_format_context, NULL, NULL, output_filename);
    if (!output_format_context) {
        fprintf(stderr, "Could not create output context.\n");
        exit(1);
    }

    AVStream *out_stream = avformat_new_stream(output_format_context, NULL);
    if (!out_stream) {
        fprintf(stderr, "Failed to allocate output stream.\n");
        exit(1);
    }

    if (avcodec_parameters_copy(out_stream->codecpar, codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters.\n");
        exit(1);
    }

    AVCodec *encoder = avcodec_find_encoder(out_stream->codecpar->codec_id);
    if (!encoder) {
        fprintf(stderr, "Necessary encoder not found.\n");
        exit(1);
    }

    AVCodecContext *encoder_context = avcodec_alloc_context3(encoder);
    if (!encoder_context) {
        fprintf(stderr, "Could not allocate video codec context.\n");
        exit(1);
    }

    if (avcodec_parameters_to_context(encoder_context, out_stream->codecpar) < 0) {
        fprintf(stderr, "Could not copy codec context.\n");
        exit(1);
    }

    encoder_context->time_base = (AVRational){1, 30};

    if (avcodec_open2(encoder_context, encoder, NULL) < 0) {
        fprintf(stderr, "Could not open encoder.\n");
        exit(1);
    }

    if (!(output_format_context->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_format_context->pb, output_filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file.\n");
            exit(1);
        }
    }

    if (avformat_write_header(output_format_context, NULL) < 0) {
        fprintf(stderr, "Error occurred when opening output file.\n");
        exit(1);
    }

    AVFrame *frame = av_frame_alloc();
    AVPacket packet;
    int frame_count = 0;

    int total_frames = input_format_context->streams[video_stream_index]->nb_frames;

    while (av_read_frame(input_format_context, &packet) >= 0) {
        if (packet.stream_index == video_stream_index) {
            if (avcodec_send_packet(decoder_context, &packet) < 0) {
                fprintf(stderr, "Error sending packet for decoding.\n");
                break;
            }

            while (avcodec_receive_frame(decoder_context, frame) >= 0) {
                add_progress_bar(frame, frame_count, total_frames);
                frame_count++;

                frame->pts = av_rescale_q(frame->pts, input_format_context->streams[video_stream_index]->time_base, encoder_context->time_base);

                if (avcodec_send_frame(encoder_context, frame) < 0) {
                    fprintf(stderr, "Error sending frame for encoding.\n");
                    break;
                }

                AVPacket out_packet;
                av_init_packet(&out_packet);
                out_packet.data = NULL;
                out_packet.size = 0;

                if (avcodec_receive_packet(encoder_context, &out_packet) == 0) {
                    out_packet.stream_index = out_stream->index;
                    av_packet_rescale_ts(&out_packet, encoder_context->time_base, out_stream->time_base);
                    av_interleaved_write_frame(output_format_context, &out_packet);
                    av_packet_unref(&out_packet);
                }
            }
        }
        av_packet_unref(&packet);
    }

    av_write_trailer(output_format_context);

    avcodec_close(decoder_context);
    avcodec_free_context(&decoder_context);
    avcodec_close(encoder_context);
    avcodec_free_context(&encoder_context);
    avformat_close_input(&input_format_context);
    avformat_free_context(output_format_context);
    av_frame_free(&frame);

    return 0;
}
