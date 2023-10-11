//{{{  description
/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
//}}}
//{{{  includes
#include "config.h"
#include "config_components.h"

#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"

#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"

#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/tx.h"

#include "libswresample/swresample.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"
#include "opt_common.h"
//}}}
//{{{  const defines
const char program_name[] = "ffplay";
const int program_birth_year = 2003;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512

/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

#define FF_QUIT_EVENT (SDL_USEREVENT + 2)
//}}}

enum { AV_SYNC_AUDIO_MASTER, AV_SYNC_VIDEO_MASTER, AV_SYNC_EXTERNAL_CLOCK };
//{{{
typedef struct PacketList {
  AVPacket* pkt;
  int serial;
  } PacketList;
//}}}
//{{{
typedef struct PacketQueue {
  AVFifo* pkt_list;

  int nb_packets;
  int size;
  int64_t duration;

  int abort_request;
  int serial;

  SDL_mutex* mutex;
  SDL_cond* cond;
  } PacketQueue;
//}}}
//{{{
typedef struct AudioParams {
  int freq;
  AVChannelLayout ch_layout;
  enum AVSampleFormat fmt;

  int frame_size;
  int bytes_per_sec;
  } AudioParams;
//}}}
//{{{
typedef struct Clock {
  double pts;           /* clock base */
  double pts_drift;     /* clock base minus time at which we updated the clock */

  double last_updated;
  double speed;

  int serial;           /* clock is based on a packet with this serial */
  int paused;
  int* queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
  } Clock;
//}}}
//{{{
typedef struct FrameData {
  int64_t pkt_pos;
  } FrameData;
//}}}
//{{{
typedef struct Frame {
  AVFrame* frame;
  AVSubtitle sub;

  int serial;
  double pts;           /* presentation timestamp for the frame */
  double duration;      /* estimated duration of the frame */
  int64_t pos;          /* byte position of the frame in the input file */

  int width;
  int height;
  int format;

  AVRational sar;
  int uploaded;
  int flip_v;
  } Frame;
//}}}
//{{{
typedef struct FrameQueue {
  Frame queue[FRAME_QUEUE_SIZE];

  int rindex;
  int windex;

  int size;
  int max_size;
  int keep_last;
  int rindex_shown;

  SDL_mutex* mutex;
  SDL_cond* cond;

  PacketQueue* pktq;
  } FrameQueue;
//}}}
//{{{
typedef struct Decoder {
  AVPacket* pkt;
  PacketQueue* queue;
  AVCodecContext* avctx;

  int pkt_serial;
  int finished;
  int packet_pending;

  SDL_cond* empty_queue_cond;

  int64_t start_pts;
  AVRational start_pts_tb;

  int64_t next_pts;
  AVRational next_pts_tb;

  SDL_Thread* decoder_tid;
  } Decoder;
//}}}
//{{{
typedef struct VideoState {
  SDL_Thread* read_tid;
  const AVInputFormat* iformat;

  int abort_request;
  int force_refresh;
  int paused;
  int last_paused;
  int queue_attachments_req;

  int seek_req;
  int seek_flags;
  int64_t seek_pos;
  int64_t seek_rel;
  int read_pause_return;

  AVFormatContext* ic;
  int realtime;

  Clock audclk;
  Clock vidclk;
  Clock extclk;

  FrameQueue pictq;
  FrameQueue subpq;
  FrameQueue sampq;

  Decoder auddec;
  Decoder viddec;
  Decoder subdec;

  int audio_stream;
  int av_sync_type;

  double audio_clock;
  int audio_clock_serial;
  double audio_diff_cum; /* used for AV difference average computation */
  double audio_diff_avg_coef;
  double audio_diff_threshold;
  int audio_diff_avg_count;
  AVStream* audio_st;
  PacketQueue audioq;
  int audio_hw_buf_size;
  uint8_t* audio_buf;
  uint8_t* audio_buf1;
  unsigned int audio_buf_size; /* in bytes */
  unsigned int audio_buf1_size;
  int audio_buf_index; /* in bytes */
  int audio_write_buf_size;

  int audio_volume;
  int muted;

  struct AudioParams audio_src;
  struct AudioParams audio_filter_src;
  struct AudioParams  audio_tgt;
  struct SwrContext* swr_ctx;
  int frame_drops_early;
  int frame_drops_late;

  enum ShowMode {
    SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB } show_mode;

  int16_t sample_array[SAMPLE_ARRAY_SIZE];
  int sample_array_index;

  int last_i_start;

  AVTXContext* rdft;
  av_tx_fn rdft_fn;
  int rdft_bits;
  float* real_data;
  AVComplexFloat* rdft_data;

  int xpos;
  double last_vis_time;

  SDL_Texture* vis_texture;
  SDL_Texture* sub_texture;
  SDL_Texture* vid_texture;

  int subtitle_stream;
  AVStream* subtitle_st;
  PacketQueue subtitleq;

  double frame_timer;
  double frame_last_returned_time;
  double frame_last_filter_delay;

  int video_stream;
  AVStream* video_st;
  PacketQueue videoq;
  double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity

  struct SwsContext* sub_convert_ctx;
  int eof;

  char* filename;
  int width, height, xleft, ytop;
  int step;

  int vfilter_idx;
  AVFilterContext* in_video_filter;   // the first filter in the video chain
  AVFilterContext* out_video_filter;  // the last filter in the video chain
  AVFilterContext* in_audio_filter;   // the first filter in the audio chain
  AVFilterContext* out_audio_filter;  // the last filter in the audio chain
  AVFilterGraph* agraph;              // audio filter graph

  int last_video_stream, last_audio_stream, last_subtitle_stream;

  SDL_cond* continue_read_thread;
  } VideoState;
//}}}

//{{{
const struct TextureFormatEntry {
  enum AVPixelFormat format;
  int texture_fmt;
  }

const sdl_texture_format_map[] = {
  { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
  { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
  { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
  { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
  { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
  { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
  { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
  { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
  { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
  { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
  { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
  { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
  { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
  { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
  { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
  { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
  { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
  { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
  { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
  { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
  };
//}}}
//{{{  options vars
static const AVInputFormat* file_iformat;
static const char* input_filename;
static const char* window_title;

static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

static int audio_disable;
static int video_disable;
static int subtitle_disable;

static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static float seek_interval = 10;
static int display_disable;
static int borderless;
static int alwaysontop;

static int startup_volume = 100;
static int show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;

static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;

static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;

static enum ShowMode show_mode = SHOW_MODE_NONE;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
double rdftspeed = 0.02;

static int64_t cursor_last_shown;
static int cursor_hidden = 0;

static const char **vfilters_list = NULL;
static int nb_vfilters = 0;
static char* afilters = NULL;

static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;
//}}}
//{{{  context vars
static SDL_Window* window = NULL;

static SDL_Renderer* renderer = NULL;
static SDL_RendererInfo renderer_info = {0};

static SDL_AudioDeviceID audio_dev;

static int is_full_screen = 0;
static int64_t audio_callback_time = 0;
//}}}

//{{{  packetQueue
//{{{
static int packet_queue_put_private (PacketQueue* q, AVPacket* pkt) {


  if (q->abort_request)
    return -1;

  PacketList pkt1;
  pkt1.pkt = pkt;
  pkt1.serial = q->serial;

  int ret = av_fifo_write (q->pkt_list, &pkt1, 1);
  if (ret < 0)
    return ret;

  q->nb_packets++;
  q->size += pkt1.pkt->size + sizeof(pkt1);
  q->duration += pkt1.pkt->duration;

  /* XXX: should duplicate packet data in DV case */
  SDL_CondSignal (q->cond);
  return 0;
  }
//}}}
//{{{
static int packet_queue_put (PacketQueue* q, AVPacket* pkt) {

  AVPacket* pkt1 = av_packet_alloc();
  if (!pkt1) {
    av_packet_unref (pkt);
    return -1;
    }
  av_packet_move_ref (pkt1, pkt);

  SDL_LockMutex (q->mutex);
  int ret = packet_queue_put_private (q, pkt1);
  SDL_UnlockMutex (q->mutex);

  if (ret < 0)
    av_packet_free (&pkt1);

  return ret;
  }
//}}}
//{{{
static int packet_queue_put_nullpacket (PacketQueue* q, AVPacket* pkt, int stream_index) {

  pkt->stream_index = stream_index;
  return packet_queue_put (q, pkt);
  }
//}}}

//{{{
/* packet queue handling */
static int packet_queue_init (PacketQueue* q) {

  memset (q, 0, sizeof(PacketQueue));

  q->pkt_list = av_fifo_alloc2 (1, sizeof(PacketList), AV_FIFO_FLAG_AUTO_GROW);
  if (!q->pkt_list)
    return AVERROR(ENOMEM);

  q->mutex = SDL_CreateMutex();
  if (!q->mutex) {
    av_log (NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
    }

  q->cond = SDL_CreateCond();
  if (!q->cond) {
    av_log (NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
    }

  q->abort_request = 1;

  return 0;
  }
//}}}
//{{{
static void packet_queue_flush (PacketQueue* q) {

  PacketList pkt1;

  SDL_LockMutex (q->mutex);
  while (av_fifo_read (q->pkt_list, &pkt1, 1) >= 0)
    av_packet_free (&pkt1.pkt);

  q->nb_packets = 0;
  q->size = 0;
  q->duration = 0;
  q->serial++;

  SDL_UnlockMutex (q->mutex);
  }
//}}}
//{{{
static void packet_queue_destroy (PacketQueue* q) {

  packet_queue_flush (q);
  av_fifo_freep2 (&q->pkt_list);

  SDL_DestroyMutex (q->mutex);
  SDL_DestroyCond (q->cond);
  }
//}}}
//{{{
static void packet_queue_abort (PacketQueue* q) {

  SDL_LockMutex (q->mutex);

  q->abort_request = 1;
  SDL_CondSignal (q->cond);

  SDL_UnlockMutex (q->mutex);
  }
//}}}

//{{{
static void packet_queue_start (PacketQueue* q) {

  SDL_LockMutex (q->mutex);

  q->abort_request = 0;
  q->serial++;

  SDL_UnlockMutex (q->mutex);
  }
//}}}

//{{{
/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get (PacketQueue* q, AVPacket* pkt, int block, int* serial) {

  int ret = 0;

  SDL_LockMutex (q->mutex);

  for (;;) {
    if (q->abort_request) {
      ret = -1;
      break;
      }

    PacketList pkt1;
    if (av_fifo_read (q->pkt_list, &pkt1, 1) >= 0) {
      q->nb_packets--;
      q->size -= pkt1.pkt->size + sizeof(pkt1);
      q->duration -= pkt1.pkt->duration;
      av_packet_move_ref (pkt, pkt1.pkt);
      if (serial)
          *serial = pkt1.serial;
      av_packet_free (&pkt1.pkt);
      ret = 1;
      break;
      }
    else if (!block) {
      ret = 0;
      break;
      }
    else {
      SDL_CondWait (q->cond, q->mutex);
      }
    }

  SDL_UnlockMutex (q->mutex);

  return ret;
  }
//}}}
//}}}
//{{{  frameQueue
//{{{
static void frame_queue_unref_item (Frame* vp) {

  av_frame_unref (vp->frame);
  avsubtitle_free (&vp->sub);
  }
//}}}

//{{{
static int frame_queue_init (FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last) {

  memset (f, 0, sizeof(FrameQueue));

  if (!(f->mutex = SDL_CreateMutex())) {
    av_log (NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
    }

  if (!(f->cond = SDL_CreateCond())) {
    av_log (NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
    }

  f->pktq = pktq;
  f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
  f->keep_last = !!keep_last;
  for (int i = 0; i < f->max_size; i++)
    if (!(f->queue[i].frame = av_frame_alloc()))
      return AVERROR(ENOMEM);

  return 0;
  }
//}}}
//{{{
static void frame_queue_destroy (FrameQueue* f) {

  for (int i = 0; i < f->max_size; i++) {
    Frame* vp = &f->queue[i];
    frame_queue_unref_item (vp);
    av_frame_free (&vp->frame);
    }

  SDL_DestroyMutex (f->mutex);
  SDL_DestroyCond (f->cond);
  }
//}}}

//{{{
static void frame_queue_signal (FrameQueue* f) {

  SDL_LockMutex (f->mutex);
  SDL_CondSignal (f->cond);
  SDL_UnlockMutex (f->mutex);
  }
//}}}

//{{{
static Frame* frame_queue_peek (FrameQueue* f) {
  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
  }
//}}}
//{{{
static Frame* frame_queue_peek_next (FrameQueue* f) {
  return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
   }
//}}}
//{{{
static Frame* frame_queue_peek_last (FrameQueue* f) {
  return &f->queue[f->rindex];
  }
//}}}
//{{{
static Frame* frame_queue_peek_writable (FrameQueue* f) {

  /* wait until we have space to put a new frame */
  SDL_LockMutex (f->mutex);

  while (f->size >= f->max_size && !f->pktq->abort_request) {
    SDL_CondWait (f->cond, f->mutex);
    }
   SDL_UnlockMutex (f->mutex);

  if (f->pktq->abort_request)
    return NULL;

  return &f->queue[f->windex];
  }
//}}}
//{{{
static Frame* frame_queue_peek_readable (FrameQueue* f) {

  /* wait until we have a readable a new frame */
  SDL_LockMutex (f->mutex);
  while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request)
    SDL_CondWait (f->cond, f->mutex);
   SDL_UnlockMutex (f->mutex);

  if (f->pktq->abort_request)
    return NULL;

  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
  }
//}}}

//{{{
static void frame_queue_push (FrameQueue* f) {

  if (++f->windex == f->max_size)
    f->windex = 0;

  SDL_LockMutex (f->mutex);
  f->size++;
  SDL_CondSignal (f->cond);
  SDL_UnlockMutex (f->mutex);
  }
//}}}
//{{{
static void frame_queue_next (FrameQueue* f) {

  if (f->keep_last && !f->rindex_shown) {
    f->rindex_shown = 1;
    return;
    }

  frame_queue_unref_item(&f->queue[f->rindex]);
  if (++f->rindex == f->max_size)
    f->rindex = 0;

  SDL_LockMutex (f->mutex);
  f->size--;
  SDL_CondSignal (f->cond);
  SDL_UnlockMutex (f->mutex);
  }
//}}}

//{{{
/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining (FrameQueue* f) {
  return f->size - f->rindex_shown;
  }
//}}}
//{{{
/* return last shown position */
static int64_t frame_queue_last_pos (FrameQueue* f) {

  Frame* fp = &f->queue[f->rindex];
  if (f->rindex_shown && fp->serial == f->pktq->serial)
    return fp->pos;
  else
    return -1;
  }
//}}}
//}}}
//{{{  clock
//{{{
static double get_clock (Clock* c) {

  if (*c->queue_serial != c->serial)
    return NAN;

  if (c->paused)
    return c->pts;
  else {
    double time = av_gettime_relative() / 1000000.0;
    return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
  }
//}}}
//{{{
static void set_clock_at (Clock* c, double pts, int serial, double time) {

  c->pts = pts;
  c->last_updated = time;
  c->pts_drift = c->pts - time;
  c->serial = serial;
  }
//}}}
//{{{
static void set_clock (Clock* c, double pts, int serial) {

  double time = av_gettime_relative() / 1000000.0;
  set_clock_at (c, pts, serial, time);
  }
//}}}
//{{{
static void set_clock_speed (Clock* c, double speed) {

  set_clock (c, get_clock(c), c->serial);
  c->speed = speed;
  }
//}}}
//{{{
static void init_clock (Clock* c, int *queue_serial) {

  c->speed = 1.0;
  c->paused = 0;
  c->queue_serial = queue_serial;
  set_clock (c, NAN, -1);
  }
//}}}
//{{{
static void sync_clock_to_slave (Clock* c, Clock* slave) {

  double clock = get_clock (c);
  double slave_clock = get_clock (slave);

  if (!isnan (slave_clock) && (isnan (clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
    set_clock (c, slave_clock, slave->serial);
  }
//}}}

//{{{
static int get_master_sync_type (VideoState* videoState) {

  if (videoState->av_sync_type == AV_SYNC_VIDEO_MASTER) {
    if (videoState->video_st)
      return AV_SYNC_VIDEO_MASTER;
    else
     return AV_SYNC_AUDIO_MASTER;
    }
  else if (videoState->av_sync_type == AV_SYNC_AUDIO_MASTER) {
    if (videoState->audio_st)
      return AV_SYNC_AUDIO_MASTER;
    else
      return AV_SYNC_EXTERNAL_CLOCK;
    }
  else
    return AV_SYNC_EXTERNAL_CLOCK;
  }
//}}}
//{{{
/* get the current master clock value */
static double get_master_clock (VideoState* videoState) {

  double val;

  switch (get_master_sync_type (videoState)) {
    case AV_SYNC_VIDEO_MASTER:
      val = get_clock (&videoState->vidclk);
      break;

    case AV_SYNC_AUDIO_MASTER:
      val = get_clock (&videoState->audclk);
      break;

    default:
      val = get_clock (&videoState->extclk);
      break;
    }

  return val;
  }
//}}}
//{{{
static void check_external_clock_speed (VideoState* videoState) {

   if (videoState->video_stream >= 0 && videoState->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
       videoState->audio_stream >= 0 && videoState->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)
     set_clock_speed(&videoState->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, videoState->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));

   else if ((videoState->video_stream < 0 || videoState->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
            (videoState->audio_stream < 0 || videoState->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES))
     set_clock_speed (&videoState->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, videoState->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));

   else {
     double speed = videoState->extclk.speed;
     if (speed != 1.0)
       set_clock_speed (&videoState->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
     }
  }
//}}}
//}}}
//{{{  audio
//{{{
static int cmp_audio_fmts (enum AVSampleFormat fmt1, int64_t channel_count1,
                                  enum AVSampleFormat fmt2, int64_t channel_count2) {
/* If channel count == 1, planar and non-planar formats are the same */

  if (channel_count1 == 1 && channel_count2 == 1)
    return av_get_packed_sample_fmt (fmt1) != av_get_packed_sample_fmt(fmt2);
  else
    return channel_count1 != channel_count2 || fmt1 != fmt2;
  }
//}}}

//{{{
static void update_sample_display (VideoState* videoState, short* samples, int samples_size) {
/* copy samples for viewing in editor window */

  int size, len;

  size = samples_size / sizeof(short);
  while (size > 0) {
    len = SAMPLE_ARRAY_SIZE - videoState->sample_array_index;
    if (len > size)
      len = size;

    memcpy (videoState->sample_array + videoState->sample_array_index, samples, len * sizeof(short));
    samples += len;
    videoState->sample_array_index += len;
    if (videoState->sample_array_index >= SAMPLE_ARRAY_SIZE)
      videoState->sample_array_index = 0;
    size -= len;
    }
  }
//}}}

//{{{
static int synchronize_audio (VideoState* videoState, int nb_samples) {
/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */

  int wanted_nb_samples = nb_samples;

  /* if not master, then we try to remove or add samples to correct the clock */
  if (get_master_sync_type (videoState) != AV_SYNC_AUDIO_MASTER) {
    double diff, avg_diff;
    int min_nb_samples, max_nb_samples;

    diff = get_clock(&videoState->audclk) - get_master_clock(videoState);

    if (!isnan (diff) && fabs (diff) < AV_NOSYNC_THRESHOLD) {
      videoState->audio_diff_cum = diff + videoState->audio_diff_avg_coef * videoState->audio_diff_cum;
      if (videoState->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
        /* not enough measures to have a correct estimate */
        videoState->audio_diff_avg_count++;
      else {
        /* estimate the A-V difference */
        avg_diff = videoState->audio_diff_cum * (1.0 - videoState->audio_diff_avg_coef);

        if (fabs(avg_diff) >= videoState->audio_diff_threshold) {
          wanted_nb_samples = nb_samples + (int)(diff * videoState->audio_src.freq);
          min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
          }

        av_log (NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                                    diff, avg_diff, wanted_nb_samples - nb_samples,
                                    videoState->audio_clock, videoState->audio_diff_threshold);
        }
      }
    else {
      /* too big difference : may be initial PTS errors, so reset A-V filter */
      videoState->audio_diff_avg_count = 0;
      videoState->audio_diff_cum = 0;
      }
    }

  return wanted_nb_samples;
  }
//}}}
//{{{
static int audio_decode_frame (VideoState* videoState) {
// Decode one audio frame and return its uncompressed size.
// The processed audio frame is decoded, converted if required, and
// stored in videoState->audio_buf, with size in bytes given by the return value.

  int resampled_data_size = 0;

  if (videoState->paused)
    return -1;

  Frame* af;
  do {
    #if defined(_WIN32)
      while (frame_queue_nb_remaining (&videoState->sampq) == 0) {
        if ((av_gettime_relative() - audio_callback_time) > 1000000LL * videoState->audio_hw_buf_size / videoState->audio_tgt.bytes_per_sec / 2)
          return -1;
        av_usleep (1000);
        }
    #endif
      if (!(af = frame_queue_peek_readable (&videoState->sampq)))
        return -1;
      frame_queue_next (&videoState->sampq);
    } while (af->serial != videoState->audioq.serial);

  int data_size = av_samples_get_buffer_size (NULL, af->frame->ch_layout.nb_channels,
                                              af->frame->nb_samples,
                                              af->frame->format, 1);

  int wanted_nb_samples = synchronize_audio (videoState, af->frame->nb_samples);

  if (af->frame->format != videoState->audio_src.fmt ||
      av_channel_layout_compare (&af->frame->ch_layout, &videoState->audio_src.ch_layout) ||
      af->frame->sample_rate != videoState->audio_src.freq ||
      (wanted_nb_samples != af->frame->nb_samples && !videoState->swr_ctx)) {
    swr_free (&videoState->swr_ctx);
    swr_alloc_set_opts2 (&videoState->swr_ctx,
                         &videoState->audio_tgt.ch_layout, videoState->audio_tgt.fmt, videoState->audio_tgt.freq,
                         &af->frame->ch_layout, af->frame->format, af->frame->sample_rate,
                         0, NULL);
    if (!videoState->swr_ctx || swr_init (videoState->swr_ctx) < 0) {
      av_log (NULL, AV_LOG_ERROR,
              "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
              af->frame->sample_rate, av_get_sample_fmt_name (af->frame->format), af->frame->ch_layout.nb_channels,
              videoState->audio_tgt.freq, av_get_sample_fmt_name (videoState->audio_tgt.fmt), videoState->audio_tgt.ch_layout.nb_channels);
        swr_free (&videoState->swr_ctx);
      return -1;
      }

    if (av_channel_layout_copy (&videoState->audio_src.ch_layout, &af->frame->ch_layout) < 0)
      return -1;
    videoState->audio_src.freq = af->frame->sample_rate;
    videoState->audio_src.fmt = af->frame->format;
    }

  if (videoState->swr_ctx) {
    const uint8_t** in = (const uint8_t **)af->frame->extended_data;
    uint8_t** out = &videoState->audio_buf1;
    int out_count = (int64_t)wanted_nb_samples * videoState->audio_tgt.freq / af->frame->sample_rate + 256;
    int out_size  = av_samples_get_buffer_size (NULL, videoState->audio_tgt.ch_layout.nb_channels, out_count, videoState->audio_tgt.fmt, 0);
    int len2;
    if (out_size < 0) {
      av_log (NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
      return -1;
      }

    if (wanted_nb_samples != af->frame->nb_samples) {
      if (swr_set_compensation (videoState->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * videoState->audio_tgt.freq / af->frame->sample_rate,
                                wanted_nb_samples * videoState->audio_tgt.freq / af->frame->sample_rate) < 0) {
         av_log (NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
         return -1;
         }
       }

    av_fast_malloc (&videoState->audio_buf1, &videoState->audio_buf1_size, out_size);
    if (!videoState->audio_buf1)
      return AVERROR(ENOMEM);

    len2 = swr_convert (videoState->swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
      av_log (NULL, AV_LOG_ERROR, "swr_convert() failed\n");
      return -1;
      }
    if (len2 == out_count) {
      av_log (NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
      if (swr_init (videoState->swr_ctx) < 0)
        swr_free (&videoState->swr_ctx);
      }

    videoState->audio_buf = videoState->audio_buf1;
    resampled_data_size = len2 * videoState->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(videoState->audio_tgt.fmt);
    }
  else {
    videoState->audio_buf = af->frame->data[0];
    resampled_data_size = data_size;
    }

  // update the audio clock with the pts
  av_unused double audio_clock0 = videoState->audio_clock;
  if (!isnan(af->pts))
    videoState->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
  else
    videoState->audio_clock = NAN;
  videoState->audio_clock_serial = af->serial;

  #ifdef DEBUG
    {
    static double last_clock;
    printf ("audio: delay:%0.3f clock:%0.3f clock0:%0.3f\n",
            videoState->audio_clock - last_clock, videoState->audio_clock, audio_clock0);
    last_clock = videoState->audio_clock;
    }
  #endif

  return resampled_data_size;
  }
//}}}
//{{{
static void sdl_audio_callback (void* opaque, Uint8* stream, int len) {
/* prepare a new audio buffer */

  VideoState* videoState = opaque;

  audio_callback_time = av_gettime_relative();

  while (len > 0) {
    if (videoState->audio_buf_index >= videoState->audio_buf_size) {
      int audio_size = audio_decode_frame (videoState);
      if (audio_size < 0) {
        // if error, just output silence
        videoState->audio_buf = NULL;
        videoState->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / videoState->audio_tgt.frame_size * videoState->audio_tgt.frame_size;
        }
      else {
        if (videoState->show_mode != SHOW_MODE_VIDEO)
         update_sample_display (videoState, (int16_t*)videoState->audio_buf, audio_size);
        videoState->audio_buf_size = audio_size;
        }
      videoState->audio_buf_index = 0;
      }

    int len1 = videoState->audio_buf_size - videoState->audio_buf_index;
    if (len1 > len)
      len1 = len;

    if (!videoState->muted && videoState->audio_buf && videoState->audio_volume == SDL_MIX_MAXVOLUME)
      memcpy (stream, (uint8_t *)videoState->audio_buf + videoState->audio_buf_index, len1);
    else {
      memset (stream, 0, len1);
      if (!videoState->muted && videoState->audio_buf)
        SDL_MixAudioFormat (stream, (uint8_t *)videoState->audio_buf + videoState->audio_buf_index, AUDIO_S16SYS, len1, videoState->audio_volume);
      }

    len -= len1;
    stream += len1;
    videoState->audio_buf_index += len1;
    }

  videoState->audio_write_buf_size = videoState->audio_buf_size - videoState->audio_buf_index;

  // Let's assume the audio driver that is used by SDL has two periods
  if (!isnan (videoState->audio_clock)) {
    set_clock_at (&videoState->audclk,
                  videoState->audio_clock - (double)(2 * videoState->audio_hw_buf_size + videoState->audio_write_buf_size) /
                                                   videoState->audio_tgt.bytes_per_sec,
                  videoState->audio_clock_serial, audio_callback_time / 1000000.0);

    sync_clock_to_slave (&videoState->extclk, &videoState->audclk);
    }
  }
//}}}

//{{{
static int audio_open (void* opaque, AVChannelLayout* wanted_channel_layout, int wanted_sample_rate,
                       struct AudioParams *audio_hw_params) {

  SDL_AudioSpec wanted_spec, spec;

  static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
  static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
  int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
  int wanted_nb_channels = wanted_channel_layout->nb_channels;

  const char* env = SDL_getenv ("SDL_AUDIO_CHANNELS");
  if (env) {
    wanted_nb_channels = atoi (env);
    av_channel_layout_uninit (wanted_channel_layout);
    av_channel_layout_default (wanted_channel_layout, wanted_nb_channels);
    }

  if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
    av_channel_layout_uninit (wanted_channel_layout);
    av_channel_layout_default (wanted_channel_layout, wanted_nb_channels);
    }

  wanted_nb_channels = wanted_channel_layout->nb_channels;
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = wanted_sample_rate;
  if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
    //{{{  error return
    av_log (NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
    return -1;
    }
    //}}}

  while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
    next_sample_rate_idx--;

  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.silence = 0;
  wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
  wanted_spec.callback = sdl_audio_callback;
  wanted_spec.userdata = opaque;
  while (!(audio_dev = SDL_OpenAudioDevice (NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
    av_log (NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
                                  wanted_spec.channels, wanted_spec.freq, SDL_GetError());
    wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
    if (!wanted_spec.channels) {
      wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
      wanted_spec.channels = wanted_nb_channels;
      if (!wanted_spec.freq) {
        //{{{  error return
        av_log (NULL, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
        return -1;
        }
        //}}}
      }
    av_channel_layout_default (wanted_channel_layout, wanted_spec.channels);
    }

  if (spec.format != AUDIO_S16SYS) {
    //{{{  error return
    av_log (NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
    return -1;
    }
    //}}}

  if (spec.channels != wanted_spec.channels) {
    av_channel_layout_uninit (wanted_channel_layout);
    av_channel_layout_default (wanted_channel_layout, spec.channels);
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
      //{{{  error return
      av_log (NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
      return -1;
      }
      //}}}
    }

  audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
  audio_hw_params->freq = spec.freq;
  if (av_channel_layout_copy (&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
    return -1;

  audio_hw_params->frame_size = av_samples_get_buffer_size (NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
  audio_hw_params->bytes_per_sec = av_samples_get_buffer_size (NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
  if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
    //{{{  error return
    av_log (NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
    return -1;
    }
    //}}}

  return spec.size;
  }
//}}}
//}}}
//{{{  video
//{{{
static void calculate_display_rect (SDL_Rect* rect,
                                    int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                    int pic_width, int pic_height, AVRational pic_sar) {

  AVRational aspect_ratio = pic_sar;
  int64_t width, height, x, y;

  if (av_cmp_q (aspect_ratio, av_make_q (0, 1)) <= 0)
    aspect_ratio = av_make_q (1, 1);

  aspect_ratio = av_mul_q (aspect_ratio, av_make_q(pic_width, pic_height));

  /* XXX: we suppose the screen has a 1.0 pixel ratio */
  height = scr_height;
  width = av_rescale (height, aspect_ratio.num, aspect_ratio.den) & ~1;
  if (width > scr_width) {
    width = scr_width;
    height = av_rescale (width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }

  x = (scr_width - width) / 2;
  y = (scr_height - height) / 2;
  rect->x = scr_xleft + x;
  rect->y = scr_ytop  + y;
  rect->w = FFMAX((int)width,  1);
  rect->h = FFMAX((int)height, 1);
  }
//}}}

//{{{
static void set_sdl_yuv_conversion_mode (AVFrame* frame) {

  SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;

  if (frame && (frame->format == AV_PIX_FMT_YUV420P
                || frame->format == AV_PIX_FMT_YUYV422
                || frame->format == AV_PIX_FMT_UYVY422)) {
    if (frame->color_range == AVCOL_RANGE_JPEG)
       mode = SDL_YUV_CONVERSION_JPEG;
    else if (frame->colorspace == AVCOL_SPC_BT709)
      mode = SDL_YUV_CONVERSION_BT709;
    else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
      mode = SDL_YUV_CONVERSION_BT601;
    }

  SDL_SetYUVConversionMode (mode); /* FIXME: no support for linear transfer */
  }
//}}}
//{{{
static void get_sdl_pix_fmt_and_blendmode (int format, Uint32* sdl_pix_fmt, SDL_BlendMode* sdl_blendmode) {

  *sdl_blendmode = SDL_BLENDMODE_NONE;
  if (format == AV_PIX_FMT_RGB32   ||
      format == AV_PIX_FMT_RGB32_1 ||
      format == AV_PIX_FMT_BGR32   ||
      format == AV_PIX_FMT_BGR32_1)
    *sdl_blendmode = SDL_BLENDMODE_BLEND;

  *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
  for (int i = 0; i < FF_ARRAY_ELEMS (sdl_texture_format_map) - 1; i++) {
    if (format == sdl_texture_format_map[i].format) {
      *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
      return;
      }
    }
  }
//}}}
//{{{
static int realloc_texture (SDL_Texture** texture, Uint32 new_format,
                            int new_width, int new_height, SDL_BlendMode blendmode, int init_texture) {

  Uint32 format;
  int access, w, h;
  if (!*texture
      || SDL_QueryTexture (*texture, &format, &access, &w, &h) < 0
      || new_width != w || new_height != h
      || new_format != format) {

    if (*texture)
      SDL_DestroyTexture (*texture);

    if (!(*texture = SDL_CreateTexture (renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
      return -1;

    if (SDL_SetTextureBlendMode (*texture, blendmode) < 0)
      return -1;

    if (init_texture) {
      void* pixels;
      int pitch;
      if (SDL_LockTexture (*texture, NULL, &pixels, &pitch) < 0)
        return -1;

      memset (pixels, 0, pitch * new_height);
      SDL_UnlockTexture (*texture);
      }

    av_log (NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }

  return 0;
  }
//}}}
//{{{
static int upload_texture (SDL_Texture** tex, AVFrame* frame) {

  Uint32 sdl_pix_fmt;
  SDL_BlendMode sdl_blendmode;
  get_sdl_pix_fmt_and_blendmode (frame->format, &sdl_pix_fmt, &sdl_blendmode);

  if (realloc_texture (tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,
                       frame->width, frame->height, sdl_blendmode, 0) < 0)
    return -1;

  int ret = 0;
  switch (sdl_pix_fmt) {
    //{{{
    case SDL_PIXELFORMAT_IYUV:
      if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0)
        ret = SDL_UpdateYUVTexture (*tex, NULL, frame->data[0], frame->linesize[0],
                                                frame->data[1], frame->linesize[1],
                                                frame->data[2], frame->linesize[2]);
      else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0)
        ret = SDL_UpdateYUVTexture (*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                                frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                                frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
      else {
        av_log (NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
        return -1;
        }
      break;
    //}}}
    //{{{
    default:
      if (frame->linesize[0] < 0)
        ret = SDL_UpdateTexture (*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
      else
        ret = SDL_UpdateTexture (*tex, NULL, frame->data[0], frame->linesize[0]);
      break;
    //}}}
    }

  return ret;
  }
//}}}

//{{{
static int compute_mod (int a, int b) {
  return a < 0 ? a%b + b : a%b;
  }
//}}}
//{{{
static int videoOpen (VideoState* videoState) {

  int w = screen_width ? screen_width : default_width;
  int h = screen_height ? screen_height : default_height;

  if (!window_title)
    window_title = input_filename;
  SDL_SetWindowTitle (window, window_title);

  SDL_SetWindowSize (window, w, h);
  SDL_SetWindowPosition (window, screen_left, screen_top);
  if (is_full_screen)
    SDL_SetWindowFullscreen (window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow (window);

  videoState->width  = w;
  videoState->height = h;

  return 0;
  }
//}}}

//{{{
static void drawRectangle (int x, int y, int w, int h) {

  SDL_Rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = w;
  rect.h = h;

  if (w && h)
    SDL_RenderFillRect (renderer, &rect);
  }
//}}}
//{{{
static void drawVideoAudioDisplay (VideoState* s) {

  int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
  int ch, h, h2;

  int rdft_bits;
  for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++) ;
  int nb_freq = 1 << (rdft_bits - 1);

  /* compute display index : center on currently output samples */
  int channels = s->audio_tgt.ch_layout.nb_channels;
  nb_display_channels = channels;
  if (!s->paused) {
    int data_used = s->show_mode == SHOW_MODE_WAVES ? s->width : (2*nb_freq);
    n = 2 * channels;
    delay = s->audio_write_buf_size;
    delay /= n;

    /* to be more precise, we take into account the time spent since the last buffer computation */
    int64_t time_diff = 0;
    if (audio_callback_time) {
      time_diff = av_gettime_relative() - audio_callback_time;
      delay -= (time_diff * s->audio_tgt.freq) / 1000000;
      }

    delay += 2 * data_used;
    if (delay < data_used)
      delay = data_used;

    i_start= x = compute_mod  (s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
    if (s->show_mode == SHOW_MODE_WAVES) {
      //{{{  precalc show_waves
      h = INT_MIN;
      for (i = 0; i < 1000; i += channels) {
        int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
        int a = s->sample_array[idx];
        int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
        int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
        int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
        int score = a - d;
        if (h < score && (b ^ c) < 0) {
          h = score;
          i_start = idx;
          }
        }
      }
      //}}}

    s->last_i_start = i_start;
    }
  else
    i_start = s->last_i_start;

  if (s->show_mode == SHOW_MODE_WAVES) {
    //{{{  draw waves
    SDL_SetRenderDrawColor (renderer, 255, 255, 255, 255);

    // total height for one channel
    h = s->height / nb_display_channels;

    // precalc graph height / 2
    h2 = (h * 9) / 20;
    for (ch = 0; ch < nb_display_channels; ch++) {
       i = i_start + ch;
       // position of center line
       y1 = s->ytop + ch * h + (h / 2);
       for (x = 0; x < s->width; x++) {
         y = (s->sample_array[i] * h2) >> 15;
         if (y < 0) {
           y = -y;
           ys = y1 - y;
           }
         else {
           ys = y1;
           }
         drawRectangle (s->xleft + x, ys, 1, y);
         i += channels;
         if (i >= SAMPLE_ARRAY_SIZE)
           i -= SAMPLE_ARRAY_SIZE;
         }
       }

     // draw
     SDL_SetRenderDrawColor (renderer, 0, 0, 255, 255);
     for (ch = 1; ch < nb_display_channels; ch++) {
       y = s->ytop + ch * h;
       drawRectangle (s->xleft, y, s->width, 1);
       }
     }
    //}}}
  else {
    //{{{  draw rdft
    int err = 0;
    if (realloc_texture (&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
      return;

    if (s->xpos >= s->width)
      s->xpos = 0;

    nb_display_channels = FFMIN (nb_display_channels, 2);
    if (rdft_bits != s->rdft_bits) {
      const float rdft_scale = 1.0;
      av_tx_uninit (&s->rdft);
      av_freep (&s->real_data);
      av_freep (&s->rdft_data);
      s->rdft_bits = rdft_bits;
      s->real_data = av_malloc_array (nb_freq, 4 *sizeof(*s->real_data));
      s->rdft_data = av_malloc_array (nb_freq + 1, 2 *sizeof(*s->rdft_data));
      err = av_tx_init (&s->rdft, &s->rdft_fn, AV_TX_FLOAT_RDFT, 0, 1 << rdft_bits, &rdft_scale, 0);
      }

    if (err < 0 || !s->rdft_data) {
      av_log (NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
      s->show_mode = SHOW_MODE_WAVES;
      }
    else {
      float* data_in[2];
      AVComplexFloat* data[2];
      SDL_Rect rect = {.x = s->xpos, .y = 0, .w = 1, .h = s->height};
      for (ch = 0; ch < nb_display_channels; ch++) {
        data_in[ch] = s->real_data + 2 * nb_freq * ch;
        data[ch] = s->rdft_data + nb_freq * ch;
        i = i_start + ch;
        for (x = 0; x < 2 * nb_freq; x++) {
          double w = (x-nb_freq) * (1.0 / nb_freq);
          data_in[ch][x] = s->sample_array[i] * (1.0 - w * w);
          i += channels;
          if (i >= SAMPLE_ARRAY_SIZE)
            i -= SAMPLE_ARRAY_SIZE;
          }
        s->rdft_fn (s->rdft, data[ch], data_in[ch], sizeof(float));
        data[ch][0].im = data[ch][nb_freq].re;
        data[ch][nb_freq].re = 0;
        }

      //{{{  draw rdft texture slowly, it is more than fast enough. */
      uint32_t* pixels;
      int pitch;
      if (!SDL_LockTexture (s->vis_texture, &rect, (void**)&pixels, &pitch)) {
        pitch >>= 2;
        pixels += pitch * s->height;
        for (y = 0; y < s->height; y++) {
          double w = 1 / sqrt(nb_freq);
          int a = sqrt (w * sqrt (data[0][y].re * data[0][y].re + data[0][y].im * data[0][y].im));
          int b = (nb_display_channels == 2 ) ? sqrt (w * hypot(data[1][y].re, data[1][y].im)) : a;
          a = FFMIN(a, 255);
          b = FFMIN(b, 255);
          pixels -= pitch;
          *pixels = (a << 16) + (b << 8) + ((a+b) >> 1);
          }
        SDL_UnlockTexture (s->vis_texture);
        }
      //}}}
      SDL_RenderCopy (renderer, s->vis_texture, NULL, NULL);
      }

    if (!s->paused)
      s->xpos++;
    }
    //}}}
  }
//}}}
//{{{
static void drawVideoDisplay (VideoState* videoState) {

  Frame* vp = frame_queue_peek_last (&videoState->pictq);
  Frame* sp = NULL;
  if (videoState->subtitle_st) {
    //{{{  subtitle
    if (frame_queue_nb_remaining (&videoState->subpq) > 0) {
      sp = frame_queue_peek (&videoState->subpq);
      if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
        if (!sp->uploaded) {
          uint8_t* pixels[4];
          int pitch[4];
          if (!sp->width || !sp->height) {
            sp->width = vp->width;
            sp->height = vp->height;
            }
          if (realloc_texture (&videoState->sub_texture, SDL_PIXELFORMAT_ARGB8888,
                               sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
            return;

          for (int i = 0; i < sp->sub.num_rects; i++) {
            AVSubtitleRect* sub_rect = sp->sub.rects[i];

            sub_rect->x = av_clip (sub_rect->x, 0, sp->width );
            sub_rect->y = av_clip (sub_rect->y, 0, sp->height);
            sub_rect->w = av_clip (sub_rect->w, 0, sp->width  - sub_rect->x);
            sub_rect->h = av_clip (sub_rect->h, 0, sp->height - sub_rect->y);

            videoState->sub_convert_ctx = sws_getCachedContext (videoState->sub_convert_ctx,
                                                        sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                        sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                        0, NULL, NULL, NULL);
            if (!videoState->sub_convert_ctx) {
              //{{{  error return
              av_log (NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
              return;
              }
              //}}}

            if (!SDL_LockTexture(videoState->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
              sws_scale (videoState->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                         0, sub_rect->h, pixels, pitch);
              SDL_UnlockTexture (videoState->sub_texture);
              }
            }
            sp->uploaded = 1;
          }
        }
      else
        sp = NULL;
      }
    }
    //}}}

  SDL_Rect rect;
  calculate_display_rect (&rect, videoState->xleft, videoState->ytop, videoState->width, videoState->height, vp->width, vp->height, vp->sar);
  set_sdl_yuv_conversion_mode (vp->frame);

  if (!vp->uploaded) {
    if (upload_texture (&videoState->vid_texture, vp->frame) < 0) {
      set_sdl_yuv_conversion_mode (NULL);
      return;
      }
    vp->uploaded = 1;
    vp->flip_v = vp->frame->linesize[0] < 0;
    }

  SDL_RenderCopyEx (renderer, videoState->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);
  set_sdl_yuv_conversion_mode (NULL);

  if (sp)
    SDL_RenderCopy (renderer, videoState->sub_texture, NULL, &rect);
  }
//}}}
//{{{
static void videoDisplay (VideoState* videoState) {
// display the current picture, if any

  if (!videoState->width)
    videoOpen (videoState);

  SDL_SetRenderDrawColor (renderer, 0, 0, 0, 255);
  SDL_RenderClear (renderer);

  if (videoState->audio_st && (videoState->show_mode != SHOW_MODE_VIDEO))
    drawVideoAudioDisplay (videoState);
  else if (videoState->video_st)
    drawVideoDisplay (videoState);

  SDL_RenderPresent (renderer);
  }
//}}}

//{{{
static double compute_target_delay (double delay, VideoState* videoState) {

  double sync_threshold, diff = 0;

  /* update delay to follow master synchronisation source */
  if (get_master_sync_type (videoState) != AV_SYNC_VIDEO_MASTER) {
    /* if video is slave, we try to correct big delays by duplicating or deleting a frame */
    diff = get_clock (&videoState->vidclk) - get_master_clock (videoState);

    /* skip or repeat frame. We take into account the
       delay to compute the threshold. I still don't know if it is the best guess */
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!isnan(diff) && fabs (diff) < videoState->max_frame_duration) {
      if (diff <= -sync_threshold)
        delay = FFMAX(0, delay + diff);
      else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
        delay = delay + diff;
      else if (diff >= sync_threshold)
        delay = 2 * delay;
      }
    }

  av_log (NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

  return delay;
  }
//}}}
//{{{
static double vp_duration (VideoState* videoState, Frame *vp, Frame *nextvp) {

  if (vp->serial == nextvp->serial) {
    double duration = nextvp->pts - vp->pts;
    if (isnan (duration) || duration <= 0 || duration > videoState->max_frame_duration)
      return vp->duration;
    else
      return duration;
    }
  else {
    return 0.0;
    }
  }
//}}}
//{{{
static void update_video_pts (VideoState* videoState, double pts, int serial) {

  /* update current video pts */
  set_clock (&videoState->vidclk, pts, serial);
  sync_clock_to_slave (&videoState->extclk, &videoState->vidclk);
  }
//}}}

//{{{
static void set_default_window_size (int width, int height, AVRational sar) {

  SDL_Rect rect;
  int max_width  = screen_width  ? screen_width  : INT_MAX;
  int max_height = screen_height ? screen_height : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX)
    max_height = height;

  calculate_display_rect (&rect, 0, 0, max_width, max_height, width, height, sar);

  default_width = rect.w;
  default_height = rect.h;
  }
//}}}
//{{{
static void stream_toggle_pause (VideoState* videoState) {
/* pause or resume the video */

  if (videoState->paused) {
    videoState->frame_timer += av_gettime_relative() / 1000000.0 - videoState->vidclk.last_updated;
    if (videoState->read_pause_return != AVERROR(ENOSYS))
      videoState->vidclk.paused = 0;

    set_clock (&videoState->vidclk, get_clock(&videoState->vidclk), videoState->vidclk.serial);
    }

  set_clock (&videoState->extclk, get_clock (&videoState->extclk), videoState->extclk.serial);
  videoState->paused = videoState->audclk.paused = videoState->vidclk.paused = videoState->extclk.paused = !videoState->paused;
  }
//}}}

//{{{
static void videoRefresh (void* opaque, double* remaining_time) {
// called to display each frame

  VideoState* videoState = opaque;

  if (!videoState->paused && get_master_sync_type (videoState) == AV_SYNC_EXTERNAL_CLOCK && videoState->realtime)
    check_external_clock_speed (videoState);

  double time;
  if (!display_disable && videoState->show_mode != SHOW_MODE_VIDEO && videoState->audio_st) {
    time = av_gettime_relative() / 1000000.0;
    if (videoState->force_refresh || videoState->last_vis_time + rdftspeed < time) {
      videoDisplay (videoState);
      videoState->last_vis_time = time;
      }
    *remaining_time = FFMIN(*remaining_time, videoState->last_vis_time + rdftspeed - time);
    }

  if (videoState->video_st) {
retry:
    if (frame_queue_nb_remaining (&videoState->pictq) == 0) {
      // nothing to do, no picture to display in the queue
      }
    else {
      // dequeue the picture
      Frame* lastvp = frame_queue_peek_last (&videoState->pictq);

      Frame* vp = frame_queue_peek (&videoState->pictq);
      if (vp->serial != videoState->videoq.serial) {
        frame_queue_next (&videoState->pictq);
        goto retry;
        }

      if (lastvp->serial != vp->serial)
        videoState->frame_timer = av_gettime_relative() / 1000000.0;

      if (videoState->paused)
        goto display;

      // compute nominal last_duration
      double last_duration = vp_duration (videoState, lastvp, vp);
      double delay = compute_target_delay (last_duration, videoState);

      time = av_gettime_relative() / 1000000.0;
      if (time < videoState->frame_timer + delay) {
        *remaining_time = FFMIN(videoState->frame_timer + delay - time, *remaining_time);
        goto display;
        }

      videoState->frame_timer += delay;
      if (delay > 0 && time - videoState->frame_timer > AV_SYNC_THRESHOLD_MAX)
        videoState->frame_timer = time;

      SDL_LockMutex(videoState->pictq.mutex);
      if (!isnan (vp->pts))
        update_video_pts (videoState, vp->pts, vp->serial);
      SDL_UnlockMutex (videoState->pictq.mutex);

      if (frame_queue_nb_remaining (&videoState->pictq) > 1) {
        Frame *nextvp = frame_queue_peek_next (&videoState->pictq);
        double duration = vp_duration (videoState, vp, nextvp);
        if (!videoState->step && (framedrop>0 || (framedrop && get_master_sync_type (videoState) != AV_SYNC_VIDEO_MASTER))
            && time > videoState->frame_timer + duration) {
          videoState->frame_drops_late++;
          frame_queue_next (&videoState->pictq);
          goto retry;
          }
        }

      if (videoState->subtitle_st) {
        while (frame_queue_nb_remaining (&videoState->subpq) > 0) {
          Frame* sp = frame_queue_peek (&videoState->subpq);
          Frame* sp2;
          if (frame_queue_nb_remaining (&videoState->subpq) > 1)
            sp2 = frame_queue_peek_next (&videoState->subpq);
          else
            sp2 = NULL;

          if (sp->serial != videoState->subtitleq.serial ||
              (videoState->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000))) ||
              (sp2 && videoState->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000)))) {
            if (sp->uploaded) {
              for (int i = 0; i < sp->sub.num_rects; i++) {
                AVSubtitleRect* sub_rect = sp->sub.rects[i];

                uint8_t* pixels;
                int pitch;
                if (!SDL_LockTexture (videoState->sub_texture, (SDL_Rect*)sub_rect, (void**)&pixels, &pitch)) {
                  for (int j = 0; j < sub_rect->h; j++, pixels += pitch)
                    memset (pixels, 0, sub_rect->w << 2);
                  SDL_UnlockTexture (videoState->sub_texture);
                  }
                }
              }

            frame_queue_next (&videoState->subpq);
            }
          else
            break;
          }
        }

      frame_queue_next (&videoState->pictq);
      videoState->force_refresh = 1;

      if (videoState->step && !videoState->paused)
        stream_toggle_pause (videoState);
      }

  display:
    // display picture
    if (!display_disable &&
        videoState->force_refresh &&
        videoState->show_mode == SHOW_MODE_VIDEO && videoState->pictq.rindex_shown)
      videoDisplay (videoState);
    }

  videoState->force_refresh = 0;
  if (show_status) {
    //{{{  show status
    AVBPrint buf;
    static int64_t last_time;
    int64_t cur_time;
    int aqsize, vqsize, sqsize;
    double av_diff;

    cur_time = av_gettime_relative();
    if (!last_time || (cur_time - last_time) >= 30000) {
      aqsize = 0;
      vqsize = 0;
      sqsize = 0;
      if (videoState->audio_st)
        aqsize = videoState->audioq.size;
      if (videoState->video_st)
        vqsize = videoState->videoq.size;
      if (videoState->subtitle_st)
        sqsize = videoState->subtitleq.size;
      av_diff = 0;
      if (videoState->audio_st && videoState->video_st)
        av_diff = get_clock (&videoState->audclk) - get_clock(&videoState->vidclk);
      else if (videoState->video_st)
        av_diff = get_master_clock (videoState) - get_clock(&videoState->vidclk);
      else if (videoState->audio_st)
        av_diff = get_master_clock (videoState) - get_clock(&videoState->audclk);

      av_bprint_init (&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
      av_bprintf (&buf,
                 "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                 get_master_clock (videoState),
                 (videoState->audio_st && videoState->video_st) ? "A-V" : (videoState->video_st ? "M-V" : (videoState->audio_st ? "M-A" : "   ")),
                 av_diff,
                 videoState->frame_drops_early + videoState->frame_drops_late,
                 aqsize / 1024,
                 vqsize / 1024,
                 sqsize,
                 videoState->video_st ? videoState->viddec.avctx->pts_correction_num_faulty_dts : 0,
                 videoState->video_st ? videoState->viddec.avctx->pts_correction_num_faulty_pts : 0);

      if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
        fprintf (stderr, "%s", buf.str);
      else
        av_log (NULL, AV_LOG_INFO, "%s", buf.str);

      fflush(stderr);
      av_bprint_finalize (&buf, NULL);

      last_time = cur_time;
      }
    }
    //}}}
  }
//}}}
//{{{
static int queuePicture (VideoState* videoState, AVFrame* src_frame, double pts, double duration,
                         int64_t pos, int serial) {

  #if defined(DEBUG_SYNC)
    printf ("frame_type=%c pts=%0.3f\n", av_get_picture_type_char (src_frame->pict_type), pts);
  #endif

  Frame* vp;
  if (!(vp = frame_queue_peek_writable (&videoState->pictq)))
    return -1;

  vp->sar = src_frame->sample_aspect_ratio;
  vp->uploaded = 0;

  vp->width = src_frame->width;
  vp->height = src_frame->height;
  vp->format = src_frame->format;

  vp->pts = pts;
  vp->duration = duration;
  vp->pos = pos;
  vp->serial = serial;

  set_default_window_size (vp->width, vp->height, vp->sar);

  av_frame_move_ref (vp->frame, src_frame);
  frame_queue_push (&videoState->pictq);

  return 0;
  }
//}}}
//}}}
//{{{  exit
//{{{
static void decoder_abort (Decoder* d, FrameQueue* fq) {

  packet_queue_abort (d->queue);
  frame_queue_signal (fq);
  SDL_WaitThread (d->decoder_tid, NULL);

  d->decoder_tid = NULL;
  packet_queue_flush (d->queue);
  }
//}}}
//{{{
static void decoder_destroy (Decoder* d) {

  av_packet_free (&d->pkt);
  avcodec_free_context (&d->avctx);
  }
//}}}

//{{{
static void stream_component_close (VideoState* videoState, int stream_index) {

  AVFormatContext* ic = videoState->ic;
  AVCodecParameters* codecpar;

  if (stream_index < 0 || stream_index >= ic->nb_streams)
    return;
  codecpar = ic->streams[stream_index]->codecpar;

  switch (codecpar->codec_type) {
    //{{{
    case AVMEDIA_TYPE_AUDIO:
      decoder_abort (&videoState->auddec, &videoState->sampq);

      SDL_CloseAudioDevice (audio_dev);
      decoder_destroy (&videoState->auddec);
      swr_free (&videoState->swr_ctx);
      av_freep (&videoState->audio_buf1);

      videoState->audio_buf1_size = 0;
      videoState->audio_buf = NULL;
      if (videoState->rdft) {
        av_tx_uninit (&videoState->rdft);
        av_freep (&videoState->real_data);
        av_freep (&videoState->rdft_data);
        videoState->rdft = NULL;
        videoState->rdft_bits = 0;
        }

      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_VIDEO:
      decoder_abort (&videoState->viddec, &videoState->pictq);
      decoder_destroy (&videoState->viddec);
      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_SUBTITLE:
      decoder_abort (&videoState->subdec, &videoState->subpq);
      decoder_destroy (&videoState->subdec);
      break;
    //}}}
    //{{{
    default:
      break;
    //}}}
    }

  ic->streams[stream_index]->discard = AVDISCARD_ALL;
  switch (codecpar->codec_type) {
    //{{{
    case AVMEDIA_TYPE_AUDIO:
      videoState->audio_st = NULL;
      videoState->audio_stream = -1;
      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_VIDEO:
      videoState->video_st = NULL;
      videoState->video_stream = -1;
      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_SUBTITLE:
      videoState->subtitle_st = NULL;
      videoState->subtitle_stream = -1;
      break;
    //}}}
    //{{{
    default:
      break;
    //}}}
    }
  }
//}}}
//{{{
static void stream_close (VideoState* videoState) {

  /* XXX: use a special url_shutdown call to abort parse cleanly */
  videoState->abort_request = 1;
  SDL_WaitThread (videoState->read_tid, NULL);

  /* close each stream */
  if (videoState->audio_stream >= 0)
    stream_component_close (videoState, videoState->audio_stream);
  if (videoState->video_stream >= 0)
    stream_component_close (videoState, videoState->video_stream);
  if (videoState->subtitle_stream >= 0)
    stream_component_close (videoState, videoState->subtitle_stream);

  avformat_close_input (&videoState->ic);

  packet_queue_destroy (&videoState->videoq);
  packet_queue_destroy (&videoState->audioq);
  packet_queue_destroy (&videoState->subtitleq);

  /* free all pictures */
  frame_queue_destroy (&videoState->pictq);
  frame_queue_destroy (&videoState->sampq);
  frame_queue_destroy (&videoState->subpq);

  SDL_DestroyCond (videoState->continue_read_thread);
  sws_freeContext (videoState->sub_convert_ctx);

  av_free (videoState->filename);

  if (videoState->vis_texture)
    SDL_DestroyTexture (videoState->vis_texture);
  if (videoState->vid_texture)
    SDL_DestroyTexture (videoState->vid_texture);
  if (videoState->sub_texture)
    SDL_DestroyTexture (videoState->sub_texture);
  av_free (videoState);
  }
//}}}

//{{{
static void do_exit (VideoState* videoState) {

  if (videoState)
    stream_close (videoState);

  if (renderer)
    SDL_DestroyRenderer (renderer);

  if (window)
    SDL_DestroyWindow (window);

  uninit_opts();
  av_freep (&vfilters_list);
  avformat_network_deinit();

  if (show_status)
    printf ("\n");
  SDL_Quit();

  av_log (NULL, AV_LOG_QUIET, "%s", "");
  exit (0);
  }
//}}}
//}}}

// thread
//{{{
static int configureFilterGraph (AVFilterGraph* graph, const char* filtergraph,
                                  AVFilterContext *source_ctx, AVFilterContext *sink_ctx) {

  int ret, i;
  int nb_filters = graph->nb_filters;

  AVFilterInOut* outputs = NULL;
  AVFilterInOut* inputs = NULL;
  if (filtergraph) {
    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs) {
      ret = AVERROR(ENOMEM);
      goto fail;
      }

    outputs->name       = av_strdup ("in");
    outputs->filter_ctx = source_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name        = av_strdup ("out");
    inputs->filter_ctx  = sink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;

    if ((ret = avfilter_graph_parse_ptr (graph, filtergraph, &inputs, &outputs, NULL)) < 0)
      goto fail;
    }

  else {
    if ((ret = avfilter_link (source_ctx, 0, sink_ctx, 0)) < 0)
      goto fail;
    } /* Reorder the filters to ensure that inputs of the custom filters are merged first */

  for (i = 0; i < graph->nb_filters - nb_filters; i++)
    FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

  ret = avfilter_graph_config (graph, NULL);

fail:
  avfilter_inout_free (&outputs);
  avfilter_inout_free (&inputs);
  return ret;
  }
//}}}
//{{{
static int decodeFrame (Decoder* decoder, AVFrame* frame, AVSubtitle *sub) {

  int ret = AVERROR(EAGAIN);

  for (;;) {
    if (decoder->queue->serial == decoder->pkt_serial) {
      do {
        if (decoder->queue->abort_request)
          return -1;

        switch (decoder->avctx->codec_type) {
          //{{{
          case AVMEDIA_TYPE_VIDEO:
            ret = avcodec_receive_frame(decoder->avctx, frame);
            if (ret >= 0) {
              if (decoder_reorder_pts == -1)
                frame->pts = frame->best_effort_timestamp;
              else if (!decoder_reorder_pts)
                frame->pts = frame->pkt_dts;
              }
            break;
          //}}}
          //{{{
          case AVMEDIA_TYPE_AUDIO:
            ret = avcodec_receive_frame (decoder->avctx, frame);
            if (ret >= 0) {
              AVRational tb = (AVRational){1, frame->sample_rate};
              if (frame->pts != AV_NOPTS_VALUE)
                frame->pts = av_rescale_q (frame->pts, decoder->avctx->pkt_timebase, tb);
              else if (decoder->next_pts != AV_NOPTS_VALUE)
                frame->pts = av_rescale_q (decoder->next_pts, decoder->next_pts_tb, tb);

              if (frame->pts != AV_NOPTS_VALUE) {
                decoder->next_pts = frame->pts + frame->nb_samples;
                decoder->next_pts_tb = tb;
                }
              }
            break;
          //}}}
          }
        if (ret == AVERROR_EOF) {
          //{{{  end of file, return
          decoder->finished = decoder->pkt_serial;
          avcodec_flush_buffers (decoder->avctx);
          return 0;
          }
          //}}}
        if (ret >= 0)
          return 1;
        } while (ret != AVERROR(EAGAIN));
      }

    do {
      if (decoder->queue->nb_packets == 0)
        SDL_CondSignal (decoder->empty_queue_cond);
      if (decoder->packet_pending)
        decoder->packet_pending = 0;
      else {
        int old_serial = decoder->pkt_serial;
        if (packet_queue_get (decoder->queue, decoder->pkt, 1, &decoder->pkt_serial) < 0)
          return -1;
        if (old_serial != decoder->pkt_serial) {
          avcodec_flush_buffers (decoder->avctx);
          decoder->finished = 0;
          decoder->next_pts = decoder->start_pts;
          decoder->next_pts_tb = decoder->start_pts_tb;
          }
        }
      if (decoder->queue->serial == decoder->pkt_serial)
        break;

      av_packet_unref (decoder->pkt);
      } while (1);

    if (decoder->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      //{{{  subtitle
      int got_frame = 0;
      ret = avcodec_decode_subtitle2 (decoder->avctx, sub, &got_frame, decoder->pkt);
      if (ret < 0)
        ret = AVERROR(EAGAIN);
      else {
        if (got_frame && !decoder->pkt->data)
          decoder->packet_pending = 1;
        ret = got_frame ? 0 : (decoder->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
        }
      av_packet_unref (decoder->pkt);
      }
      //}}}
    else {
      //{{{  audio, video
      if (decoder->pkt->buf && !decoder->pkt->opaque_ref) {
        FrameData *fd;

        decoder->pkt->opaque_ref = av_buffer_allocz (sizeof(*fd));
        if (!decoder->pkt->opaque_ref)
          return AVERROR(ENOMEM);
        fd = (FrameData*)decoder->pkt->opaque_ref->data;
        fd->pkt_pos = decoder->pkt->pos;
        }

      if (avcodec_send_packet (decoder->avctx, decoder->pkt) == AVERROR(EAGAIN)) {
        av_log (decoder->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
        decoder->packet_pending = 1;
        }
      else
        av_packet_unref (decoder->pkt);
      }
      //}}}
    }
  }
//}}}

//{{{
static int configureVideoFilters (AVFilterGraph* graph, VideoState* videoState,
                                    const char* vfilters, AVFrame* frame) {

  enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];

  char sws_flags_str[512] = "";
  char buffersrc_args[256];

  AVFilterContext* filt_src = NULL;
  AVFilterContext* filt_out = NULL;
  AVFilterContext* last_filter = NULL;

  AVCodecParameters* codecpar = videoState->video_st->codecpar;
  AVRational fr = av_guess_frame_rate (videoState->ic, videoState->video_st, NULL);

  int nb_pix_fmts = 0;
  for (int i = 0; i < renderer_info.num_texture_formats; i++) {
    for (int j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
      if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
        pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
        break;
        }
      }
    }
  pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

  const AVDictionaryEntry* e = NULL;
  while ((e = av_dict_iterate (sws_dict, e))) {
    if (!strcmp(e->key, "sws_flags"))
      av_strlcatf (sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
    else
      av_strlcatf (sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }

  if (strlen (sws_flags_str))
    sws_flags_str[strlen(sws_flags_str)-1] = '\0';

  graph->scale_sws_opts = av_strdup(sws_flags_str);

  snprintf (buffersrc_args, sizeof(buffersrc_args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            frame->width, frame->height, frame->format,
            videoState->video_st->time_base.num, videoState->video_st->time_base.den,
            codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
  if (fr.num && fr.den)
    av_strlcatf (buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

  int ret;
  if ((ret = avfilter_graph_create_filter (&filt_src, avfilter_get_by_name ("buffer"),
                                           "ffplay_buffer", buffersrc_args, NULL, graph)) < 0)
    goto fail;

  ret = avfilter_graph_create_filter (&filt_out, avfilter_get_by_name ("buffersink"),
                                      "ffplay_buffersink", NULL, NULL, graph);
  if (ret < 0)
    goto fail;

  if ((ret = av_opt_set_int_list (filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
    goto fail;

  last_filter = filt_out;

  /* Note: this macro adds a filter before the lastly added filter, so the
   * processing order of the filters is in reverse */
  //{{{
  #define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter (&filt_ctx,                            \
                                        avfilter_get_by_name(name),           \
                                        "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
      goto fail;                                                           \
                                                                             \
    ret = avfilter_link (filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
      goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
    } while (0)
  //}}}

  if (autorotate) {
    double theta = 0.0;
    int32_t* displaymatrix = NULL;
    AVFrameSideData* sd = av_frame_get_side_data (frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (sd)
      displaymatrix = (int32_t *)sd->data;
    if (!displaymatrix) {
      //const AVPacketSideData *sd = av_packet_side_data_get (videoState->video_st->codecpar->coded_side_data,
      //                                                      videoState->video_st->codecpar->nb_coded_side_data,
      //                                                      AV_PKT_DATA_DISPLAYMATRIX);
      //if (sd)
      //    displaymatrix = (int32_t *)sd->data;
      }
    theta = get_rotation (displaymatrix);

    if (fabs (theta - 90) < 1.0) {
      INSERT_FILT("transpose", "clock");
      }
    else if (fabs(theta - 180) < 1.0) {
      INSERT_FILT("hflip", NULL);
      INSERT_FILT("vflip", NULL);
      }
    else if (fabs(theta - 270) < 1.0) {
      INSERT_FILT("transpose", "cclock");
      }
    else if (fabs(theta) > 1.0) {
      char rotate_buf[64];
      snprintf (rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
      INSERT_FILT("rotate", rotate_buf);
      }
    }

  if ((ret = configureFilterGraph (graph, vfilters, filt_src, last_filter)) < 0)
    goto fail;

  videoState->in_video_filter  = filt_src;
  videoState->out_video_filter = filt_out;

fail:
  return ret;
  }
//}}}
//{{{
static int get_video_frame (VideoState* videoState, AVFrame* frame) {

  int got_picture;
  if ((got_picture = decodeFrame (&videoState->viddec, frame, NULL)) < 0)
    return -1;

  if (got_picture) {
    double dpts = NAN;
    if (frame->pts != AV_NOPTS_VALUE)
      dpts = av_q2d (videoState->video_st->time_base) * frame->pts;

    frame->sample_aspect_ratio = av_guess_sample_aspect_ratio (videoState->ic, videoState->video_st, frame);

    if (framedrop >0  || (framedrop && get_master_sync_type (videoState) != AV_SYNC_VIDEO_MASTER)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        double diff = dpts - get_master_clock (videoState);
        if (!isnan (diff) && fabs (diff) < AV_NOSYNC_THRESHOLD &&
            diff - videoState->frame_last_filter_delay < 0 &&
            videoState->viddec.pkt_serial == videoState->vidclk.serial &&
            videoState->videoq.nb_packets) {
          videoState->frame_drops_early++;
          av_frame_unref (frame);
          got_picture = 0;
          }
        }
      }
    }

  return got_picture;
  }
//}}}
//{{{
static int videoThread (void* arg) {

  VideoState* videoState = arg;
  AVFrame* frame = av_frame_alloc();

  AVRational tb = videoState->video_st->time_base;
  AVRational frame_rate = av_guess_frame_rate (videoState->ic, videoState->video_st, NULL);

  AVFilterGraph* graph = NULL;
  AVFilterContext* filt_out = NULL;
  AVFilterContext* filt_in = NULL;

  int last_w = 0;
  int last_h = 0;
  enum AVPixelFormat last_format = -2;
  int last_serial = -1;
  int last_vfilter_idx = 0;

  if (!frame)
    return AVERROR(ENOMEM);

  double pts;
  double duration;
  int ret;
  for (;;) {
    //{{{  frame
    ret = get_video_frame (videoState, frame);
    if (ret < 0)
      goto the_end;

    if (!ret)
      continue;

    if (last_w != frame->width
        || last_h != frame->height
        || last_format != frame->format
        || last_serial != videoState->viddec.pkt_serial
        || last_vfilter_idx != videoState->vfilter_idx) {
      av_log (NULL, AV_LOG_DEBUG,
              "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
              last_w, last_h,
              (const char*)av_x_if_null (av_get_pix_fmt_name(last_format), "none"), last_serial,
              frame->width, frame->height,
              (const char*)av_x_if_null (av_get_pix_fmt_name(frame->format), "none"), videoState->viddec.pkt_serial);

      avfilter_graph_free (&graph);
      graph = avfilter_graph_alloc();
      if (!graph) {
        //{{{  error return
        ret = AVERROR (ENOMEM);
        goto the_end;
        }
        //}}}

      graph->nb_threads = filter_nbthreads;
      if ((ret = configureVideoFilters (graph, videoState, vfilters_list ? vfilters_list[videoState->vfilter_idx] : NULL, frame)) < 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = videoState;
        SDL_PushEvent (&event);
        goto the_end;
        }

      filt_in = videoState->in_video_filter;
      filt_out = videoState->out_video_filter;
      last_w = frame->width;
      last_h = frame->height;
      last_format = frame->format;
      last_serial = videoState->viddec.pkt_serial;
      last_vfilter_idx = videoState->vfilter_idx;
      frame_rate = av_buffersink_get_frame_rate(filt_out);
      }

    ret = av_buffersrc_add_frame (filt_in, frame);
    if (ret < 0)
      goto the_end;

    while (ret >= 0) {
      videoState->frame_last_returned_time = av_gettime_relative() / 1000000.0;

      ret = av_buffersink_get_frame_flags (filt_out, frame, 0);
      if (ret < 0) {
        if (ret == AVERROR_EOF)
          videoState->viddec.finished = videoState->viddec.pkt_serial;
        ret = 0;
        break;
        }

      FrameData* fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;

      videoState->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - videoState->frame_last_returned_time;
      if (fabs (videoState->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
        videoState->frame_last_filter_delay = 0;

      tb = av_buffersink_get_time_base (filt_out);
      duration = (frame_rate.num && frame_rate.den ? av_q2d ((AVRational){frame_rate.den, frame_rate.num}) : 0);
      pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
      ret = queuePicture (videoState, frame, pts, duration, fd ? fd->pkt_pos : -1, videoState->viddec.pkt_serial);

      av_frame_unref (frame);

      if (videoState->videoq.serial != videoState->viddec.pkt_serial)
        break;
      }

    if (ret < 0)
      goto the_end;
    }
    //}}}

the_end:
  avfilter_graph_free (&graph);
  av_frame_free (&frame);
  return 0;
  }
//}}}

//{{{
static int configureAudioFilters (VideoState* videoState, const char* afilters, int force_output_format) {

  static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
  int sample_rates[2] = { 0, -1 };

  AVFilterContext* filt_asrc = NULL;
  AVFilterContext* filt_asink = NULL;

  char aresample_swr_opts[512] = "";
  const AVDictionaryEntry* e = NULL;
  AVBPrint bp;
  char asrc_args[256];
  int ret;

  avfilter_graph_free (&videoState->agraph);
  if (!(videoState->agraph = avfilter_graph_alloc()))
      return AVERROR(ENOMEM);
  videoState->agraph->nb_threads = filter_nbthreads;

  av_bprint_init (&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

  while ((e = av_dict_iterate(swr_opts, e)))
    av_strlcatf (aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
  if (strlen (aresample_swr_opts))
      aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
  av_opt_set (videoState->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

  av_channel_layout_describe_bprint (&videoState->audio_filter_src.ch_layout, &bp);

  ret = snprintf (asrc_args, sizeof(asrc_args),
                  "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                  videoState->audio_filter_src.freq, av_get_sample_fmt_name(videoState->audio_filter_src.fmt),
                  1, videoState->audio_filter_src.freq, bp.str);

  ret = avfilter_graph_create_filter (&filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                      asrc_args, NULL, videoState->agraph);
  if (ret < 0)
     goto end;

  ret = avfilter_graph_create_filter (&filt_asink, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                      NULL, NULL, videoState->agraph);
  if (ret < 0)
    goto end;

  if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
    goto end;
  if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
    goto end;

  if (force_output_format) {
    sample_rates   [0] = videoState->audio_tgt.freq;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
      goto end;
    if ((ret = av_opt_set(filt_asink, "ch_layouts", bp.str, AV_OPT_SEARCH_CHILDREN)) < 0)
      goto end;
    if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
      goto end;
    }

  if ((ret = configureFilterGraph (videoState->agraph, afilters, filt_asrc, filt_asink)) < 0)
    goto end;

  videoState->in_audio_filter  = filt_asrc;
  videoState->out_audio_filter = filt_asink;

end:
  if (ret < 0)
    avfilter_graph_free(&videoState->agraph);
  av_bprint_finalize(&bp, NULL);

  return ret;
  }
//}}}
//{{{
static int audioThread (void* arg) {

  VideoState* videoState = arg;

  AVFrame* frame = av_frame_alloc();
  if (!frame)
    return AVERROR(ENOMEM);

  int ret = 0;
  do {
    int got_frame;
    if ((got_frame = decodeFrame (&videoState->auddec, frame, NULL)) < 0)
      goto the_end;

    int last_serial = -1;
    int reconfigure;
    AVRational tb;
    if (got_frame) {
      //{{{  got frame
      tb = (AVRational){1, frame->sample_rate};

      reconfigure = cmp_audio_fmts (videoState->audio_filter_src.fmt, videoState->audio_filter_src.ch_layout.nb_channels,
                                    frame->format, frame->ch_layout.nb_channels)    ||
      av_channel_layout_compare(&videoState->audio_filter_src.ch_layout, &frame->ch_layout) ||
      videoState->audio_filter_src.freq != frame->sample_rate ||
      videoState->auddec.pkt_serial != last_serial;

      if (reconfigure) {
        char buf1[1024], buf2[1024];
        av_channel_layout_describe (&videoState->audio_filter_src.ch_layout, buf1, sizeof(buf1));
        av_channel_layout_describe (&frame->ch_layout, buf2, sizeof(buf2));
        av_log (NULL, AV_LOG_DEBUG,
                "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                videoState->audio_filter_src.freq, videoState->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(videoState->audio_filter_src.fmt), buf1, last_serial,
                frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(frame->format), buf2, videoState->auddec.pkt_serial);

        videoState->audio_filter_src.fmt = frame->format;
        ret = av_channel_layout_copy(&videoState->audio_filter_src.ch_layout, &frame->ch_layout);
        if (ret < 0)
          goto the_end;

        videoState->audio_filter_src.freq = frame->sample_rate;
        last_serial = videoState->auddec.pkt_serial;

        if ((ret = configureAudioFilters (videoState, afilters, 1)) < 0)
          goto the_end;
        }

      if ((ret = av_buffersrc_add_frame (videoState->in_audio_filter, frame)) < 0)
        goto the_end;

      while ((ret = av_buffersink_get_frame_flags(videoState->out_audio_filter, frame, 0)) >= 0) {
        FrameData* fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
        tb = av_buffersink_get_time_base (videoState->out_audio_filter);

        Frame* af;
        if (!(af = frame_queue_peek_writable (&videoState->sampq)))
          goto the_end;

        af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        af->pos = fd ? fd->pkt_pos : -1;
        af->serial = videoState->auddec.pkt_serial;
        af->duration = av_q2d ((AVRational){frame->nb_samples, frame->sample_rate});

        av_frame_move_ref (af->frame, frame);
        frame_queue_push (&videoState->sampq);

        if (videoState->audioq.serial != videoState->auddec.pkt_serial)
          break;
        }

      if (ret == AVERROR_EOF)
        videoState->auddec.finished = videoState->auddec.pkt_serial;
      }
      //}}}
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:
  avfilter_graph_free (&videoState->agraph);
  av_frame_free (&frame);

  return ret;
  }
//}}}

//{{{
static int subtitleThread (void* arg) {

  VideoState* videoState = arg;

  for (;;) {
    Frame* sp;
    if (!(sp = frame_queue_peek_writable (&videoState->subpq)))
      return 0;

    int got_subtitle;
    if ((got_subtitle = decodeFrame (&videoState->subdec, NULL, &sp->sub)) < 0)
      break;

    double pts = 0;
    if (got_subtitle && sp->sub.format == 0) {
      if (sp->sub.pts != AV_NOPTS_VALUE)
        pts = sp->sub.pts / (double)AV_TIME_BASE;
      sp->pts = pts;
      sp->serial = videoState->subdec.pkt_serial;
      sp->width = videoState->subdec.avctx->width;
      sp->height = videoState->subdec.avctx->height;
      sp->uploaded = 0;

      // now we can update the picture count
      frame_queue_push (&videoState->subpq);
      }
    else if (got_subtitle)
      avsubtitle_free (&sp->sub);
    }

  return 0;
  }
//}}}

// decoder
//{{{
static int decoder_init (Decoder* d, AVCodecContext* avctx, PacketQueue* queue, SDL_cond *empty_queue_cond) {

  memset (d, 0, sizeof(Decoder));

  d->pkt = av_packet_alloc();
  if (!d->pkt)
    return AVERROR(ENOMEM);

  d->avctx = avctx;
  d->queue = queue;
  d->empty_queue_cond = empty_queue_cond;
  d->start_pts = AV_NOPTS_VALUE;
  d->pkt_serial = -1;

  return 0;
  }
//}}}
//{{{
static int decoder_start (Decoder* d, int (*fn)(void*), const char* thread_name, void* arg) {

  packet_queue_start (d->queue);
  d->decoder_tid = SDL_CreateThread (fn, thread_name, arg);
  if (!d->decoder_tid) {
    av_log (NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
    }

  return 0;
  }
//}}}
//{{{  event handlers
//{{{
/* seek in the stream */
static void stream_seek (VideoState* videoState, int64_t pos, int64_t rel, int by_bytes) {

  if (!videoState->seek_req) {
    videoState->seek_pos = pos;
    videoState->seek_rel = rel;
    videoState->seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (by_bytes)
      videoState->seek_flags |= AVSEEK_FLAG_BYTE;
    videoState->seek_req = 1;
     SDL_CondSignal (videoState->continue_read_thread);
    }
  }
//}}}
//{{{
static void seek_chapter (VideoState* videoState, int incr) {

  int64_t pos = get_master_clock (videoState) * AV_TIME_BASE;

  if (!videoState->ic->nb_chapters)
    return;

  /* find the current chapter */
  int i;
  for (i = 0; i < videoState->ic->nb_chapters; i++) {
    AVChapter* ch = videoState->ic->chapters[i];
    if (av_compare_ts (pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
      i--;
      break;
      }
    }

  i += incr;
  i = FFMAX(i, 0);
  if (i >= videoState->ic->nb_chapters)
    return;

  av_log (NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
  stream_seek (videoState, av_rescale_q (videoState->ic->chapters[i]->start, videoState->ic->chapters[i]->time_base, AV_TIME_BASE_Q), 0, 0);
  }
//}}}

//{{{
static void toggle_pause (VideoState* videoState) {

  stream_toggle_pause (videoState);
  videoState->step = 0;
  }
//}}}
//{{{
static void step_to_next_frame (VideoState* videoState) {

  /* if the stream is paused unpause it, then step */
  if (videoState->paused)
    stream_toggle_pause (videoState);

  videoState->step = 1;
  }
//}}}

//{{{
static void toggle_mute (VideoState* videoState) {
  videoState->muted = !videoState->muted;
  }
//}}}
//{{{
static void update_volume (VideoState* videoState, int sign, double step) {

  double volume_level = videoState->audio_volume ? (20 * log(videoState->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
  int new_volume = lrint (SDL_MIX_MAXVOLUME * pow (10.0, (volume_level + sign * step) / 20.0));

  videoState->audio_volume = av_clip (videoState->audio_volume == new_volume ? (videoState->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
  }
//}}}

//{{{
static void toggle_full_screen (VideoState* videoState) {

  is_full_screen = !is_full_screen;
  SDL_SetWindowFullscreen (window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  }
//}}}
//{{{
static void toggle_audio_display (VideoState* videoState) {

  int next = videoState->show_mode;
  do {
    next = (next + 1) % SHOW_MODE_NB;
    } while (next != videoState->show_mode &&
            (next == SHOW_MODE_VIDEO && !videoState->video_st || next != SHOW_MODE_VIDEO && !videoState->audio_st));

  if (videoState->show_mode != next) {
    videoState->force_refresh = 1;
    videoState->show_mode = next;
    }
  }
//}}}
//}}}

// stream
//{{{
static int stream_has_enough_packets (AVStream* st, int stream_id, PacketQueue* queue) {
  return stream_id < 0 ||
         queue->abort_request ||
         (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
         queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d (st->time_base) * queue->duration > 1.0);
  }
//}}}
//{{{
static int stream_component_open (VideoState* videoState, int stream_index) {
/* open a given stream. Return 0 if OK */

  AVFormatContext* ic = videoState->ic;
  const AVCodec* codec;
  const char* forced_codec_name = NULL;
  AVDictionary* opts = NULL;
  const AVDictionaryEntry* t = NULL;
  int sample_rate;
  AVChannelLayout ch_layout = { 0 };
  int stream_lowres = lowres;

  if (stream_index < 0 || stream_index >= ic->nb_streams)
    return -1;

  AVCodecContext* avctx = avcodec_alloc_context3(NULL);
  if (!avctx)
    return AVERROR(ENOMEM);

  int ret = avcodec_parameters_to_context (avctx, ic->streams[stream_index]->codecpar);
  if (ret < 0)
    goto fail;
  avctx->pkt_timebase = ic->streams[stream_index]->time_base;

  codec = avcodec_find_decoder (avctx->codec_id);

  switch (avctx->codec_type){
    //{{{
    case AVMEDIA_TYPE_AUDIO:
      videoState->last_audio_stream = stream_index;
      forced_codec_name = audio_codec_name;
      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_SUBTITLE:
      videoState->last_subtitle_stream = stream_index;
      forced_codec_name = subtitle_codec_name;
      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_VIDEO:
      videoState->last_video_stream = stream_index;
      forced_codec_name = video_codec_name;
      break;
    //}}}
    }

  if (forced_codec_name)
    codec = avcodec_find_decoder_by_name (forced_codec_name);
  if (!codec) {
    if (forced_codec_name)
      av_log (NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
    else
     av_log (NULL, AV_LOG_WARNING, "No decoder could be found for codec %s\n", avcodec_get_name (avctx->codec_id));
    ret = AVERROR(EINVAL);
    goto fail;
    }

  avctx->codec_id = codec->id;
  if (stream_lowres > codec->max_lowres) {
    av_log (avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                                   codec->max_lowres);
    stream_lowres = codec->max_lowres;
    }
  avctx->lowres = stream_lowres;

  if (fast)
    avctx->flags2 |= AV_CODEC_FLAG2_FAST;

  ret = filter_codec_opts (codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec, &opts);
  if (ret < 0)
    goto fail;

  if (!av_dict_get (opts, "threads", NULL, 0))
    av_dict_set (&opts, "threads", "auto", 0);
  if (stream_lowres)
    av_dict_set_int (&opts, "lowres", stream_lowres, 0);

  av_dict_set (&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

  if ((ret = avcodec_open2 (avctx, codec, &opts)) < 0)
    goto fail;

  if ((t = av_dict_get (opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log (NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret =  AVERROR_OPTION_NOT_FOUND;
    goto fail;
    }

  videoState->eof = 0;
  ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
  switch (avctx->codec_type) {
    //{{{
    case AVMEDIA_TYPE_AUDIO: {
      AVFilterContext* sink;
      videoState->audio_filter_src.freq = avctx->sample_rate;
      ret = av_channel_layout_copy(&videoState->audio_filter_src.ch_layout, &avctx->ch_layout);
      if (ret < 0)
        goto fail;
      videoState->audio_filter_src.fmt = avctx->sample_fmt;
      if ((ret = configureAudioFilters (videoState, afilters, 0)) < 0)
        goto fail;
      sink = videoState->out_audio_filter;
      sample_rate = av_buffersink_get_sample_rate (sink);
      ret = av_buffersink_get_ch_layout (sink, &ch_layout);
      if (ret < 0)
        goto fail;
      }

      /* prepare audio output */
      if ((ret = audio_open (videoState, &ch_layout, sample_rate, &videoState->audio_tgt)) < 0)
        goto fail;
      videoState->audio_hw_buf_size = ret;
      videoState->audio_src = videoState->audio_tgt;
      videoState->audio_buf_size  = 0;
      videoState->audio_buf_index = 0;

      /* init averaging filter */
      videoState->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
      videoState->audio_diff_avg_count = 0;
      /* since we do not have a precise anough audio FIFO fullness,
         we correct audio sync only if larger than this threshold */
      videoState->audio_diff_threshold = (double)(videoState->audio_hw_buf_size) / videoState->audio_tgt.bytes_per_sec;

      videoState->audio_stream = stream_index;
      videoState->audio_st = ic->streams[stream_index];

      if ((ret = decoder_init (&videoState->auddec, avctx, &videoState->audioq, videoState->continue_read_thread)) < 0)
        goto fail;
      if (videoState->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
        videoState->auddec.start_pts = videoState->audio_st->start_time;
        videoState->auddec.start_pts_tb = videoState->audio_st->time_base;
        }
      if ((ret = decoder_start (&videoState->auddec, audioThread, "audio_decoder", videoState)) < 0)
        goto out;

      SDL_PauseAudioDevice (audio_dev, 0);
      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_VIDEO:
      videoState->video_stream = stream_index;
        videoState->video_st = ic->streams[stream_index];

      if ((ret = decoder_init(&videoState->viddec, avctx, &videoState->videoq, videoState->continue_read_thread)) < 0)
        goto fail;
      if ((ret = decoder_start(&videoState->viddec, videoThread, "video_decoder", videoState)) < 0)
        goto out;

      videoState->queue_attachments_req = 1;

      break;
    //}}}
    //{{{
    case AVMEDIA_TYPE_SUBTITLE:
      videoState->subtitle_stream = stream_index;
      videoState->subtitle_st = ic->streams[stream_index];

      if ((ret = decoder_init(&videoState->subdec, avctx, &videoState->subtitleq, videoState->continue_read_thread)) < 0)
        goto fail;
      if ((ret = decoder_start(&videoState->subdec, subtitleThread, "subtitle_decoder", videoState)) < 0)
        goto out;

      break;
    //}}}
    //{{{
    default:
      break;
    //}}}
    }
  goto out;

fail:
  avcodec_free_context (&avctx);

out:
  av_channel_layout_uninit (&ch_layout);
  av_dict_free (&opts);

  return ret;
  }
//}}}
//{{{
static void stream_cycle_channel (VideoState* videoState, int codec_type) {

  AVFormatContext* ic = videoState->ic;
  int nb_streams = videoState->ic->nb_streams;

  int start_index;
  int old_index;
  if (codec_type == AVMEDIA_TYPE_VIDEO) {
    start_index = videoState->last_video_stream;
    old_index = videoState->video_stream;
    }
  else if (codec_type == AVMEDIA_TYPE_AUDIO) {
    start_index = videoState->last_audio_stream;
    old_index = videoState->audio_stream;
    }
  else {
    start_index = videoState->last_subtitle_stream;
    old_index = videoState->subtitle_stream;
    }
  int stream_index = start_index;

  AVProgram* p = NULL;
  if (codec_type != AVMEDIA_TYPE_VIDEO && videoState->video_stream != -1) {
    p = av_find_program_from_stream (ic, NULL, videoState->video_stream);
    if (p) {
      nb_streams = p->nb_stream_indexes;
      for (start_index = 0; start_index < nb_streams; start_index++)
        if (p->stream_index[start_index] == stream_index)
          break;
      if (start_index == nb_streams)
        start_index = -1;
      stream_index = start_index;
      }
    }

  for (;;) {
    if (++stream_index >= nb_streams) {
      if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
        //{{{  subtitle
        stream_index = -1;
        videoState->last_subtitle_stream = -1;

        goto the_end;
        }
        //}}}
      if (start_index == -1)
        return;
      stream_index = 0;
      }
    if (stream_index == start_index)
      return;

    AVStream* st = videoState->ic->streams[p ? p->stream_index[stream_index] : stream_index];
    if (st->codecpar->codec_type == codec_type) {
      // check that parameters are OK
      switch (codec_type) {
        //{{{
        case AVMEDIA_TYPE_AUDIO:
          if (st->codecpar->sample_rate != 0 && st->codecpar->ch_layout.nb_channels != 0)
            goto the_end;
          break;
        //}}}
        case AVMEDIA_TYPE_VIDEO:
        //{{{
        case AVMEDIA_TYPE_SUBTITLE:
          goto the_end;
        //}}}
        //{{{
        default:
          break;
        //}}}
        }
      }
    }

the_end:
  if (p && stream_index != -1)
    stream_index = p->stream_index[stream_index];

  av_log (NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
                             av_get_media_type_string(codec_type), old_index, stream_index);

  stream_component_close (videoState, old_index);
  stream_component_open (videoState, stream_index);
  }
//}}}
//{{{
static int isRealtime (AVFormatContext* s) {

  if (!strcmp (s->iformat->name, "rtp") ||
      !strcmp (s->iformat->name, "rtsp") ||
      !strcmp (s->iformat->name, "sdp"))
    return 1;

  if (s->pb &&
      (!strncmp (s->url, "rtp:", 4) || !strncmp (s->url, "udp:", 4)))
    return 1;

  return 0;
  }
//}}}
//{{{
static int decodeInterruptCallback (void* ctx) {

  VideoState* videoState = ctx;
  return videoState->abort_request;
  }
//}}}
//{{{
static int readThread (void* arg) {
// this thread gets the stream from the disk or the network

  VideoState* videoState = arg;
  SDL_mutex* wait_mutex = SDL_CreateMutex();

  int i, ret;
  int st_index[AVMEDIA_TYPE_NB];
  int64_t stream_start_time;
  int pkt_in_play_range = 0;
  int scan_all_pmts_set = 0;
  int64_t pkt_ts;

  if (!wait_mutex) {
    //{{{  error
    av_log (NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    ret = AVERROR (ENOMEM);
    goto fail;
    }
    //}}}

  memset (st_index, -1, sizeof(st_index));
  videoState->eof = 0;

  AVPacket* pkt = av_packet_alloc();
  if (!pkt) {
    //{{{  error
    av_log (NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
    ret = AVERROR (ENOMEM);
    goto fail;
    }
    //}}}

  AVFormatContext* ic = avformat_alloc_context();
  if (!ic) {
    //{{{  error
    av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
    }
    //}}}
  ic->interrupt_callback.callback = decodeInterruptCallback;
  ic->interrupt_callback.opaque = videoState;
  if (!av_dict_get (format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
    av_dict_set (&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    scan_all_pmts_set = 1;
    }

  int err = avformat_open_input (&ic, videoState->filename, videoState->iformat, &format_opts);
  if (err < 0) {
    //{{{  error
    print_error(videoState->filename, err);
    ret = -1;
    goto fail;
    }
    //}}}

  if (scan_all_pmts_set)
    av_dict_set (&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

  const AVDictionaryEntry* t;
  if ((t = av_dict_get (format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log (NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto fail;
    }

  videoState->ic = ic;

  if (genpts)
    ic->flags |= AVFMT_FLAG_GENPTS;

  if (find_stream_info) {
    int orig_nb_streams = ic->nb_streams;
    AVDictionary** opts;
    err = setup_find_stream_info_opts (ic, codec_opts, &opts);
    if (err < 0) {
      //{{{  error fail
      av_log (NULL, AV_LOG_ERROR, "Error setting up avformat_find_stream_info() options\n");
      ret = err;
      goto fail;
      }
      //}}}

    err = avformat_find_stream_info (ic, opts);
    for (i = 0; i < orig_nb_streams; i++)
      av_dict_free (&opts[i]);
    av_freep (&opts);
    if (err < 0) {
      //{{{  error fail
      av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", videoState->filename);
      ret = -1;
      goto fail;
      }
      //}}}
    }

  if (ic->pb)
    ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

  if (seek_by_bytes < 0)
    seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                    !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                    strcmp ("ogg", ic->iformat->name);

  videoState->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

  if (!window_title && (t = av_dict_get (ic->metadata, "title", NULL, 0)))
    window_title = av_asprintf ("%s - %s", t->value, input_filename);

  /* if seeking requested, we execute it */
  if (start_time != AV_NOPTS_VALUE) {
    int64_t timestamp = start_time;
    /* add the stream start time */
    if (ic->start_time != AV_NOPTS_VALUE)
      timestamp += ic->start_time;
    ret = avformat_seek_file (ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0)
      av_log (NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                                    videoState->filename, (double)timestamp / AV_TIME_BASE);
    }

  videoState->realtime = isRealtime (ic);

  if (show_status)
    av_dump_format (ic, 0, videoState->filename, 0);

  for (i = 0; i < ic->nb_streams; i++) {
    AVStream* st = ic->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
      if (avformat_match_stream_specifier (ic, st, wanted_stream_spec[type]) > 0)
        st_index[type] = i;
    }

  for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
    if (wanted_stream_spec[i] && st_index[i] == -1) {
      av_log (NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                                  wanted_stream_spec[i], av_get_media_type_string(i));
      st_index[i] = INT_MAX;
      }
    }

  if (!video_disable)
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream (ic, AVMEDIA_TYPE_VIDEO,
                                                        st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
  if (!audio_disable)
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream (ic, AVMEDIA_TYPE_AUDIO,
                                                        st_index[AVMEDIA_TYPE_AUDIO],
                                                        st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
  if (!video_disable && !subtitle_disable)
    st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream (ic, AVMEDIA_TYPE_SUBTITLE,
                                                           st_index[AVMEDIA_TYPE_SUBTITLE],
                                                           (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                                            st_index[AVMEDIA_TYPE_AUDIO] :
                                                            st_index[AVMEDIA_TYPE_VIDEO]), NULL, 0);

  videoState->show_mode = show_mode;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    AVStream* st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
    AVCodecParameters* codecpar = st->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio (ic, st, NULL);
    if (codecpar->width)
      set_default_window_size (codecpar->width, codecpar->height, sar);
    }

  /* open the streams */
  if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
    stream_component_open (videoState, st_index[AVMEDIA_TYPE_AUDIO]);

  ret = -1;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
    ret = stream_component_open (videoState, st_index[AVMEDIA_TYPE_VIDEO]);

  if (videoState->show_mode == SHOW_MODE_NONE)
    videoState->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

  if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0)
    stream_component_open (videoState, st_index[AVMEDIA_TYPE_SUBTITLE]);

  if (videoState->video_stream < 0 && videoState->audio_stream < 0) {
    //{{{  error
    av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", videoState->filename);
    ret = -1;
    goto fail;
    }
    //}}}

  if (infinite_buffer < 0 && videoState->realtime)
    infinite_buffer = 1;

  for (;;) {
    if (videoState->abort_request)
      break;
    if (videoState->paused != videoState->last_paused) {
      videoState->last_paused = videoState->paused;
      if (videoState->paused)
        videoState->read_pause_return = av_read_pause (ic);
      else
        av_read_play (ic);
      }

    #if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
      if (videoState->paused &&
          (!strcmp (ic->iformat->name, "rtsp") ||
                   (ic->pb && !strncmp (input_filename, "mmsh:", 5)))) {
        /* wait 10 ms to avoid trying to get another packet */
        SDL_Delay (10);
        continue;
        }
    #endif

    if (videoState->seek_req) {
      int64_t seek_target = videoState->seek_pos;
      int64_t seek_min = videoState->seek_rel > 0 ? seek_target - videoState->seek_rel + 2: INT64_MIN;
      int64_t seek_max = videoState->seek_rel < 0 ? seek_target - videoState->seek_rel - 2: INT64_MAX;

      // FIXME the +-2 is due to rounding being not done in the correct direction in generation
      //      of the seek_pos/seek_rel variables
      ret = avformat_seek_file (videoState->ic, -1, seek_min, seek_target, seek_max, videoState->seek_flags);
      if (ret < 0)
        av_log (NULL, AV_LOG_ERROR, "%s: error while seeking\n", videoState->ic->url);
      else {
        if (videoState->audio_stream >= 0)
          packet_queue_flush (&videoState->audioq);
        if (videoState->subtitle_stream >= 0)
          packet_queue_flush (&videoState->subtitleq);
        if (videoState->video_stream >= 0)
          packet_queue_flush (&videoState->videoq);
        if (videoState->seek_flags & AVSEEK_FLAG_BYTE)
         set_clock (&videoState->extclk, NAN, 0);
        else
          set_clock (&videoState->extclk, seek_target / (double)AV_TIME_BASE, 0);
        }

      videoState->seek_req = 0;
      videoState->queue_attachments_req = 1;
      videoState->eof = 0;
      if (videoState->paused)
        step_to_next_frame (videoState);
      }

    if (videoState->queue_attachments_req) {
      if (videoState->video_st && videoState->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        if ((ret = av_packet_ref (pkt, &videoState->video_st->attached_pic)) < 0)
          goto fail;
        packet_queue_put (&videoState->videoq, pkt);
        packet_queue_put_nullpacket (&videoState->videoq, pkt, videoState->video_stream);
        }
      videoState->queue_attachments_req = 0;
      }

    /* if the queue are full, no need to read more */
    if (infinite_buffer<1 &&
        (videoState->audioq.size + videoState->videoq.size + videoState->subtitleq.size > MAX_QUEUE_SIZE
        || (stream_has_enough_packets (videoState->audio_st, videoState->audio_stream, &videoState->audioq) &&
        stream_has_enough_packets (videoState->video_st, videoState->video_stream, &videoState->videoq) &&
        stream_has_enough_packets (videoState->subtitle_st, videoState->subtitle_stream, &videoState->subtitleq)))) {
       //{{{  wait 10 ms
       SDL_LockMutex(wait_mutex);
       SDL_CondWaitTimeout(videoState->continue_read_thread, wait_mutex, 10);
       SDL_UnlockMutex(wait_mutex);
       continue;
       }
       //}}}

    if (!videoState->paused &&
        (!videoState->audio_st || (videoState->auddec.finished == videoState->audioq.serial && frame_queue_nb_remaining(&videoState->sampq) == 0)) &&
        (!videoState->video_st || (videoState->viddec.finished == videoState->videoq.serial && frame_queue_nb_remaining(&videoState->pictq) == 0))) {
      if (loop != 1 && (!loop || --loop))
        stream_seek (videoState, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
      else if (autoexit) {
        ret = AVERROR_EOF;
        goto fail;
        }
      }

    ret = av_read_frame (ic, pkt);
    if (ret < 0) {
      if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !videoState->eof) {
        if (videoState->video_stream >= 0)
          packet_queue_put_nullpacket (&videoState->videoq, pkt, videoState->video_stream);
        if (videoState->audio_stream >= 0)
          packet_queue_put_nullpacket (&videoState->audioq, pkt, videoState->audio_stream);
        if (videoState->subtitle_stream >= 0)
          packet_queue_put_nullpacket (&videoState->subtitleq, pkt, videoState->subtitle_stream);
        videoState->eof = 1;
        }

      if (ic->pb && ic->pb->error) {
        if (autoexit)
          goto fail;
        else
          break;
        }

      SDL_LockMutex (wait_mutex);
      SDL_CondWaitTimeout (videoState->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex (wait_mutex);
      continue;
      }
    else
      videoState->eof = 0;

    /* check if packet is in play range specified by user, then queue, otherwise discard */
    stream_start_time = ic->streams[pkt->stream_index]->start_time;
    pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                        (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                        av_q2d(ic->streams[pkt->stream_index]->time_base) -
                        (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                        <= ((double)duration / 1000000);
    if (pkt->stream_index == videoState->audio_stream && pkt_in_play_range)
      packet_queue_put (&videoState->audioq, pkt);
    else if (pkt->stream_index == videoState->video_stream && pkt_in_play_range
             && !(videoState->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
      packet_queue_put (&videoState->videoq, pkt);
    else if (pkt->stream_index == videoState->subtitle_stream && pkt_in_play_range)
      packet_queue_put (&videoState->subtitleq, pkt);
   else
      av_packet_unref (pkt);
    }

   ret = 0;

fail:
  if (ic && !videoState->ic)
    avformat_close_input (&ic);

  av_packet_free (&pkt);
  if (ret != 0) {
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = videoState;
    SDL_PushEvent (&event);
    }

  SDL_DestroyMutex (wait_mutex);
  return 0;
  }
//}}}
//{{{
static VideoState* stream_open (const char* filename, const AVInputFormat* iformat) {

  VideoState* videoState = av_mallocz (sizeof(VideoState));
  if (!videoState)
    return NULL;

  videoState->last_video_stream = videoState->video_stream = -1;
  videoState->last_audio_stream = videoState->audio_stream = -1;
  videoState->last_subtitle_stream = videoState->subtitle_stream = -1;
  videoState->filename = av_strdup (filename);
  if (!videoState->filename)
    goto fail;
  videoState->iformat = iformat;
  videoState->ytop = 0;
  videoState->xleft = 0;

  // start video display
  if (frame_queue_init (&videoState->pictq, &videoState->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
    goto fail;
  if (frame_queue_init (&videoState->subpq, &videoState->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
    goto fail;
  if (frame_queue_init (&videoState->sampq, &videoState->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
    goto fail;

  if (packet_queue_init (&videoState->videoq) < 0 ||
      packet_queue_init (&videoState->audioq) < 0 ||
      packet_queue_init (&videoState->subtitleq) < 0)
    goto fail;

  if (!(videoState->continue_read_thread = SDL_CreateCond())) {
    //{{{  error return
    av_log (NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    goto fail;
    }
    //}}}

  init_clock (&videoState->vidclk, &videoState->videoq.serial);
  init_clock (&videoState->audclk, &videoState->audioq.serial);
  init_clock (&videoState->extclk, &videoState->extclk.serial);
  videoState->audio_clock_serial = -1;
  if (startup_volume < 0)
    av_log (NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
  if (startup_volume > 100)
    av_log (NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);

  startup_volume = av_clip (startup_volume, 0, 100);
  startup_volume = av_clip (SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);

  videoState->audio_volume = startup_volume;
  videoState->muted = 0;
  videoState->av_sync_type = av_sync_type;

  videoState->read_tid = SDL_CreateThread (readThread, "readThread", videoState);
  if (!videoState->read_tid) {
    av_log (NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
    stream_close (videoState);
    return NULL;
    }

  return videoState;
  }
//}}}

//{{{
static void eventLoop (VideoState* videoState) {
// handle an event sent by the GUI

  double remaining_time = 0.0;
  for (;;) {
    SDL_PumpEvents();
    SDL_Event event;
    while (!SDL_PeepEvents (&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
      if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
        SDL_ShowCursor (0);
        cursor_hidden = 1;
        }

      if (remaining_time > 0.0)
        av_usleep ((int64_t)(remaining_time * 1000000.0));
      remaining_time = REFRESH_RATE;

      if ((videoState->show_mode != SHOW_MODE_NONE) &&
          (!videoState->paused || videoState->force_refresh))
        videoRefresh (videoState, &remaining_time);
      SDL_PumpEvents();
      }

    double x, incr, pos, frac;
    switch (event.type) {
      //{{{
      case SDL_KEYDOWN:
        if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
          //{{{  escape, exit
          do_exit (videoState);
          break;
          }
          //}}}

        // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
        if (!videoState->width)
           continue;

        switch (event.key.keysym.sym) {
          //{{{
          case SDLK_f:
            toggle_full_screen (videoState);
            videoState->force_refresh = 1;
            break;
          //}}}
          case SDLK_p:
          //{{{
          case SDLK_SPACE:
            toggle_pause (videoState);
            break;
          //}}}
          //{{{
          case SDLK_m:
            toggle_mute (videoState);
            break;
          //}}}
          case SDLK_KP_MULTIPLY:
          //{{{
          case SDLK_0:
            update_volume (videoState, 1, SDL_VOLUME_STEP);
            break;
          //}}}
          case SDLK_KP_DIVIDE:
          //{{{
          case SDLK_9:
            update_volume (videoState, -1, SDL_VOLUME_STEP);
            break;
          //}}}
          //{{{
          case SDLK_s: // S: Step to next frame
            step_to_next_frame (videoState);
            break;
          //}}}
          //{{{
          case SDLK_a: // cycle audio
            stream_cycle_channel (videoState, AVMEDIA_TYPE_AUDIO);
             break;
          //}}}
          //{{{
          case SDLK_v: // cycle video
            stream_cycle_channel (videoState, AVMEDIA_TYPE_VIDEO);
            break;
          //}}}
          //{{{
          case SDLK_c: // cycle stream
            stream_cycle_channel (videoState, AVMEDIA_TYPE_VIDEO);
            stream_cycle_channel (videoState, AVMEDIA_TYPE_AUDIO);
            stream_cycle_channel (videoState, AVMEDIA_TYPE_SUBTITLE);
            break;
          //}}}
          //{{{
          case SDLK_t:
            stream_cycle_channel (videoState, AVMEDIA_TYPE_SUBTITLE);
            break;
          //}}}
          //{{{
          case SDLK_w:
            if (videoState->show_mode == SHOW_MODE_VIDEO && videoState->vfilter_idx < nb_vfilters - 1) {
              if (++videoState->vfilter_idx >= nb_vfilters)
                videoState->vfilter_idx = 0;
                }
              else {
                videoState->vfilter_idx = 0;
                toggle_audio_display (videoState);
                }
            break;
          //}}}
          //{{{
          case SDLK_PAGEUP:
            if (videoState->ic->nb_chapters <= 1) {
              incr = 600.0;
              goto do_seek;
              }

            seek_chapter (videoState, 1);
            break;
          //}}}
          //{{{
          case SDLK_PAGEDOWN:
            if (videoState->ic->nb_chapters <= 1) {
              incr = -600.0;
              goto do_seek;
              }

            seek_chapter (videoState, -1);
            break;
          //}}}
          //{{{
          case SDLK_LEFT:
            incr = seek_interval ? -seek_interval : -10.0;
            goto do_seek;
          //}}}
          //{{{
          case SDLK_RIGHT:
            incr = seek_interval ? seek_interval : 10.0;
            goto do_seek;
          //}}}
          //{{{
          case SDLK_UP:
            incr = 60.0;
            goto do_seek;
          //}}}
          //{{{
          case SDLK_DOWN:
            incr = -60.0;

          do_seek:
            if (seek_by_bytes) {
              pos = -1;
              if (pos < 0 && videoState->video_stream >= 0)
                pos = frame_queue_last_pos (&videoState->pictq);
              if (pos < 0 && videoState->audio_stream >= 0)
                pos = frame_queue_last_pos (&videoState->sampq);
              if (pos < 0)
                pos = avio_tell (videoState->ic->pb);
              if (videoState->ic->bit_rate)
                incr *= videoState->ic->bit_rate / 8.0;
              else
                incr *= 180000.0;
              pos += incr;
              stream_seek (videoState, pos, incr, 1);
              }
            else {
              pos = get_master_clock (videoState);
              if (isnan(pos))
                 pos = (double)videoState->seek_pos / AV_TIME_BASE;
              pos += incr;
              if (videoState->ic->start_time != AV_NOPTS_VALUE && pos < videoState->ic->start_time / (double)AV_TIME_BASE)
                pos = videoState->ic->start_time / (double)AV_TIME_BASE;
              stream_seek (videoState, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
              }

            break;
          //}}}
          //{{{
          default:
            break;
          //}}}
          }

        break;
      //}}}
      //{{{
      case SDL_MOUSEBUTTONDOWN:
        if (exit_on_mousedown) {
          do_exit (videoState);
          break;
          }

        if (event.button.button == SDL_BUTTON_LEFT) {
          static int64_t last_mouse_left_click = 0;
          if (av_gettime_relative() - last_mouse_left_click <= 500000) {
            toggle_full_screen (videoState);
            videoState->force_refresh = 1;
            last_mouse_left_click = 0;
            }
          else
            last_mouse_left_click = av_gettime_relative();
          }
        break;
      //}}}
      //{{{
      case SDL_MOUSEMOTION:
        if (cursor_hidden) {
          SDL_ShowCursor (1);
          cursor_hidden = 0;
          }

        cursor_last_shown = av_gettime_relative();
        if (event.type == SDL_MOUSEBUTTONDOWN) {
          if (event.button.button != SDL_BUTTON_RIGHT)
            break;
          x = event.button.x;
          }
        else {
          if (!(event.motion.state & SDL_BUTTON_RMASK))
            break;
          x = event.motion.x;
          }

        if (seek_by_bytes || videoState->ic->duration <= 0) {
          uint64_t size =  avio_size(videoState->ic->pb);
          stream_seek (videoState, size*x/videoState->width, 0, 1);
          }
        else {
          int64_t ts;
          int ns, hh, mm, ss;
          int tns, thh, tmm, tss;
          tns  = videoState->ic->duration / 1000000LL;
          thh  = tns / 3600;
          tmm  = (tns % 3600) / 60;
          tss  = (tns % 60);
          frac = x / videoState->width;
          ns   = frac * tns;
          hh   = ns / 3600;
          mm   = (ns % 3600) / 60;
          ss   = (ns % 60);
          av_log (NULL, AV_LOG_INFO,
                 "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
                  hh, mm, ss, thh, tmm, tss);
          ts = frac * videoState->ic->duration;
          if (videoState->ic->start_time != AV_NOPTS_VALUE)
            ts += videoState->ic->start_time;
          stream_seek (videoState, ts, 0, 0);
          }

        break;
      //}}}
      //{{{
      case SDL_WINDOWEVENT:
        switch (event.window.event) {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
            screen_width  = videoState->width  = event.window.data1;
            screen_height = videoState->height = event.window.data2;
             if (videoState->vis_texture) {
               SDL_DestroyTexture (videoState->vis_texture);
                videoState->vis_texture = NULL;
                }
            videoState->force_refresh = 1;
            break;

          case SDL_WINDOWEVENT_EXPOSED:
            videoState->force_refresh = 1;
            break;
          default:;
          }

        break;
      //}}}
      case SDL_QUIT:
      //{{{
      case FF_QUIT_EVENT:
        do_exit (videoState);
        break;
      //}}}
      default:
        break;
      }
    }
  }
//}}}

//{{{  options
static int dummy;
//{{{
static int opt_add_vfilter (void* optctx, const char* opt, const char* arg) {

  int ret = GROW_ARRAY (vfilters_list, nb_vfilters);
  if (ret < 0)
    return ret;

  vfilters_list[nb_vfilters - 1] = arg;
  return 0;
  }
//}}}
//{{{
static int opt_width (void* optctx, const char* opt, const char* arg) {

  double num;
  int ret = parse_number (opt, arg, OPT_INT64, 1, INT_MAX, &num);
  if (ret < 0)
    return ret;

  screen_width = num;
  return 0;
  }
//}}}
//{{{
static int opt_height (void* optctx, const char* opt, const char* arg) {

  double num;
  int ret = parse_number (opt, arg, OPT_INT64, 1, INT_MAX, &num);
  if (ret < 0)
    return ret;

  screen_height = num;
  return 0;
  }
//}}}
//{{{
static int opt_format (void* optctx, const char* opt, const char* arg) {

  file_iformat = av_find_input_format(arg);
  if (!file_iformat) {
    av_log (NULL, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
    return AVERROR(EINVAL);
    }

  return 0;
  }
//}}}
//{{{
static int opt_sync (void* optctx, const char* opt, const char* arg) {

  if (!strcmp(arg, "audio"))
    av_sync_type = AV_SYNC_AUDIO_MASTER;
  else if (!strcmp(arg, "video"))
    av_sync_type = AV_SYNC_VIDEO_MASTER;
  else if (!strcmp(arg, "ext"))
    av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
  else {
    av_log (NULL, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
    exit (1);
    }

  return 0;
  }
//}}}
//{{{
static int opt_show_mode (void* optctx, const char* opt, const char* arg) {

  show_mode = !strcmp (arg, "video") ? SHOW_MODE_VIDEO :
              !strcmp (arg, "waves") ? SHOW_MODE_WAVES :
              !strcmp (arg, "rdft" ) ? SHOW_MODE_RDFT  : SHOW_MODE_NONE;

  if (show_mode == SHOW_MODE_NONE) {
    double num;
    int ret = parse_number (opt, arg, OPT_INT, 0, SHOW_MODE_NB-1, &num);
    if (ret < 0)
      return ret;
    show_mode = num;
    }

  return 0;
  }
//}}}
//{{{
static int opt_input_file (void* optctx, const char* filename) {

  if (input_filename) {
    av_log (NULL, AV_LOG_FATAL,
            "Argument '%s' provided as input filename, but '%s' was already specified.\n", filename, input_filename);
    return AVERROR(EINVAL);
    }

  if (!strcmp(filename, "-"))
    filename = "fd:";
  input_filename = filename;

  return 0;
  }
//}}}
//{{{
static int opt_codec (void* optctx, const char* opt, const char* arg) {

 const char* spec = strchr(opt, ':');
 if (!spec) {
   av_log (NULL, AV_LOG_ERROR, "No media specifier was specified in '%s' in option '%s'\n", arg, opt);
   return AVERROR(EINVAL);
   }

 spec++;
 switch (spec[0]) {
   case 'a' :
     audio_codec_name = arg;
     break;
   case 's' :
     subtitle_codec_name = arg;
     break;
   case 'v' :
     video_codec_name = arg;
     break;
   default:
     av_log (NULL, AV_LOG_ERROR, "Invalid media specifier '%s' in option '%s'\n", spec, opt);
     return AVERROR(EINVAL);
   }

  return 0;
  }
//}}}

//{{{
static const OptionDef options[] = {
  CMDUTILS_COMMON_OPTIONS
  { "x", HAS_ARG, { .func_arg = opt_width }, "force displayed width", "width" },
  { "y", HAS_ARG, { .func_arg = opt_height }, "force displayed height", "height" },
  { "fs", OPT_BOOL, { &is_full_screen }, "force full screen" },
  { "an", OPT_BOOL, { &audio_disable }, "disable audio" },
  { "vn", OPT_BOOL, { &video_disable }, "disable video" },
  { "sn", OPT_BOOL, { &subtitle_disable }, "disable subtitling" },
  { "ast", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_AUDIO] }, "select desired audio stream", "stream_specifier" },
  { "vst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_VIDEO] }, "select desired video stream", "stream_specifier" },
  { "sst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE] }, "select desired subtitle stream", "stream_specifier" },
  { "ss", HAS_ARG | OPT_TIME, { &start_time }, "seek to a given position in seconds", "pos" },
  { "t",  HAS_ARG | OPT_TIME, { &duration }, "play  \"duration\" seconds of audio/video", "duration" },
  { "bytes", OPT_INT | HAS_ARG, { &seek_by_bytes }, "seek by bytes 0=off 1=on -1=auto", "val" },
  { "seek_interval", OPT_FLOAT | HAS_ARG, { &seek_interval }, "set seek interval for left/right keys, in seconds", "seconds" },
  { "nodisp", OPT_BOOL, { &display_disable }, "disable graphical display" },
  { "noborder", OPT_BOOL, { &borderless }, "borderless window" },
  { "alwaysontop", OPT_BOOL, { &alwaysontop }, "window always on top" },
  { "volume", OPT_INT | HAS_ARG, { &startup_volume}, "set startup volume 0=min 100=max", "volume" },
  { "f", HAS_ARG, { .func_arg = opt_format }, "force format", "fmt" },
  { "stats", OPT_BOOL | OPT_EXPERT, { &show_status }, "show status", "" },
  { "fast", OPT_BOOL | OPT_EXPERT, { &fast }, "non spec compliant optimizations", "" },
  { "genpts", OPT_BOOL | OPT_EXPERT, { &genpts }, "generate pts", "" },
  { "drp", OPT_INT | HAS_ARG | OPT_EXPERT, { &decoder_reorder_pts }, "let decoder reorder pts 0=off 1=on -1=auto", ""},
  { "lowres", OPT_INT | HAS_ARG | OPT_EXPERT, { &lowres }, "", "" },
  { "sync", HAS_ARG | OPT_EXPERT, { .func_arg = opt_sync }, "set audio-video sync. type (type=audio/video/ext)", "type" },
  { "autoexit", OPT_BOOL | OPT_EXPERT, { &autoexit }, "exit at the end", "" },
  { "exitonkeydown", OPT_BOOL | OPT_EXPERT, { &exit_on_keydown }, "exit on key down", "" },
  { "exitonmousedown", OPT_BOOL | OPT_EXPERT, { &exit_on_mousedown }, "exit on mouse down", "" },
  { "loop", OPT_INT | HAS_ARG | OPT_EXPERT, { &loop }, "set number of times the playback shall be looped", "loop count" },
  { "framedrop", OPT_BOOL | OPT_EXPERT, { &framedrop }, "drop frames when cpu is too slow", "" },
  { "infbuf", OPT_BOOL | OPT_EXPERT, { &infinite_buffer }, "don't limit the input buffer size (useful with realtime streams)", "" },
  { "window_title", OPT_STRING | HAS_ARG, { &window_title }, "set window title", "window title" },
  { "left", OPT_INT | HAS_ARG | OPT_EXPERT, { &screen_left }, "set the x position for the left of the window", "x pos" },
  { "top", OPT_INT | HAS_ARG | OPT_EXPERT, { &screen_top }, "set the y position for the top of the window", "y pos" },
  { "vf", OPT_EXPERT | HAS_ARG, { .func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },
  { "af", OPT_STRING | HAS_ARG, { &afilters }, "set audio filters", "filter_graph" },
  { "rdftspeed", OPT_INT | HAS_ARG| OPT_AUDIO | OPT_EXPERT, { &rdftspeed }, "rdft speed", "msecs" },
  { "showmode", HAS_ARG, { .func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
  { "i", OPT_BOOL, { &dummy}, "read specified file", "input_file"},
  { "codec", HAS_ARG, { .func_arg = opt_codec}, "force decoder", "decoder_name" },
  { "acodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &audio_codec_name }, "force audio decoder",    "decoder_name" },
  { "scodec", HAS_ARG | OPT_STRING | OPT_EXPERT, { &subtitle_codec_name }, "force subtitle decoder", "decoder_name" },
  { "vcodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &video_codec_name }, "force video decoder",    "decoder_name" },
  { "autorotate", OPT_BOOL, { &autorotate }, "automatically rotate video", "" },
  { "find_stream_info", OPT_BOOL | OPT_INPUT | OPT_EXPERT, { &find_stream_info },
      "read and decode the streams to fill missing information with heuristics" },
  { "filter_threads", HAS_ARG | OPT_INT | OPT_EXPERT, { &filter_nbthreads }, "number of filter threads per graph" },
  { NULL, },
  };
//}}}
//{{{
static void show_usage() {

  av_log (NULL, AV_LOG_INFO, "Simple media player\n");
  av_log (NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
  av_log (NULL, AV_LOG_INFO, "\n");
  }
//}}}
//{{{
void show_help_default (const char* opt, const char* arg) {

  av_log_set_callback (log_callback_help);

  show_usage();
  show_help_options (options, "Main options:", 0, OPT_EXPERT, 0);
  show_help_options (options, "Advanced options:", OPT_EXPERT, 0, 0);
  printf ("\n");

  show_help_children (avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
  show_help_children (avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
  show_help_children (avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
  printf ("\nWhile playing:\n"
          "q, ESC              quit\n"
          "f                   toggle full screen\n"
          "p, SPC              pause\n"
          "m                   toggle mute\n"
          "9, 0                decrease and increase volume respectively\n"
          "/, *                decrease and increase volume respectively\n"
          "a                   cycle audio channel in the current program\n"
          "v                   cycle video channel\n"
          "t                   cycle subtitle channel in the current program\n"
          "c                   cycle program\n"
          "w                   cycle video filters or show modes\n"
          "s                   activate frame-step mode\n"
          "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
          "down/up             seek backward/forward 1 minute\n"
          "page down/page up   seek backward/forward 10 minutes\n"
          "right mouse click   seek to percentage in file corresponding to fraction of width\n"
          "left double-click   toggle full screen\n"
          );
  }
//}}}
//}}}
//{{{
static void sigterm_handler (int sig) {
  exit (123);
  }
//}}}
//{{{
/* Called from the main */
int main (int argc, char** argv) {

  init_dynload();

  av_log_set_flags (AV_LOG_SKIP_REPEATED);
  parse_loglevel (argc, argv, options);

  /* register all codecs, demux and protocols */
  #if CONFIG_AVDEVICE
    avdevice_register_all();
  #endif
  avformat_network_init();

  signal (SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
  signal (SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  show_banner (argc, argv, options);

  int ret = parse_options (NULL, argc, argv, options, opt_input_file);
  if (ret < 0)
    exit (ret == AVERROR_EXIT ? 0 : 1);

  if (!input_filename) {
    //{{{  error
    show_usage();
    av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
    av_log(NULL, AV_LOG_FATAL,
           "Use -h to get full help or, even better, run 'man %s'\n", program_name);
    exit(1);
    }
    //}}}

  if (display_disable)
    video_disable = 1;
  int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;

  if (audio_disable)
    flags &= ~SDL_INIT_AUDIO;
  else {
    //{{{  alsa buffer underflow
    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv ("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
       SDL_setenv ("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }
    //}}}

  if (display_disable)
    flags &= ~SDL_INIT_VIDEO;

  if (SDL_Init (flags)) {
    av_log (NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
    av_log (NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
    exit (1);
    }

  SDL_EventState (SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState (SDL_USEREVENT, SDL_IGNORE);

  if (!display_disable) {
    //{{{  show window
    int flags = SDL_WINDOW_HIDDEN;

    if (alwaysontop)
      flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    if (borderless)
      flags |= SDL_WINDOW_BORDERLESS;
    else
      flags |= SDL_WINDOW_RESIZABLE;

      #ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
        SDL_SetHint (SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
      #endif

    window = SDL_CreateWindow (program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
    SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
      renderer = SDL_CreateRenderer (window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

      if (!renderer) {
        av_log (NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
        renderer = SDL_CreateRenderer (window, -1, 0);
        }

      if (renderer) {
        if (!SDL_GetRendererInfo (renderer, &renderer_info))
          av_log (NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
      }

    if (!window || !renderer || !renderer_info.num_texture_formats) {
      av_log (NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
      do_exit (NULL);
      }
    }
    //}}}

  VideoState* videoState = stream_open (input_filename, file_iformat);
  if (!videoState) {
    av_log (NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
    do_exit (NULL);
    }

  eventLoop (videoState);

  /* never returns */
  return 0;
  }
//}}}
