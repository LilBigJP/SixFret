/* libSceVideodec2: H.264 access units decoded by FFmpeg into the game's NV12 frame buffers.
 * Guitar Hero Live plays its movies and the live-action footage behind the highway through it.
 * Structures, error codes and the memory estimate follow shadPS4's videodec2
 * (GPL-2.0-or-later): the picture info of a frame is kept behind its pixels in the game's
 * frame buffer, where sceVideodec2GetPictureInfo reads it back. A frame buffer too small for
 * the picture is an error code instead of an overflow. */
#include "runtime.h"
#include "gpu/mtgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#define ERR_API_FAIL ((int32_t)0x811D0100)
#define ERR_STRUCT_SIZE ((int32_t)0x811D0101)
#define ERR_ARGUMENT_POINTER ((int32_t)0x811D0102)
#define ERR_DECODER_INSTANCE ((int32_t)0x811D0103)
#define ERR_MEMORY_POINTER ((int32_t)0x811D0105)
#define ERR_FRAME_BUFFER_SIZE ((int32_t)0x811D0106)
#define ERR_FRAME_BUFFER_POINTER ((int32_t)0x811D0107)
#define ERR_ACCESS_UNIT_SIZE ((int32_t)0x811D010D)
#define ERR_ACCESS_UNIT_POINTER ((int32_t)0x811D010E)
#define ERR_CONFIG_INFO ((int32_t)0x811D0200)
#define ERR_COMPUTE_PIPE_ID ((int32_t)0x811D0201)
#define ERR_COMPUTE_QUEUE_ID ((int32_t)0x811D0202)
#define ERR_CODEC_TYPE ((int32_t)0x811D0204)
#define ERR_PIPELINE_DEPTH ((int32_t)0x811D0206)
#define ERR_THREAD_PRIORITY ((int32_t)0x811D0208)

#define CODEC_AVC 1u
#define MINIMUM_MEMORY (UINT64_C(16)<<20)
#define QUEUED 16 /* decoded pictures waiting for the game: more than a decoder's reorder depth */

typedef struct {
    uint64_t this_size;
    uint32_t resource_type, codec_type, profile, max_level;
    int32_t max_frame_width, max_frame_height, max_dpb_frame_count;
    uint32_t decode_pipeline_depth;
    void *compute_queue;
    uint64_t cpu_affinity_mask;
    int32_t cpu_thread_priority;
    uint8_t optimize_progressive_video, check_memory_type, reserved0, reserved1;
    void *extra_config_info;
} ConfigInfo;
typedef struct {
    uint64_t this_size, cpu_memory_size;
    void *cpu_memory;
    uint64_t gpu_memory_size;
    void *gpu_memory;
    uint64_t cpu_gpu_memory_size;
    void *cpu_gpu_memory;
    uint64_t max_frame_buffer_size;
    uint32_t frame_buffer_alignment, reserved0;
} MemoryInfo;
typedef struct { uint64_t this_size; void *au_data; uint64_t au_size, pts_data, dts_data, attached_data; } InputData;
typedef struct {
    uint64_t this_size;
    uint8_t is_valid, is_error_frame, picture_count;
    uint32_t codec_type, frame_width, frame_pitch, frame_height;
    void *frame_buffer;
    uint64_t frame_buffer_size;
    uint32_t frame_format, frame_pitch_in_bytes; /* absent before SDK 4: this_size 0x30 */
} OutputInfo;
typedef struct { uint64_t this_size; void *frame_buffer; uint64_t frame_buffer_size; uint8_t is_accepted; } FrameBuffer;
typedef struct { uint64_t this_size, cpu_gpu_memory_size; void *cpu_gpu_memory; } ComputeMemoryInfo;
typedef struct { uint64_t this_size; uint16_t compute_pipe_id, compute_queue_id; uint8_t check_memory_type, reserved0; uint16_t reserved1; } ComputeConfigInfo;
typedef struct {
    uint64_t this_size;
    uint8_t is_valid;
    uint64_t pts_data, dts_data, attached_data;
    uint8_t idr_pictureflag, profile_idc, level_idc;
    uint32_t pic_width_in_mbs_minus1, pic_height_in_map_units_minus1;
    uint8_t frame_mbs_only_flag, frame_cropping_flag;
    uint32_t frame_crop_left_offset, frame_crop_right_offset, frame_crop_top_offset, frame_crop_bottom_offset;
    uint8_t aspect_ratio_info_present_flag, aspect_ratio_idc;
    uint16_t sar_width, sar_height;
    uint8_t video_signal_type_present_flag, video_format, video_full_range_flag, colour_description_present_flag,
            colour_primaries, transfer_characteristics, matrix_coefficients, timing_info_present_flag;
    uint32_t num_units_in_tick, time_scale;
    uint8_t fixed_frame_rate_flag, bitstream_restriction_flag, max_dec_frame_buffering, pic_struct_present_flag,
            pic_struct, field_pic_flag, bottom_field_flag, sequence_parameter_set_present_flag,
            picture_parameter_set_present_flag, au_delimiter_present_flag, end_of_sequence_present_flag,
            end_of_stream_present_flag, filler_data_present_flag, picture_timing_sei_present_flag,
            buffering_period_sei_present_flag, constraint_set0_flag, constraint_set1_flag, constraint_set2_flag,
            constraint_set3_flag, constraint_set4_flag, constraint_set5_flag;
} AvcPictureInfo;
_Static_assert(sizeof(ConfigInfo)==0x48 && sizeof(MemoryInfo)==0x48 && sizeof(InputData)==0x30 &&
               sizeof(OutputInfo)==0x38 && sizeof(FrameBuffer)==0x20 && sizeof(ComputeMemoryInfo)==0x18 &&
               sizeof(ComputeConfigInfo)==0x10 && sizeof(AvcPictureInfo)==0x78,"Videodec2 guest layouts");

typedef struct {
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *queue[QUEUED], *nv12;
    unsigned head, count;
    struct SwsContext *sws;
    uint64_t pictures;
} Decoder;

static uint32_t align(uint32_t value, uint32_t to) { return (value+to-1)&~(to-1); }
static int trace(void) {
    static int on=-1;
    if (on<0) { const char *t=getenv("SF_VIDEODEC_TRACE"); on=t && *t && *t!='0'; }
    return on;
}

static ABI int32_t compute_memory(ComputeMemoryInfo *info) {
    if (!info) return ERR_ARGUMENT_POINTER;
    if (info->this_size!=sizeof(*info)) return ERR_STRUCT_SIZE;
    info->cpu_gpu_memory=NULL; info->cpu_gpu_memory_size=MINIMUM_MEMORY;
    return 0;
}
/* The PS4 decodes on a GPU compute queue; FFmpeg decodes on the CPU, so the queue is only a token. */
static ABI int32_t compute_queue_allocate(const ComputeConfigInfo *config, const ComputeMemoryInfo *memory, void **queue) {
    if (!config || !memory || !queue) return ERR_ARGUMENT_POINTER;
    if (config->this_size!=sizeof(*config) || memory->this_size!=sizeof(*memory)) return ERR_STRUCT_SIZE;
    if (config->reserved0 || config->reserved1) return ERR_CONFIG_INFO;
    if (config->compute_pipe_id>4) return ERR_COMPUTE_PIPE_ID;
    if (config->compute_queue_id>7) return ERR_COMPUTE_QUEUE_ID;
    if (!memory->cpu_gpu_memory) return ERR_MEMORY_POINTER;
    *queue=memory->cpu_gpu_memory;
    return 0;
}
static ABI int32_t compute_queue_release(void *queue) { (void)queue; return 0; }

static int32_t check_config(const ConfigInfo *config, const MemoryInfo *memory) {
    if (!config || !memory) return ERR_ARGUMENT_POINTER;
    if (config->this_size!=sizeof(*config) || memory->this_size!=sizeof(*memory)) return ERR_STRUCT_SIZE;
    if (config->codec_type!=CODEC_AVC) return ERR_CODEC_TYPE; /* HEVC is a PS4 Pro feature no SDK 2.5 game uses */
    return 0;
}
static ABI int32_t decoder_memory(const ConfigInfo *config, MemoryInfo *out) {
    int32_t r=check_config(config,out);
    if (r) return r;
    if (!config->decode_pipeline_depth || config->decode_pipeline_depth>8) return ERR_PIPELINE_DEPTH;
    if (config->cpu_thread_priority!=-1 && (uint32_t)(config->cpu_thread_priority-0x100)>=0x200) return ERR_THREAD_PRIORITY;
    int32_t width=config->max_frame_width, height=config->max_frame_height;
    if (width<=0 || height<=0) { width=config->max_level>=150 ? 3840 : 1920; height=config->max_level>=150 ? 2160 : 1080; }
    uint64_t frame=(uint64_t)align((uint32_t)width,64)*align((uint32_t)height,16)*3/2;
    out->cpu_memory=out->gpu_memory=out->cpu_gpu_memory=NULL;
    out->cpu_memory_size=out->gpu_memory_size=out->cpu_gpu_memory_size=MINIMUM_MEMORY;
    out->max_frame_buffer_size=((frame+255)&~UINT64_C(255))+0x4000; /* the pixels, then the picture info */
    out->frame_buffer_alignment=0x100;
    return 0;
}
static void destroy(Decoder *d) {
    for (unsigned i=0;i<QUEUED;++i) av_frame_free(&d->queue[i]);
    avcodec_free_context(&d->codec); av_packet_free(&d->packet); av_frame_free(&d->nv12);
    sws_freeContext(d->sws);
    free(d);
}
static ABI int32_t decoder_create(const ConfigInfo *config, const MemoryInfo *memory, Decoder **out) {
    int32_t r=check_config(config,memory);
    if (r) return r;
    if (!out) return ERR_ARGUMENT_POINTER;
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    Decoder *d=calloc(1,sizeof(*d));
    if (!codec || !d) { free(d); return ERR_API_FAIL; }
    d->codec=avcodec_alloc_context3(codec);
    d->packet=av_packet_alloc();
    d->nv12=av_frame_alloc();
    int frames=d->nv12!=NULL;
    for (unsigned i=0;i<QUEUED;++i) frames&=(d->queue[i]=av_frame_alloc())!=NULL;
    if (d->codec) {
        d->codec->flags|=AV_CODEC_FLAG_COPY_OPAQUE; /* attached data follows its picture */
        /* 1080p footage beside the game's own threads: frame threads, a few of them. */
        d->codec->thread_count=4; d->codec->thread_type=FF_THREAD_FRAME;
    }
    if (!d->codec || !d->packet || !frames || avcodec_open2(d->codec,codec,NULL)<0) { destroy(d); return ERR_API_FAIL; }
    *out=d;
    printf("Videodec2: H.264 decoder created (up to %dx%d, pipeline depth %u)\n",
           config->max_frame_width,config->max_frame_height,config->decode_pipeline_depth);
    return 0;
}
static ABI int32_t decoder_delete(Decoder *d) {
    if (!d) return ERR_DECODER_INSTANCE;
    if (trace()) printf("Videodec2: decoder deleted after %llu pictures\n",(unsigned long long)d->pictures);
    destroy(d);
    return 0;
}

/* Every picture the decoder has ready moves to the queue (it holds them until they are taken). */
static void drain(Decoder *d) {
    while (d->count<QUEUED) {
        AVFrame *f=d->queue[(d->head+d->count)%QUEUED];
        av_frame_unref(f);
        if (avcodec_receive_frame(d->codec,f)) return;
        ++d->count;
    }
}
/* The oldest queued picture into the game's buffer: luma, then interleaved chroma, both with a
 * pitch of the width aligned to 64 and the height aligned to 16 (the last rows repeated); the
 * picture info follows the pixels. */
static int32_t output(Decoder *d, FrameBuffer *buffer, OutputInfo *info) {
    buffer->is_accepted=0; info->is_valid=0; info->picture_count=0;
    if (!d->count) return 0;
    if (!buffer->frame_buffer) return ERR_FRAME_BUFFER_POINTER;
    AVFrame *source=d->queue[d->head], *f=source;
    if (f->format!=AV_PIX_FMT_NV12) {
        av_frame_unref(d->nv12);
        d->nv12->format=AV_PIX_FMT_NV12; d->nv12->width=f->width; d->nv12->height=f->height;
        d->sws=sws_getCachedContext(d->sws,f->width,f->height,(enum AVPixelFormat)f->format,
                                    f->width,f->height,AV_PIX_FMT_NV12,SWS_FAST_BILINEAR,NULL,NULL,NULL);
        if (!d->sws || av_frame_get_buffer(d->nv12,0)<0 ||
            sws_scale(d->sws,(const uint8_t *const *)f->data,f->linesize,0,f->height,d->nv12->data,d->nv12->linesize)<0)
            return ERR_API_FAIL;
        f=d->nv12;
    }
    uint32_t width=(uint32_t)f->width, height=(uint32_t)f->height;
    uint32_t pitch=align(width,64), rows=align(height,16);
    uint64_t pixels=(uint64_t)pitch*rows*3/2;
    if (pixels+sizeof(AvcPictureInfo)>buffer->frame_buffer_size) return ERR_FRAME_BUFFER_SIZE;
    unsigned char *luma=buffer->frame_buffer, *chroma=luma+(size_t)pitch*rows;
    for (uint32_t y=0;y<rows;++y) {
        uint32_t from=y<height ? y : height-1;
        memcpy(luma+(size_t)y*pitch,f->data[0]+(size_t)from*(size_t)f->linesize[0],width);
        if (y<rows/2) {
            uint32_t c=y<height/2 ? y : height/2-1;
            memcpy(chroma+(size_t)y*pitch,f->data[1]+(size_t)c*(size_t)f->linesize[1],width);
        }
    }
    buffer->is_accepted=1;
    info->is_valid=1; info->is_error_frame=0; info->picture_count=1;
    info->codec_type=CODEC_AVC;
    info->frame_width=align(width,16); info->frame_pitch=pitch; info->frame_height=rows;
    info->frame_buffer=buffer->frame_buffer; info->frame_buffer_size=pixels;
    if (info->this_size==sizeof(*info)) { info->frame_format=0; info->frame_pitch_in_bytes=pitch; }
    AvcPictureInfo picture={.this_size=sizeof(picture),.is_valid=1};
    picture.pts_data=(uint64_t)source->pts;
    picture.dts_data=source->pkt_dts<0 ? 0 : (uint64_t)source->pkt_dts;
    picture.attached_data=(uint64_t)(uintptr_t)source->opaque;
    picture.idr_pictureflag=(source->flags&AV_FRAME_FLAG_KEY)!=0;
    picture.profile_idc=(uint8_t)d->codec->profile; picture.level_idc=(uint8_t)d->codec->level;
    picture.pic_width_in_mbs_minus1=align(width,16)/16-1; picture.pic_height_in_map_units_minus1=rows/16-1;
    picture.frame_mbs_only_flag=1;
    picture.frame_cropping_flag=pitch!=width || rows!=height;
    picture.frame_crop_right_offset=pitch-width; picture.frame_crop_bottom_offset=rows-height;
    picture.colour_primaries=(uint8_t)source->color_primaries;
    picture.transfer_characteristics=(uint8_t)source->color_trc;
    picture.matrix_coefficients=(uint8_t)source->colorspace;
    picture.video_full_range_flag=source->color_range==AVCOL_RANGE_JPEG;
    if (d->codec->framerate.num>0 && d->codec->framerate.den>0) {
        picture.timing_info_present_flag=1; picture.fixed_frame_rate_flag=1;
        picture.num_units_in_tick=(uint32_t)d->codec->framerate.den; picture.time_scale=(uint32_t)d->codec->framerate.num*2;
    }
    memcpy(luma+pixels,&picture,sizeof(picture));
    if (trace() && !(d->pictures&63))
        printf("Videodec2: picture %llu, %ux%u (pitch %u), pts %llu\n",(unsigned long long)d->pictures,width,height,pitch,
               (unsigned long long)picture.pts_data);
    ++d->pictures;
    av_frame_unref(source);
    d->head=(d->head+1)%QUEUED; --d->count;
    mtgpu_movie_picture();
    return 0;
}
static int output_size_ok(const OutputInfo *info) { return info && (info->this_size|8)==sizeof(*info); }

static ABI int32_t decoder_decode(Decoder *d, const InputData *input, FrameBuffer *buffer, OutputInfo *info) {
    if (!d) return ERR_DECODER_INSTANCE;
    if (!input || !buffer || !info) return ERR_ARGUMENT_POINTER;
    if (input->this_size!=sizeof(*input) || buffer->this_size!=sizeof(*buffer) || !output_size_ok(info)) return ERR_STRUCT_SIZE;
    buffer->is_accepted=0; info->is_valid=0; info->picture_count=0;
    if (!input->au_data) return ERR_ACCESS_UNIT_POINTER;
    if (!input->au_size || input->au_size>INT32_MAX) return ERR_ACCESS_UNIT_SIZE;
    d->packet->data=input->au_data; d->packet->size=(int)input->au_size;
    d->packet->pts=(int64_t)input->pts_data; d->packet->dts=(int64_t)input->dts_data;
    d->packet->opaque=(void *)(uintptr_t)input->attached_data;
    int sent=avcodec_send_packet(d->codec,d->packet);
    if (sent==AVERROR_EOF) { avcodec_flush_buffers(d->codec); sent=avcodec_send_packet(d->codec,d->packet); } /* after a flush */
    if (sent==AVERROR(EAGAIN)) { drain(d); sent=avcodec_send_packet(d->codec,d->packet); }
    d->packet->data=NULL; d->packet->size=0;
    if (sent<0) {
        if (trace()) printf("Videodec2: access unit of %llu bytes rejected (%d)\n",(unsigned long long)input->au_size,sent);
        return ERR_API_FAIL;
    }
    drain(d);
    return output(d,buffer,info);
}
/* One buffered picture per call; the game calls it until no picture comes back. */
static ABI int32_t decoder_flush(Decoder *d, FrameBuffer *buffer, OutputInfo *info) {
    if (!d) return ERR_DECODER_INSTANCE;
    if (!buffer || !info) return ERR_ARGUMENT_POINTER;
    if (buffer->this_size!=sizeof(*buffer) || !output_size_ok(info)) return ERR_STRUCT_SIZE;
    if (!d->count) { avcodec_send_packet(d->codec,NULL); drain(d); } /* AVERROR_EOF once draining: the rest still comes out */
    return output(d,buffer,info);
}
static ABI int32_t decoder_reset(Decoder *d) {
    if (!d) return ERR_DECODER_INSTANCE;
    for (unsigned i=0;i<QUEUED;++i) av_frame_unref(d->queue[i]);
    d->head=d->count=0;
    avcodec_flush_buffers(d->codec);
    return 0;
}
static ABI int32_t picture_info(const OutputInfo *info, void *first, void *second) {
    (void)second; /* the second field of an interlaced frame: pictures are delivered as frames */
    if (!info) return ERR_ARGUMENT_POINTER;
    if (!output_size_ok(info)) return ERR_STRUCT_SIZE;
    if (!info->picture_count || !first) return 0;
    uint64_t size;
    memcpy(&size,first,sizeof(size));
    if (size<sizeof(uint64_t) || size>sizeof(AvcPictureInfo) || !info->frame_buffer) return ERR_STRUCT_SIZE;
    memcpy((unsigned char *)first+8,(const unsigned char *)info->frame_buffer+info->frame_buffer_size+8,size-8);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceVideodec2QueryComputeMemoryInfo",compute_memory}, {"sceVideodec2AllocateComputeQueue",compute_queue_allocate},
    {"sceVideodec2ReleaseComputeQueue",compute_queue_release},
    {"sceVideodec2QueryDecoderMemoryInfo",decoder_memory}, {"sceVideodec2CreateDecoder",decoder_create},
    {"sceVideodec2DeleteDecoder",decoder_delete}, {"sceVideodec2Decode",decoder_decode},
    {"sceVideodec2Flush",decoder_flush}, {"sceVideodec2Reset",decoder_reset},
    {"sceVideodec2GetPictureInfo",picture_info},
};
uintptr_t runtime_videodec2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
