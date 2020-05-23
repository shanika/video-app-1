#include "video_reader.hpp"


// av_err2str returns a temporary array. This doesn't work in gcc.
// This function can be used as a replacement for av_err2str.
static const char* av_make_error(int errnum) {
    static char str[AV_ERROR_MAX_STRING_SIZE];
    memset(str, 0, sizeof(str));
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}

bool video_reader_open(VideoReaderState* state) {

    avdevice_register_all();

    // Unpack members of state
    auto& width = state->width;
    auto& height = state->height;
    auto& time_base = state->time_base;
    auto& av_format_ctx = state->av_format_ctx;
    auto& av_codec_ctx = state->av_codec_ctx;
    auto& video_stream_index = state->video_stream_index;
    auto& av_frame = state->av_frame;
    auto& av_fltr_frame = state->av_fltr_frame;
    auto& av_packet = state->av_packet;

    auto& filter_graph = state->filter_graph;
    auto& buffersrc_ctx = state->buffersrc_ctx;
    auto& buffersink_ctx = state->buffersink_ctx;


    // Open the file using libavformat
    av_format_ctx = avformat_alloc_context();
    if (!av_format_ctx) {
        printf("Couldn't created AVFormatContext\n");
        return false;
    }

    // Get Cam device context ---------------------------------------------------------------------
    AVInputFormat* av_input_format = NULL;
    do {
        av_input_format = av_input_video_device_next(av_input_format);
    } while (av_input_format != NULL && strcmp(av_input_format->name, "avfoundation") != 0);

    AVDictionary* options = NULL;
    av_dict_set(&options, "framerate", "25", 0);
    av_dict_set(&options, "video_size", "1280x720", 0);
    av_dict_set(&options, "pix_fmt", "bgr0", 0);

    if (avformat_open_input(&av_format_ctx, "default:none", av_input_format, &options) != 0) {
        printf("Couldn't open video file\n");
        return false;
    }

    // Find the first valid video stream inside the file ------------------------------------------
    video_stream_index = -1;
    AVCodecParameters* av_codec_params;
    AVCodec* av_codec;
    for (int i = 0; i < av_format_ctx->nb_streams; ++i) {
        av_codec_params = av_format_ctx->streams[i]->codecpar;
        av_codec = avcodec_find_decoder(av_codec_params->codec_id);
        if (!av_codec) {
            continue;
        }
        if (av_codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            width = av_codec_params->width;
            height = av_codec_params->height;
            time_base = av_format_ctx->streams[i]->time_base;
            break;
        }
    }
    if (video_stream_index == -1) {
        printf("Couldn't find valid video stream inside file\n");
        return false;
    }

    // Set up a codec context for the decoder -------------------------------------
    av_codec_ctx = avcodec_alloc_context3(av_codec);
    if (!av_codec_ctx) {
        printf("Couldn't create AVCodecContext\n");
        return false;
    }
    if (avcodec_parameters_to_context(av_codec_ctx, av_codec_params) < 0) {
        printf("Couldn't initialize AVCodecContext\n");
        return false;
    }
    if (avcodec_open2(av_codec_ctx, av_codec, NULL) < 0) {
        printf("Couldn't open codec\n");
        return false;
    }

    // Now decoder is ready let's allocate memory for a frame and a packet------------------------------------

    av_frame = av_frame_alloc();
    av_fltr_frame = av_frame_alloc();

    if (!av_frame || !av_fltr_frame) {
        printf("Couldn't allocate AVFrame\n");
        return false;
    }

    av_packet = av_packet_alloc();
    if (!av_packet) {
        printf("Couldn't allocate AVPacket\n");
        return false;
    }

    int response = 0;

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();

    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_UYVY422, AV_PIX_FMT_NONE };

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        printf("Couldn't allocate FilterGraph\n");
        return false;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             av_codec_ctx->width, av_codec_ctx->height, av_codec_ctx->pix_fmt,
             time_base.num, time_base.den,
             av_codec_ctx->sample_aspect_ratio.num, av_codec_ctx->sample_aspect_ratio.den);

    if ((response = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph)) < 0) {
        printf("Can not create buffer source\n");
        return false;
    }

    if ((response = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph)) < 0) {
        printf("Cannot create buffer sink\n");
        return false;
    }

    if ((response = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0) {
        printf("Cannot set output pixel format\n");
        return false;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    const char *filter_descr = "drawbox=y=ih-68:color=black@0.4:width=iw:height=48:t=fill, drawtext=fontfile=OpenSans-Regular.ttf:text='Shanika Wijerathna | Software Developer | +642102482571 | Shanika.Wijerathna@nzta.govt.nz':fontcolor=white:fontsize=24:x=20:y=(h-53)";

    if ((response = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, NULL)) < 0) {
        printf("Cannot parse ptr\n");
        return false;
    }

    if ((response = avfilter_graph_config(filter_graph, NULL)) < 0) {
        printf("Cannot do filter graph config\n");
        return false;
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return true;
}

bool video_reader_read_frame(VideoReaderState* state, uint8_t* frame_buffer, int64_t* pts) {

    // Unpack members of state
    auto& width = state->width;
    auto& height = state->height;
    auto& av_format_ctx = state->av_format_ctx;
    auto& av_codec_ctx = state->av_codec_ctx;
    auto& video_stream_index = state->video_stream_index;
    auto& av_frame = state->av_frame;
    auto& av_fltr_frame = state->av_fltr_frame;
    auto& av_packet = state->av_packet;
    auto& sws_scaler_ctx = state->sws_scaler_ctx;

    auto& filter_graph = state->filter_graph;
    auto& buffersrc_ctx = state->buffersrc_ctx;
    auto& buffersink_ctx = state->buffersink_ctx;

    // Decode one frame
    int response;
    while (av_read_frame(av_format_ctx, av_packet) >= 0) {
        if (av_packet->stream_index != video_stream_index) {
            av_packet_unref(av_packet);
            continue;
        }

        response = avcodec_send_packet(av_codec_ctx, av_packet);
        if (response < 0) {
            printf("Failed to decode packet: %s\n", av_make_error(response));
            return false;
        }

        response = avcodec_receive_frame(av_codec_ctx, av_frame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
            av_packet_unref(av_packet);
            continue;
        } else if (response < 0) {
            printf("Failed to decode packet: %s\n", av_make_error(response));
            return false;
        }

        av_frame->pts = av_frame->best_effort_timestamp;
        av_packet_unref(av_packet);
        break;
    }

    if (av_buffersrc_add_frame_flags(buffersrc_ctx, av_frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
        printf("Error while feeding the filtergraph\n");
    }

    response = av_buffersink_get_frame(buffersink_ctx, av_fltr_frame);
    if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
        printf("Error while pulling filtered frames, end of file\n");
        return false;
    }
    if (response < 0) {
        printf("Error while pulling filtered frames\n");
        return false;
    }

    *pts = av_fltr_frame->pts;
    
    // Set up sws scaler
    if (!sws_scaler_ctx) {
        sws_scaler_ctx = sws_getContext(width, height, AV_PIX_FMT_UYVY422,
                                        width, height, AV_PIX_FMT_RGB0,
                                        SWS_BILINEAR, NULL, NULL, NULL);
    }
    if (!sws_scaler_ctx) {
        printf("Couldn't initialize sw scaler\n");
        return false;
    }

    uint8_t* dest[4] = { frame_buffer, NULL, NULL, NULL };
    int dest_linesize[4] = { width * 4, 0, 0, 0 };

    sws_scale(sws_scaler_ctx, av_fltr_frame->data, av_fltr_frame->linesize, 0, av_fltr_frame->height, dest, dest_linesize);

    return true;
}

void video_reader_close(VideoReaderState* state) {
    sws_freeContext(state->sws_scaler_ctx);
    avformat_close_input(&state->av_format_ctx);
    avformat_free_context(state->av_format_ctx);
    av_frame_free(&state->av_frame);
    av_packet_free(&state->av_packet);
    avcodec_free_context(&state->av_codec_ctx);
}
