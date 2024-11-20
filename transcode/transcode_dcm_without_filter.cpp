extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libavutil/channel_layout.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/dict.h>
}

static AVFormatContext* ifmt_ctx;
static AVFormatContext* ofmt_ctx;

typedef struct StreamContext {
    AVCodecContext* dec_ctx;
    AVCodecContext* enc_ctx;

    AVFrame* dec_frame;
} StreamContext;
static StreamContext* stream_ctx;

static int open_input_file(const char* filename)
{
    int ret;
    unsigned int i;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    stream_ctx = (StreamContext*)av_calloc(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* stream = ifmt_ctx->streams[i];

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
            || stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AVDictionaryEntry* tag = av_dict_get(stream->metadata, "rotate", nullptr, 0);
            const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
            AVCodecContext* codec_ctx;
            if (!dec) {
                av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
                return AVERROR_DECODER_NOT_FOUND;
            }

            codec_ctx = avcodec_alloc_context3(dec);
            if (!codec_ctx) {
                av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
                return AVERROR(ENOMEM);
            }
            ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                    "for stream #%u\n", i);
                return ret;
            }

            /* Inform the decoder about the timebase for the packet timestamps.
             * This is highly recommended, but not mandatory. */
            codec_ctx->pkt_timebase = stream->time_base;

            /* Reencode video & audio and remux subtitles etc. */
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                    codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
                /* Open decoder */
                ret = avcodec_open2(codec_ctx, dec, NULL);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                    return ret;
                }
            }
            stream_ctx[i].dec_ctx = codec_ctx;

            stream_ctx[i].dec_frame = av_frame_alloc();
            if (!stream_ctx[i].dec_frame)
                return AVERROR(ENOMEM);
        }
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

static int open_output_file(const char* filename)
{
    AVStream* out_stream;
    AVStream* in_stream;
    AVCodecContext* dec_ctx, * enc_ctx;
    const AVCodec* encoder;
    int ret;
    unsigned int i;

    ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }


    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* stream = ifmt_ctx->streams[i];

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
            || stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            out_stream = avformat_new_stream(ofmt_ctx, NULL);
            if (!out_stream) {
                av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
                return AVERROR_UNKNOWN;
            }

            in_stream = ifmt_ctx->streams[i];

            dec_ctx = stream_ctx[i].dec_ctx;

            if (dec_ctx && (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)) {
                /* in this example, we choose transcoding to same codec */
                encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
                if (!encoder) {
                    av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
                    return AVERROR_INVALIDDATA;
                }
                enc_ctx = avcodec_alloc_context3(encoder);
                if (!enc_ctx) {
                    av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
                    return AVERROR(ENOMEM);
                }

                /* In this example, we transcode to same properties (picture size,
                 * sample rate etc.). These properties can be changed for output
                 * streams easily using filters */
                if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                    enc_ctx->height = dec_ctx->height;
                    enc_ctx->width = dec_ctx->width;
                    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                    /* take first format from list of supported formats */
                    enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                    enc_ctx->color_range = AVCOL_RANGE_MPEG; // Set to TV range (16-235)
                    enc_ctx->colorspace = AVCOL_SPC_BT709; // Set to BT.709
                    enc_ctx->color_primaries = AVCOL_PRI_BT709; // Set color primaries to BT.709
                    enc_ctx->color_trc = AVCOL_TRC_BT709; // Set transfer characteristics to BT.709
                    enc_ctx->profile = FF_PROFILE_H264_HIGH; // Set profile to High
                    enc_ctx->level = 41;
                    /* video time_base can be set to whatever is handy and supported by encoder */
                    enc_ctx->framerate = dec_ctx->framerate;
                    enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
                }
                else {
                    enc_ctx->sample_rate = dec_ctx->sample_rate;
                    ret = av_channel_layout_copy(&enc_ctx->ch_layout, &dec_ctx->ch_layout);
                    if (ret < 0)
                        return ret;
                    /* take first format from list of supported formats */
                    enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
                    //enc_ctx->time_base = (AVRational){ 1, enc_ctx->sample_rate };
                }

                if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                    enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                /* Third parameter can be used to pass settings to encoder */
                ret = avcodec_open2(enc_ctx, encoder, NULL);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
                    return ret;
                }
                ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
                    return ret;
                }

                out_stream->time_base = enc_ctx->time_base;
                stream_ctx[i].enc_ctx = enc_ctx;

                for (int j = 0; j < in_stream->codecpar->nb_coded_side_data; j++) {
                    const AVPacketSideData* in_side_data = &in_stream->codecpar->coded_side_data[j];

                    void* data = av_malloc(in_side_data->size);
                    memcpy(data, in_side_data->data, in_side_data->size);

                    const AVPacketSideData* side_data = av_packet_side_data_add(
                        &out_stream->codecpar->coded_side_data,
                        &out_stream->codecpar->nb_coded_side_data,
                        in_side_data->type,
                        data,
                        in_side_data->size, 0);

                    if (!side_data) {
                        av_log(NULL, AV_LOG_ERROR, "Failed to copy side data parameters to output stream #%u\n", i);
                        return -1;
                    }
                }
            }
            else if (dec_ctx && (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN)) {
                av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
                return AVERROR_INVALIDDATA;
            }
            else {
                /* if this stream must be remuxed */
                ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Copying parameters for stream #%u failed\n", i);
                    return ret;
                }
                out_stream->time_base = in_stream->time_base;
            }
        }
    }
    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}

static int encode_and_write_frame(AVFrame* frame, unsigned int stream_index) {
    StreamContext* stream = &stream_ctx[stream_index];
    AVPacket* enc_pkt = av_packet_alloc();
    int ret;

    av_log(NULL, AV_LOG_INFO, "Encoding frame\n");
    /* encode filtered frame */
    av_packet_unref(enc_pkt);

    if (frame && frame->pts != AV_NOPTS_VALUE) {
        frame->pts = av_rescale_q(frame->pts, frame->time_base, stream->enc_ctx->time_base);
    }

    ret = avcodec_send_frame(stream->enc_ctx, frame);

    while (ret >= 0) {
        ret = avcodec_receive_packet(stream->enc_ctx, enc_pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;

        /* prepare packet for muxing */
        enc_pkt->stream_index = stream_index;        

        if (frame && enc_pkt->dts != AV_NOPTS_VALUE) {
            enc_pkt->dts = av_rescale_q(enc_pkt->dts, frame->time_base, stream->enc_ctx->time_base);
        }

        enc_pkt->duration = stream->enc_ctx->time_base.den / stream->enc_ctx->time_base.num;

        av_packet_rescale_ts(enc_pkt,
            stream->enc_ctx->time_base,
            ofmt_ctx->streams[stream_index]->time_base);

        /* mux encoded frame */
        ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);

        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Muxing frame\n");
        }
    }

    return ret;
}

int transcode_dcm_without_filter()
{
    int ret;
    AVPacket* packet = NULL;
    unsigned int stream_index;
    unsigned int i;

    auto input = "e:\\03_work\\transcode\\mpeg-2.dcm";
    auto output = "e:\\03_work\\transcode\\mpeg-2_without_filter.mp4";

    if ((ret = open_input_file(input)) < 0)
        goto end;
    if ((ret = open_output_file(output)) < 0)
        goto end;
    if (!(packet = av_packet_alloc()))
        goto end;

    /* read all packets */
    while (av_read_frame(ifmt_ctx, packet) >= 0) {

        stream_index = packet->stream_index;
        av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u\n",
            stream_index);

        StreamContext* stream = &stream_ctx[stream_index];

        if (stream->dec_ctx != NULL) {
            ret = avcodec_send_packet(stream->dec_ctx, packet);
            av_packet_unref(packet);

            if (ret < 0) continue;

            while (ret >= 0) {
                ret = avcodec_receive_frame(stream->dec_ctx, stream->dec_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error receiving frame from decoder: %s\n");
                    goto end;
                }

                stream->dec_frame->time_base = ifmt_ctx->streams[stream_index]->time_base;
                ret = encode_and_write_frame(stream->dec_frame, stream_index);

                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error encoding and writing frame\n");
                    //goto end;
                }
            }
        }

        av_packet_unref(packet);
    }

    /* flush decoders, filters and encoders */
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (stream_ctx[i].enc_ctx) {
            ret = encode_and_write_frame(NULL, i);
        }
    }

    av_write_trailer(ofmt_ctx);

end:
    av_packet_free(&packet);
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&stream_ctx[i].dec_ctx);
        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            avcodec_free_context(&stream_ctx[i].enc_ctx);

        av_frame_free(&stream_ctx[i].dec_frame);
    }
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmt_ctx->pb);
    }
    avformat_free_context(ofmt_ctx);

    return ret ? 1 : 0;
}