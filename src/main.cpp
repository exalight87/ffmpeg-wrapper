#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/pixdesc.h>
#ifdef __cplusplus
}
#endif

#ifdef av_err2str
#undef av_err2str
#include <string>
av_always_inline std::string av_err2string(int errnum) {
    char str[AV_ERROR_MAX_STRING_SIZE];
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#define av_err2str(err) av_err2string(err).c_str()
#endif  // av_err2str

#ifdef av_ts2str
#undef av_ts2str
#include <string>
av_always_inline std::string av_ts2string(int ts) {
    char str[AV_TS_MAX_STRING_SIZE];
    return av_ts_make_string(str, ts);
}
#define av_ts2str(ts) av_ts2string(ts).c_str()
#endif  // av_ts2str

#ifdef av_ts2timestr
#undef av_ts2timestr
#include <string>
av_always_inline std::string av_ts2timestring(int ts, AVRational* tb) {
    char str[AV_TS_MAX_STRING_SIZE];
    return av_ts_make_time_string(str, ts, tb);
}
#define av_ts2timestr(ts, tb) av_ts2timestring(ts, tb).c_str()
#endif  // av_ts2timestr

#include <format>
#include <iostream>
#include <string_view>
#include <string>
#include <memory>
#include <vector>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <span>

#define OUTPUT_FMT AV_PIX_FMT_YUV420P 
#define SCALE_FLAGS SWS_BICUBIC

// Example based on https://ffmpeg.org/doxygen/trunk/doc_2examples_2mux_8c_source.html#l00125

// a wrapper around a single output AVStream
typedef struct OutputStream {
    AVStream *st = nullptr;
    AVCodecContext *enc = nullptr;

    /* pts of the next frame that will be generated */
    int64_t next_pts = 0;
    int samples_count = 0;

    AVFrame *frame = nullptr;
    AVFrame *tmp_frame = nullptr;

    AVPacket *tmp_pkt = nullptr;

    float t = 0.f, tincr = 0.f, tincr2 = 0.f;

    struct SwsContext *sws_ctx = nullptr;
} OutputStream;

static void add_stream(OutputStream& ost, AVFormatContext& oc, const AVCodec** codec, AVCodecID codec_id, int w, int h, int framerate)
{

    AVCodecContext *c;

    /* find the encoder */
    //*codec = avcodec_find_encoder_by_name("libx264");
    //*codec = avcodec_find_encoder_by_name("libx265");
    //*codec = avcodec_find_encoder_by_name("libx264rgb");
    //*codec = avcodec_find_encoder_by_name("h264_nvenc");
    *codec = avcodec_find_encoder_by_name("h265_nvenc");
    if (!(*codec))
    {
        *codec = avcodec_find_encoder(codec_id);
    }
    uint32_t i = 0;

    while((*codec)->pix_fmts[i] != -1)
    {
        std::cout << (*codec)->pix_fmts[i] << " : " << av_get_pix_fmt_name((*codec)->pix_fmts[i++]) << '\n';
    }

    if (!(*codec)) {
        throw std::runtime_error(std::format(
                        "Could not find encoder for '{}'", 
                        avcodec_get_name(codec_id))
                    );
    }
    ost.tmp_pkt = av_packet_alloc();
    if (!ost.tmp_pkt) {
        throw std::runtime_error(
                        "Could not allocate AVPacket"
                    );
    }

    ost.st = avformat_new_stream(&oc, NULL);
    if (!ost.st) {
        throw std::runtime_error(
                        "Could not allocate stream"
                    );
    }
    ost.st->id = oc.nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        throw std::runtime_error(
                        "Could not alloc an encoding context"
                    );
    }
    ost.enc = c;

    switch ((*codec)->type) {
        /* NOT USED FOR THE MOMENT
        
        case AVMEDIA_TYPE_AUDIO:
            c->sample_fmt  = (*codec)->sample_fmts ?
                (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            c->bit_rate    = 64000;
            c->sample_rate = 44100;
            if ((*codec)->supported_samplerates) {
                c->sample_rate = (*codec)->supported_samplerates[0];
                for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                    if ((*codec)->supported_samplerates[i] == 44100)
                        c->sample_rate = 44100;
                }
            }
            av_channel_layout_copy(&c->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
            ost->st->time_base = (AVRational){ 1, c->sample_rate };
            break;
        */

        case AVMEDIA_TYPE_VIDEO:
            c->codec_id = codec_id;

            /* Resolution must be a multiple of two. */
            c->width    = w;
            c->height   = h;
            /* timebase: This is the fundamental unit of time (in seconds) in terms
            * of which frame timestamps are represented. For fixed-fps content,
            * timebase should be 1/framerate and timestamp increments should be
            * identical to 1. */
            ost.st->time_base = { 1, framerate };
            c->time_base       = ost.st->time_base;

            c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
            c->pix_fmt       = OUTPUT_FMT;

            //av_opt_set(c, "crf", "23", 0);
            av_opt_set(c, "preset", "fast", 0);

            break;

        default:
            break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc.oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
};

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;
 
    picture = av_frame_alloc();
    if (!picture)
        return NULL;
 
    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;
 
    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 0);
    if (ret < 0) {
        throw std::runtime_error(
                        "Could not allocate frame data."
                    );
    }
 
    return picture;
}

static void open_video(AVFormatContext *oc, const AVCodec *codec,
                    OutputStream *ost, AVDictionary *opt_arg)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        throw std::runtime_error(std::format(
                        "Could not open video codec: {}", 
                        av_err2str(ret))
                    );
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        throw std::runtime_error(
                        "Could not allocate video frame"
                    );
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
    * picture is needed too. It is then converted to the required
    * output format. */
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
        if (!ost->tmp_frame) {
            throw std::runtime_error(
                        "Could not allocate temporary picture"
                    );
        }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        throw std::runtime_error(
                        "Could not copy the stream parameters"
                    );
    }
}

 static void close_stream(AVFormatContext *oc, OutputStream *ost)
 {
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    av_packet_free(&ost->tmp_pkt);
    sws_freeContext(ost->sws_ctx);
 }

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
 
    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

namespace ffmpeg
{
    auto _format_context_deleter = [](AVFormatContext* c) -> void { avformat_free_context(c);}; 

    class Video
    {
        std::string _filename;
        const AVCodec* _codec; // not owning ptr that's why it is raw ptr
        std::unique_ptr<AVFormatContext, decltype(_format_context_deleter)> _format_context;
        OutputStream _stream;

    public:
        Video() = delete;
        Video(std::string&& fname, int w, int h, int fps) : _filename(fname)
        {

            AVFormatContext *oc;

            avformat_alloc_output_context2(&oc, NULL, NULL, _filename.c_str());
            if (!oc) {
                avformat_alloc_output_context2(&oc, NULL, "mp4", _filename.c_str());
            }

            std::unique_ptr<AVFormatContext, decltype(_format_context_deleter)> tmp_codec(oc, _format_context_deleter);
            _format_context = std::move(tmp_codec);

            if(_format_context == nullptr)
            {
                throw std::runtime_error(std::format(
                        "Fail to initialize format context for {}", 
                        _filename)
                    );
            }

            //AV_CODEC_ID_H265;
            //format->video_codec

            auto format = _format_context->oformat;
            add_stream(_stream, *_format_context, &_codec, AVCodecID::AV_CODEC_ID_H265, w, h, fps);

            AVDictionary *opt = NULL;
            open_video(_format_context.get(), _codec, &_stream, opt);

            av_dump_format(oc, 0, _filename.c_str(), 1);
 
            /* open the output file, if needed */
            if (!(format->flags & AVFMT_NOFILE)) {
                auto ret = avio_open(&oc->pb, _filename.c_str(), AVIO_FLAG_WRITE);
                if (ret < 0) {
                    throw std::runtime_error(std::format(
                        "Could not open '{}': {}",
                        _filename,
                        av_err2str(ret))
                    );
                }
            }

            auto ret = avformat_write_header(oc, &opt);
            if (ret < 0) {
                throw std::runtime_error(std::format(
                        "Error occurred when opening output file: {}", 
                        av_err2str(ret))
                    );
            }
        }

        ~Video() 
        {
            av_write_trailer(_format_context.get());
 
            /* Close each codec. */
            close_stream(_format_context.get(), &_stream);
        
            if (!(_format_context->oformat->flags & AVFMT_NOFILE))
                /* Close the output file. */
                avio_closep(&_format_context->pb);
        }

        void AddFrame(std::span<uint8_t> data, AVPixelFormat fmt, int w, int h)
        {
            AVCodecContext *c = _stream.enc;

            if (av_frame_make_writable(_stream.frame) < 0)
                exit(1);

            if (fmt != AV_PIX_FMT_YUV420P) {
                /* as we only generate a RGB24 picture, we must convert it
                * to the codec pixel format if needed */
                if (!_stream.sws_ctx) {
                    _stream.sws_ctx = sws_getContext(w, h,
                                                fmt,
                                                c->width, c->height,
                                                c->pix_fmt,
                                                SCALE_FLAGS, NULL, NULL, NULL);
                    if (!_stream.sws_ctx) {
                        throw std::runtime_error(
                                "Could not initialize the conversion context"
                            );
                    }
                }

                if(!_stream.tmp_frame)
                {
                    _stream.tmp_frame = alloc_picture(fmt, w, h);
                    if (!_stream.tmp_frame) {
                        throw std::runtime_error(
                                    "Could not allocate temporary picture"
                                );
                    }
                }

                std::memcpy(_stream.tmp_frame->data[0], data.data(), data.size());

                sws_scale(_stream.sws_ctx, (const uint8_t * const *) _stream.tmp_frame->data,
                        _stream.tmp_frame->linesize, 0, h, _stream.frame->data,
                        _stream.frame->linesize);
            } else {
                std::memcpy(_stream.frame->data[0], data.data(), data.size());
            }

            _stream.frame->pts = _stream.next_pts;
            _stream.next_pts+=1;

            int ret;
  
            // send the frame to the encoder
            ret = avcodec_send_frame(c, _stream.frame);
            if (ret < 0) {
                throw std::runtime_error(std::format(
                        "Error sending a frame to the encoder: {}", 
                        av_err2str(ret))
                    );
            }
        
            while (ret >= 0) {
                ret = avcodec_receive_packet(c, _stream.tmp_pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0) {
                    throw std::runtime_error(std::format(
                        "Error encoding a frame: {}", 
                        av_err2str(ret))
                    );
                }
        
                /* rescale output packet timestamp values from codec to stream timebase */
                av_packet_rescale_ts(_stream.tmp_pkt, c->time_base, _stream.st->time_base);
                _stream.tmp_pkt->stream_index = _stream.st->index;
        
                /* Write the compressed frame to the media file. */
                //log_packet(_format_context.get(), _stream.tmp_pkt);
                ret = av_interleaved_write_frame(_format_context.get(), _stream.tmp_pkt);
                /* pkt is now blank (av_interleaved_write_frame() takes ownership of
                * its contents and resets pkt), so that no unreferencing is necessary.
                * This would be different if one used av_write_frame(). */
                if (ret < 0) {
                    throw std::runtime_error(std::format(
                        "Error while writing output packet: {}", 
                        av_err2str(ret))
                    );
                }
            }
        }
    };
}

int main(int argc, char const *argv[])
{
    int w = 4608;
    int h = 3072;
    ffmpeg::Video video("test.mp4", w, h, 24); // Create video with width, height and fps wanted
                                               // Codec and format are computed from the filename
    
    std::filesystem::path picturesFolder{ R"(E:\vacances\24h_du_Mans\resultats)" };

    std::vector<uint8_t> data(w*h*3, 255ui8);

    for  ( const auto& entry : std::filesystem::directory_iterator(picturesFolder) )
    {
        //int width, height, channels;
        //auto pixelDeleter = [](uint8_t* p) { stbi_image_free(p); };
        //std::unique_ptr<uint8_t, decltype(pixelDeleter)> pixels(
        //    stbi_load(entry.path().string().c_str(), &width, &height, &channels, 0),
        //    pixelDeleter
        //);
//
        //std::span<uint8_t> data{pixels.get(), pixels.get() + size_t(width * height * channels)}; 



        video.AddFrame(data, AV_PIX_FMT_RGB24, w, h); // Add frames contained in container in memory
                                                               // You can provide every pixel format, width and height
    }
    
    // The video will be automaticaly ended at the end of the Video instance

    return 0;
}


