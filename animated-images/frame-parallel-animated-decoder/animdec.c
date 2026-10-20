/* cutepianimdec: animated (and still) images GStreamer cannot play here —
 * animated WebP, APNG, AVIF (stills and sequences) and JPEG XL (stills and
 * animations) — decoded from the whole file into straight-alpha RGBA frames at
 * the file's own frame timing (DESIGN §6.1.3, §12.16).
 *
 * GStreamer 1.26 on Debian trixie has webpdec for WebP stills only, no APNG
 * demuxer (pngdec shows the first frame; gst-libav exposes no avdemux_apng),
 * no AVIF or JPEG XL decoder, and FFmpeg 7.1 cannot decode animated WebP. The
 * libraries are on the system as runtime packages (libwebpdemux2, libavif16,
 * libjxl0.11) without headers, so this element loads them at run time (dlopen)
 * against vendored headers of the same versions, as cutepidav1ddec does. APNG
 * needs no library: its frames are rebuilt as single PNGs and decoded by
 * GStreamer's own pngdec, then composited here (dispose and blend operations).
 *
 * The file is collected until upstream's EOS; frames are then decoded and pushed
 * in order on the source pad's task with their timestamps and durations. In WebP
 * and APNG every frame's compressed data is a complete image of its region (only
 * the compositing depends on the frames before), so WORKERS threads decode frames
 * ahead into a ring of patches and the task composites them in order (dispose,
 * blend): a 1080p30 WebP decoded at 12 fps serially (libwebp's WebPAnimDecoder,
 * plus a pass to find the duration). AVIF and JPEG XL frames depend on earlier
 * ones; their libraries decode them in order with their own threads.
 * TIME seeks (cue loops seek to 0 with FLUSH) restart the decode and skip to the
 * frame showing at the target time. One frame is one buffer from a pool. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include <avif/avif.h>
#include <dav1d/dav1d.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <webp/demux.h>

GST_DEBUG_CATEGORY_STATIC (animdec_debug);
#define GST_CAT_DEFAULT animdec_debug

#define MAX_FILE (512u << 20) /* refuse more than 512 MB of image file */

/* libpng 1.6's simplified API (png.h: png_image, PNG_IMAGE_VERSION 1); the
 * runtime package has no headers, and this public struct is all it needs. */
typedef struct {
  void *opaque;
  guint32 version, width, height, format, flags, colormap_entries, warning_or_error;
  char message[64];
} cutepi_png_image;
#define CUTEPI_PNG_IMAGE_VERSION 1
#define CUTEPI_PNG_FORMAT_RGBA 3 /* PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_ALPHA: straight alpha */

typedef enum { FMT_NONE, FMT_WEBP, FMT_APNG, FMT_AVIF, FMT_JXL, FMT_GIF } anim_fmt;

/* ---- run-time libraries ------------------------------------------------- */

static struct {
  int webp, avif, jxl, png; /* loaded */
  /* libpng16 */
  int (*png_begin) (cutepi_png_image *, const void *, size_t);
  int (*png_finish) (cutepi_png_image *, const void *, void *, gint32, void *);
  void (*png_free) (cutepi_png_image *);
  /* libdav1d (AVIF image sequences) and libavif's image helpers */
  int dav1d;
  void (*d_defaults) (Dav1dSettings *);
  int (*d_open) (Dav1dContext **, const Dav1dSettings *);
  int (*d_send) (Dav1dContext *, Dav1dData *);
  int (*d_get) (Dav1dContext *, Dav1dPicture *);
  void (*d_close) (Dav1dContext **);
  void (*d_flush) (Dav1dContext *);
  int (*d_wrap) (Dav1dData *, const uint8_t *, size_t, void (*) (const uint8_t *, void *), void *);
  void (*d_data_unref) (Dav1dData *);
  void (*d_pic_unref) (Dav1dPicture *);
  avifImage *(*avif_image_empty) (void);
  void (*avif_image_destroy) (avifImage *);
  /* libwebpdemux, libwebp */
  WebPDemuxer *(*webp_demux) (const WebPData *, int, WebPDemuxState *, int);
  void (*webp_demux_delete) (WebPDemuxer *);
  uint32_t (*webp_demux_get) (const WebPDemuxer *, WebPFormatFeature);
  int (*webp_get_frame) (const WebPDemuxer *, int, WebPIterator *);
  void (*webp_release_iter) (WebPIterator *);
  uint8_t *(*webp_decode_rgba_into) (const uint8_t *, size_t, uint8_t *, size_t, int);
  /* libavif */
  avifDecoder *(*avif_create) (void);
  void (*avif_destroy) (avifDecoder *);
  avifResult (*avif_set_mem) (avifDecoder *, const uint8_t *, size_t);
  avifResult (*avif_parse) (avifDecoder *);
  avifResult (*avif_next) (avifDecoder *);
  avifResult (*avif_reset) (avifDecoder *);
  void (*avif_rgb_defaults) (avifRGBImage *, const avifImage *);
  avifResult (*avif_yuv_to_rgb) (const avifImage *, avifRGBImage *);
  const char *(*avif_result_string) (avifResult);
  /* libjxl, libjxl_threads */
  JxlDecoder *(*jxl_create) (const JxlMemoryManager *);
  void (*jxl_destroy) (JxlDecoder *);
  void (*jxl_rewind) (JxlDecoder *);
  JxlDecoderStatus (*jxl_subscribe) (JxlDecoder *, int);
  JxlDecoderStatus (*jxl_set_input) (JxlDecoder *, const uint8_t *, size_t);
  void (*jxl_close_input) (JxlDecoder *);
  JxlDecoderStatus (*jxl_process) (JxlDecoder *);
  JxlDecoderStatus (*jxl_basic_info) (const JxlDecoder *, JxlBasicInfo *);
  JxlDecoderStatus (*jxl_frame_header) (const JxlDecoder *, JxlFrameHeader *);
  JxlDecoderStatus (*jxl_out_size) (const JxlDecoder *, const JxlPixelFormat *, size_t *);
  JxlDecoderStatus (*jxl_set_out) (JxlDecoder *, const JxlPixelFormat *, void *, size_t);
  JxlDecoderStatus (*jxl_set_runner) (JxlDecoder *, JxlParallelRunner, void *);
  void *(*jxl_runner_create) (const JxlMemoryManager *, size_t);
  void (*jxl_runner_destroy) (void *);
  JxlParallelRetCode (*jxl_runner) (void *, void *, JxlParallelRunInit, JxlParallelRunFunction, uint32_t, uint32_t);
} L;

#define SYM(h, field, name) (*(void **) &L.field = dlsym (h, name))

static void
load_libs (void)
{
  void *h;
  void *w;
  if ((h = dlopen ("libwebpdemux.so.2", RTLD_NOW | RTLD_LOCAL)) && (w = dlopen ("libwebp.so.7", RTLD_NOW | RTLD_LOCAL)) &&
      SYM (h, webp_demux, "WebPDemuxInternal") && SYM (h, webp_demux_delete, "WebPDemuxDelete") &&
      SYM (h, webp_demux_get, "WebPDemuxGetI") && SYM (h, webp_get_frame, "WebPDemuxGetFrame") &&
      SYM (h, webp_release_iter, "WebPDemuxReleaseIterator") && SYM (w, webp_decode_rgba_into, "WebPDecodeRGBAInto"))
    L.webp = 1;
  if ((h = dlopen ("libavif.so.16", RTLD_NOW | RTLD_LOCAL)) &&
      SYM (h, avif_create, "avifDecoderCreate") && SYM (h, avif_destroy, "avifDecoderDestroy") &&
      SYM (h, avif_set_mem, "avifDecoderSetIOMemory") && SYM (h, avif_parse, "avifDecoderParse") &&
      SYM (h, avif_next, "avifDecoderNextImage") && SYM (h, avif_reset, "avifDecoderReset") &&
      SYM (h, avif_rgb_defaults, "avifRGBImageSetDefaults") && SYM (h, avif_yuv_to_rgb, "avifImageYUVToRGB") &&
      SYM (h, avif_result_string, "avifResultToString"))
    L.avif = 1;
  if (L.avif && (h = dlopen ("libdav1d.so.7", RTLD_NOW | RTLD_LOCAL)) &&
      SYM (h, d_defaults, "dav1d_default_settings") && SYM (h, d_open, "dav1d_open") &&
      SYM (h, d_send, "dav1d_send_data") && SYM (h, d_get, "dav1d_get_picture") && SYM (h, d_close, "dav1d_close") &&
      SYM (h, d_flush, "dav1d_flush") && SYM (h, d_wrap, "dav1d_data_wrap") && SYM (h, d_data_unref, "dav1d_data_unref") &&
      SYM (h, d_pic_unref, "dav1d_picture_unref")) {
    void *a = dlopen ("libavif.so.16", RTLD_NOW | RTLD_LOCAL);
    if (a && SYM (a, avif_image_empty, "avifImageCreateEmpty") && SYM (a, avif_image_destroy, "avifImageDestroy"))
      L.dav1d = 1;
  }
  if ((h = dlopen ("libpng16.so.16", RTLD_NOW | RTLD_LOCAL)) &&
      SYM (h, png_begin, "png_image_begin_read_from_memory") && SYM (h, png_finish, "png_image_finish_read") &&
      SYM (h, png_free, "png_image_free"))
    L.png = 1;
  void *t = NULL;
  if ((h = dlopen ("libjxl.so.0.11", RTLD_NOW | RTLD_LOCAL)) &&
      SYM (h, jxl_create, "JxlDecoderCreate") && SYM (h, jxl_destroy, "JxlDecoderDestroy") &&
      SYM (h, jxl_rewind, "JxlDecoderRewind") && SYM (h, jxl_subscribe, "JxlDecoderSubscribeEvents") &&
      SYM (h, jxl_set_input, "JxlDecoderSetInput") && SYM (h, jxl_close_input, "JxlDecoderCloseInput") &&
      SYM (h, jxl_process, "JxlDecoderProcessInput") && SYM (h, jxl_basic_info, "JxlDecoderGetBasicInfo") &&
      SYM (h, jxl_frame_header, "JxlDecoderGetFrameHeader") && SYM (h, jxl_out_size, "JxlDecoderImageOutBufferSize") &&
      SYM (h, jxl_set_out, "JxlDecoderSetImageOutBuffer") && SYM (h, jxl_set_runner, "JxlDecoderSetParallelRunner")) {
    L.jxl = 1;
    if ((t = dlopen ("libjxl_threads.so.0.11", RTLD_NOW | RTLD_LOCAL)) &&
        SYM (t, jxl_runner_create, "JxlThreadParallelRunnerCreate") &&
        SYM (t, jxl_runner_destroy, "JxlThreadParallelRunnerDestroy") && SYM (t, jxl_runner, "JxlThreadParallelRunner"))
      ;
    else
      L.jxl_runner_create = NULL;
  }
}

/* ---- type finding ---------------------------------------------------------- */

static GstStaticCaps apng_caps = GST_STATIC_CAPS ("video/x-apng");
static GstStaticCaps avif_caps = GST_STATIC_CAPS ("image/avif");
static GstStaticCaps jxl_caps = GST_STATIC_CAPS ("image/jxl");

/* A PNG with an acTL chunk before its first IDAT is an APNG; a plain PNG is
 * left to the png typefinder (stills keep pngdec). */
static void
apng_typefind (GstTypeFind * tf, gpointer d)
{
  static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
  const guint8 *p = gst_type_find_peek (tf, 0, 8);
  if (!p || memcmp (p, sig, 8)) return;
  guint64 off = 8;
  for (int n = 0; n < 64; n++) {
    const guint8 *c = gst_type_find_peek (tf, off, 8);
    if (!c) return;
    guint32 len = GST_READ_UINT32_BE (c);
    if (!memcmp (c + 4, "acTL", 4)) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, gst_static_caps_get (&apng_caps));
      return;
    }
    if (!memcmp (c + 4, "IDAT", 4) || !memcmp (c + 4, "IEND", 4)) return;
    off += 12 + (guint64) len;
  }
}

/* ISOBMFF with an AVIF brand (major or compatible): avif (still), avis (sequence). */
static void
avif_typefind (GstTypeFind * tf, gpointer d)
{
  const guint8 *p = gst_type_find_peek (tf, 0, 12);
  if (!p || memcmp (p + 4, "ftyp", 4)) return;
  guint32 size = GST_READ_UINT32_BE (p);
  if (size < 16 || size > 256) return;
  const guint8 *b = gst_type_find_peek (tf, 8, size - 8);
  if (!b) return;
  for (guint32 i = 0; i + 4 <= size - 8; i += 4) {
    if (i == 4) continue; /* minor version */
    if (!memcmp (b + i, "avif", 4) || !memcmp (b + i, "avis", 4)) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, gst_static_caps_get (&avif_caps));
      return;
    }
  }
}

/* JPEG XL: a bare codestream (FF 0A) or the ISOBMFF-style container. */
static void
jxl_typefind (GstTypeFind * tf, gpointer d)
{
  static const guint8 box[12] = { 0, 0, 0, 0x0c, 'J', 'X', 'L', ' ', 0x0d, 0x0a, 0x87, 0x0a };
  const guint8 *p = gst_type_find_peek (tf, 0, 12);
  if (p && !memcmp (p, box, 12)) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, gst_static_caps_get (&jxl_caps));
    return;
  }
  p = gst_type_find_peek (tf, 0, 2);
  if (p && p[0] == 0xff && p[1] == 0x0a)
    gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, gst_static_caps_get (&jxl_caps));
}


/* ---- the element --------------------------------------------------------------- */

#define WORKERS 4
#define RING 8 /* frames decoded ahead: patches of up to the canvas size */

/* One frame of a WebP or APNG animation: its region, timing, operations and
 * where its compressed data is. */
typedef struct {
  guint32 x, y, w, h; /* the region shown (clipped to the canvas) */
  guint32 sw, sh;     /* the frame's own size (its decoded patch is sw x sh) */
  GstClockTime dur;
  int dispose; /* 0 none, 1 background (transparent), 2 previous (APNG) */
  int blend;   /* 0 source (replace), 1 over (straight alpha) */
  gsize off, len; /* WebP: the frame's bitstream (ALPH + VP8/VP8L) */
  GArray *data;   /* APNG: (offset, length) pairs of its IDAT / fdAT payloads */
  /* GIF: off = the LZW minimum code size byte, then the data sub-blocks */
  gsize pal_off;  /* its colour table (local, else global) */
  int pal_n, trans, interlace; /* entries; transparent index or -1 */
} frame_info;

typedef struct {
  int idx;      /* frame index held, -1 free */
  int state;    /* 0 free, 1 decoding, 2 ready, -1 failed */
  guint gen;    /* decode generation it belongs to */
  guint8 *px;   /* RGBA, frame w x h, stride w*4 */
} slot;

typedef struct _CutepiAnimDec CutepiAnimDec;

typedef struct {
  CutepiAnimDec *s;
  GThread *thread;
  GstElement *png_pipe, *png_src, *png_sink; /* APNG: this worker's frame decoder */
} worker;

struct _CutepiAnimDec {
  GstElement parent;
  GstPad *sinkpad, *srcpad;
  GByteArray *data;      /* the whole file */
  gboolean have_eos;     /* the file is complete */
  anim_fmt fmt;
  int w, h;
  guint loops;           /* 0 = forever (informational) */
  GstClockTime duration; /* whole animation, or NONE */
  guint nframes;         /* 0 = unknown */
  GstBufferPool *pool;
  GstSegment segment;
  GstClockTime seek_to;  /* skip frames ending before this */
  gboolean need_segment, need_caps;
  GstClockTime pts;      /* next frame's timestamp */
  /* WebP / APNG: frame table, decode workers, compositor */
  GArray *frames;        /* frame_info */
  GByteArray *apng_head; /* chunks every rebuilt frame PNG carries before IDAT */
  slot ring[RING];
  worker workers[WORKERS];
  GMutex lock;
  GCond cond;
  int next_decode, next_show;
  guint gen;
  gboolean quit;
  /* No canvas of its own: each frame is built in its output buffer, from the
   * last one pushed (kept mapped) when it does not replace the whole picture.
   * That saves a full-frame copy a frame (two for whole-picture frames). */
  GstBuffer *last;        /* the last frame pushed, mapped read-only */
  GstMapInfo last_map;
  const guint8 *last_px;  /* its pixels and stride */
  int last_stride;
  guint8 *saved;          /* RGBA w*h: the picture before a dispose-previous frame */
  int prev;               /* the frame shown last, -1 none */
  /* AVIF image sequence through dav1d: colour track, optional alpha track */
  struct av1_track { GArray *samples; Dav1dContext *ctx; guint next; Dav1dData pending; } seq[2];
  gboolean avis;          /* decoding the sequence directly (not libavif) */
  guint avis_shown;       /* pictures output since the start */
  /* AVIF / JPEG XL */
  avifDecoder *avif;
  JxlDecoder *jxl;
  void *jxl_runner;
  JxlPixelFormat jxl_fmt;
  double jxl_tick; /* seconds per tick */
};

typedef struct {
  GstElementClass parent_class;
} CutepiAnimDecClass;

GType cutepi_anim_dec_get_type (void);
G_DEFINE_TYPE (CutepiAnimDec, cutepi_anim_dec, GST_TYPE_ELEMENT);

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/gif; image/webp; video/x-apng; image/avif; image/jxl"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string)RGBA, width=(int)[1, 32768], height=(int)[1, 32768], "
        "framerate=(fraction)[0/1, 1000/1]"));

/* ---- PNG chunks (APNG) --------------------------------------------------------- */

static gboolean
png_chunk (const GByteArray * d, gsize pos, guint32 * len, const guint8 ** type, const guint8 ** body)
{
  if (pos + 12 > d->len) return FALSE;
  *len = GST_READ_UINT32_BE (d->data + pos);
  if (*len > d->len - pos - 12) return FALSE;
  *type = d->data + pos + 4;
  *body = d->data + pos + 8;
  return TRUE;
}

static void
append_chunk (GByteArray * a, const char *type, const guint8 * body, guint32 len)
{
  static guint32 table[256];
  static gsize once;
  if (g_once_init_enter (&once)) {
    for (guint32 n = 0; n < 256; n++) {
      guint32 c = n;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    g_once_init_leave (&once, 1);
  }
  guint8 hdr[8];
  GST_WRITE_UINT32_BE (hdr, len);
  memcpy (hdr + 4, type, 4);
  g_byte_array_append (a, hdr, 8);
  if (len) g_byte_array_append (a, body, len);
  guint32 c = 0xffffffffu;
  for (int i = 0; i < 4; i++) c = table[(c ^ (guint8) type[i]) & 0xff] ^ (c >> 8);
  for (guint32 i = 0; i < len; i++) c = table[(c ^ body[i]) & 0xff] ^ (c >> 8);
  guint8 crc[4];
  GST_WRITE_UINT32_BE (crc, c ^ 0xffffffffu);
  g_byte_array_append (a, crc, 4);
}

/* APNG frame i as a single PNG, decoded by this worker's pngdec into px (w*4 stride). */
static gboolean
apng_decode (worker * wk, const frame_info * f, guint8 * px)
{
  CutepiAnimDec *s = wk->s;
  if (!L.png && !wk->png_pipe) { /* fallback: libpng16 could not be loaded */
    GError *err = NULL;
    /* RGB passes through (its expansion to RGBA is done below, in the copy
     * this decode needs anyway): videoconvert's RGB -> RGBA cost 9.3 ms a
     * 1080p frame against pngdec's 17.6. */
    wk->png_pipe = gst_parse_launch ("appsrc name=s format=time ! pngdec ! videoconvert ! "
        "video/x-raw,format={RGBA,RGB} ! appsink name=k sync=false", &err);
    if (!wk->png_pipe) {
      GST_ERROR_OBJECT (s, "APNG frame decoder: %s", err ? err->message : "?");
      g_clear_error (&err);
      return FALSE;
    }
    wk->png_src = gst_bin_get_by_name (GST_BIN (wk->png_pipe), "s");
    wk->png_sink = gst_bin_get_by_name (GST_BIN (wk->png_pipe), "k");
    gst_object_unref (wk->png_src); /* the bin holds them */
    gst_object_unref (wk->png_sink);
    gst_element_set_state (wk->png_pipe, GST_STATE_PLAYING);
  }
  GByteArray *png = g_byte_array_sized_new (64 + s->apng_head->len + 16);
  static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
  g_byte_array_append (png, sig, 8);
  guint8 ihdr[13];
  memcpy (ihdr, s->data->data + 16, 13);
  GST_WRITE_UINT32_BE (ihdr, f->w);
  GST_WRITE_UINT32_BE (ihdr + 4, f->h);
  append_chunk (png, "IHDR", ihdr, 13);
  g_byte_array_append (png, s->apng_head->data, s->apng_head->len);
  for (guint i = 0; i < f->data->len; i += 2)
    append_chunk (png, "IDAT", s->data->data + g_array_index (f->data, gsize, i), g_array_index (f->data, gsize, i + 1));
  append_chunk (png, "IEND", NULL, 0);
  if (L.png) {
    /* libpng straight into the slot (any colour type, palette transparency
     * and 16-bit to 8-bit RGBA): no internal pipeline, no extra copy. */
    cutepi_png_image im = { 0 };
    im.version = CUTEPI_PNG_IMAGE_VERSION;
    gboolean ok = L.png_begin (&im, png->data, png->len) && im.width == f->w && im.height == f->h;
    if (ok) {
      im.format = CUTEPI_PNG_FORMAT_RGBA;
      ok = L.png_finish (&im, NULL, px, (gint32) f->w * 4, NULL);
    }
    if (!ok) GST_WARNING_OBJECT (s, "APNG frame: %s", im.message);
    L.png_free (&im);
    g_byte_array_unref (png);
    return ok;
  }
  GstBuffer *b = gst_buffer_new_wrapped_full (0, png->data, png->len, 0, png->len, png, (GDestroyNotify) g_byte_array_unref);
  GstCaps *c = gst_caps_new_simple ("image/png", "width", G_TYPE_INT, f->w, "height", G_TYPE_INT, f->h,
      "framerate", GST_TYPE_FRACTION, 0, 1, NULL);
  GstSample *in = gst_sample_new (b, c, NULL, NULL);
  gst_app_src_push_sample (GST_APP_SRC (wk->png_src), in);
  gst_sample_unref (in);
  gst_buffer_unref (b);
  gst_caps_unref (c);
  GstSample *out = gst_app_sink_try_pull_sample (GST_APP_SINK (wk->png_sink), 10 * GST_SECOND);
  if (!out) return FALSE;
  GstVideoInfo vi; GstVideoFrame fr;
  gboolean ok = gst_video_info_from_caps (&vi, gst_sample_get_caps (out)) && (guint) vi.width == f->w &&
      (guint) vi.height == f->h && gst_video_frame_map (&fr, &vi, gst_sample_get_buffer (out), GST_MAP_READ);
  if (ok) {
    const guint8 *p = GST_VIDEO_FRAME_PLANE_DATA (&fr, 0);
    int st = GST_VIDEO_FRAME_PLANE_STRIDE (&fr, 0);
    gboolean rgb = GST_VIDEO_INFO_FORMAT (&vi) == GST_VIDEO_FORMAT_RGB;
    for (guint32 y = 0; y < f->h; y++) {
      guint8 *d = px + (gsize) y * f->w * 4;
      const guint8 *r = p + (gsize) y * st;
      if (!rgb) { memcpy (d, r, (gsize) f->w * 4); continue; }
      guint32 x = 0;
#ifdef __aarch64__
      uint8x16_t a = vdupq_n_u8 (255);
      for (; x + 16 <= f->w; x += 16) {
        uint8x16x3_t v = vld3q_u8 (r + x * 3);
        uint8x16x4_t o = { { v.val[0], v.val[1], v.val[2], a } };
        vst4q_u8 (d + x * 4, o);
      }
#endif
      for (; x < f->w; x++) {
        d[x * 4] = r[x * 3]; d[x * 4 + 1] = r[x * 3 + 1]; d[x * 4 + 2] = r[x * 3 + 2]; d[x * 4 + 3] = 255;
      }
    }
    gst_video_frame_unmap (&fr);
  }
  gst_sample_unref (out);
  return ok;
}

/* ---- GIF -------------------------------------------------------------------------- */

/* One frame's LZW data (from its sub-blocks) to colour indices in px (sw x sh,
 * one byte a pixel, rows in display order). The compositor maps them through
 * the palette as it draws (gif_composite): handing over 1-byte indices instead
 * of RGBA saves 12 MB of memory traffic a 1080p frame, which held a 50 fps
 * GIF at 48 fps in the service. A short or damaged stream leaves the rest of
 * the frame transparent (or index 0 without transparency), as browsers do. */
static gboolean
gif_decode (CutepiAnimDec * s, const frame_info * f, guint8 * px)
{
  const guint8 *d = s->data->data, *end = d + s->data->len;
  const guint8 *p = d + f->off;
  int min = *p++;
  if (min < 2 || min > 11) return FALSE;
  gsize npx = (gsize) f->sw * f->sh;
  guint8 *idx = f->interlace ? g_malloc (npx) : px; /* stored row order, then de-interlaced */
  memset (idx, f->trans >= 0 ? f->trans : 0, npx);
  /* LZW: codes LSB-first; dictionary as prefix/suffix chains. */
  static __thread guint16 prefix[4096];
  static __thread guint8 suffix[4096], stack[4097];
  int clear = 1 << min, eoi = clear + 1, next = clear + 2, size = min + 1, old = -1;
  guint8 first = 0;
  guint32 acc = 0; int bits = 0;
  gsize out = 0;
  int blk = 0; /* bytes left in this sub-block */
  for (int i = 0; i < clear; i++) { prefix[i] = 0xffff; suffix[i] = i; }
  for (;;) {
    while (bits < size) {
      if (!blk) {
        if (p >= end || !(blk = *p++)) goto done;
      }
      if (p >= end) goto done;
      acc |= (guint32) *p++ << bits;
      bits += 8; blk--;
    }
    int code = acc & ((1 << size) - 1);
    acc >>= size; bits -= size;
    if (code == clear) { next = clear + 2; size = min + 1; old = -1; continue; }
    if (code == eoi) break;
    int sp = 0, c = code;
    if (old < 0) {
      if (code >= clear) break; /* the first code must be a colour */
      first = code;
      if (out < npx) idx[out++] = first;
      old = code;
      continue;
    }
    if (code > next || (code == next && next >= 4096)) break; /* damaged */
    if (code == next) { stack[sp++] = first; c = old; }
    while (c >= clear) {
      if (c >= 4096 || sp >= 4096) goto done;
      stack[sp++] = suffix[c];
      c = prefix[c];
    }
    first = c;
    stack[sp++] = first;
    while (sp && out < npx) idx[out++] = stack[--sp];
    if (next < 4096) {
      prefix[next] = old; suffix[next] = first; next++;
      if (next == (1 << size) && size < 12) size++;
    }
    old = code;
    if (out >= npx) break;
  }
done:;
  if (!f->interlace) return TRUE;
  /* De-interlace: stored rows 0, 8, 16, …, 4, 12, …, 2, 6, …, 1, 3, … */
  static const guint32 start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
  guint32 row = 0;
  for (int pass = 0; pass < 4; pass++)
    for (guint32 y = start[pass]; y < f->sh && row < f->sh; y += step[pass], row++)
      memcpy (px + (gsize) y * f->sw, idx + (gsize) row * f->sw, f->sw);
  g_free (idx);
  return TRUE;
}

/* Draw a GIF frame's indices (w x h of a sw-wide patch) at x0, y0: the
 * transparent index leaves what is there. */
static void
gif_composite (CutepiAnimDec * s, const frame_info * f, guint8 * dst, int dstride, const guint8 * idx)
{
  const guint8 *pal = s->data->data + f->pal_off;
  guint32 lut[256];
  for (int i = 0; i < 256; i++) {
    guint8 c[4] = { 0, 0, 0, 255 };
    if (i < f->pal_n) { c[0] = pal[i * 3]; c[1] = pal[i * 3 + 1]; c[2] = pal[i * 3 + 2]; }
    memcpy (&lut[i], c, 4);
  }
  for (guint32 y = 0; y < f->h; y++) {
    guint32 *d = (guint32 *) (dst + (gsize) (f->y + y) * dstride + (gsize) f->x * 4);
    const guint8 *p = idx + (gsize) y * f->sw;
    if (f->trans < 0) {
      for (guint32 x = 0; x < f->w; x++) d[x] = lut[p[x]];
    } else {
      for (guint32 x = 0; x < f->w; x++)
        if (p[x] != f->trans) d[x] = lut[p[x]];
    }
  }
}

/* Skip a chain of data sub-blocks; the position after its terminator, or 0. */
static gsize
gif_skip_blocks (const GByteArray * a, gsize p)
{
  while (p < a->len) {
    guint8 n = a->data[p];
    if (!n) return p + 1;
    p += 1 + n;
  }
  return 0;
}

/* GIF: frame table from the image descriptors and their graphic control. */
static gboolean
gif_open (CutepiAnimDec * s)
{
  const GByteArray *a = s->data;
  const guint8 *d = a->data;
  if (a->len < 13 || memcmp (d, "GIF8", 4)) return FALSE;
  s->w = GST_READ_UINT16_LE (d + 6); s->h = GST_READ_UINT16_LE (d + 8);
  guint8 flags = d[10];
  gsize p = 13, gpal = 0;
  int gpal_n = 0;
  if (flags & 0x80) { gpal = p; gpal_n = 2 << (flags & 7); p += 3 * gpal_n; }
  s->loops = 1;
  int dispose = 0, trans = -1;
  guint delay = 0;
  GstClockTime total = 0;
  while (p < a->len) {
    guint8 b = d[p++];
    if (b == 0x3b) break; /* trailer */
    if (b == 0x21) {
      if (p + 1 > a->len) break;
      guint8 label = d[p++];
      if (label == 0xf9 && p + 5 <= a->len && d[p] >= 4) {
        guint8 pf = d[p + 1];
        dispose = (pf >> 2) & 7;
        delay = GST_READ_UINT16_LE (d + p + 2);
        trans = pf & 1 ? d[p + 4] : -1;
      } else if (label == 0xff && p + 12 <= a->len && d[p] == 11 && !memcmp (d + p + 1, "NETSCAPE2.0", 11) &&
          p + 16 <= a->len && d[p + 12] >= 3 && d[p + 13] == 1) {
        s->loops = GST_READ_UINT16_LE (d + p + 14); /* 0 = forever */
      }
      if (!(p = gif_skip_blocks (a, p))) break;
      continue;
    }
    if (b != 0x2c || p + 9 > a->len) break;
    frame_info f = { 0 };
    guint32 fx = GST_READ_UINT16_LE (d + p), fy = GST_READ_UINT16_LE (d + p + 2);
    f.sw = GST_READ_UINT16_LE (d + p + 4); f.sh = GST_READ_UINT16_LE (d + p + 6);
    guint8 ff = d[p + 8];
    p += 9;
    f.interlace = (ff & 0x40) != 0;
    if (ff & 0x80) { f.pal_off = p; f.pal_n = 2 << (ff & 7); p += 3 * f.pal_n; }
    else { f.pal_off = gpal; f.pal_n = gpal_n; }
    if (p >= a->len || !f.pal_n) break;
    f.off = p;
    if (!(p = gif_skip_blocks (a, p + 1))) p = a->len; /* a truncated last frame decodes what is there */
    /* Clipped to the canvas, as browsers draw it. */
    f.x = MIN (fx, (guint32) s->w); f.y = MIN (fy, (guint32) s->h);
    f.w = MIN (f.sw, s->w - f.x); f.h = MIN (f.sh, s->h - f.y);
    /* Browsers show a delay of 0 or 1/100 s as 1/10 s. */
    f.dur = (delay < 2 ? 10 : delay) * (GST_SECOND / 100);
    f.dispose = dispose == 2 ? 1 : dispose == 3 ? 2 : 0;
    if (!s->frames->len && f.dispose == 2) f.dispose = 1;
    f.blend = trans >= 0; /* transparent pixels leave the picture as it is; without, the frame replaces its region */
    f.trans = trans;
    if (f.sw && f.sh && f.w && f.h) {
      g_array_append_val (s->frames, f);
      total += f.dur;
    }
    dispose = 0; trans = -1; delay = 0; /* a graphic control applies to one image */
  }
  if (!s->frames->len || s->w <= 0 || s->h <= 0) return FALSE;
  s->nframes = s->frames->len;
  if (s->nframes > 1) s->duration = total;
  return TRUE;
}

/* ---- decode workers and the in-order compositor ------------------------------- */

static gpointer
worker_main (gpointer d)
{
  worker *wk = d;
  CutepiAnimDec *s = wk->s;
  g_mutex_lock (&s->lock);
  for (;;) {
    while (!s->quit && (s->next_decode >= (int) s->frames->len || s->ring[s->next_decode % RING].state != 0))
      g_cond_wait (&s->cond, &s->lock);
    if (s->quit) break;
    int i = s->next_decode++;
    slot *sl = &s->ring[i % RING];
    sl->idx = i; sl->state = 1; sl->gen = s->gen;
    guint gen = s->gen;
    const frame_info *f = &g_array_index (s->frames, frame_info, i);
    g_mutex_unlock (&s->lock);
    gboolean ok;
    if (s->fmt == FMT_WEBP)
      ok = L.webp_decode_rgba_into (s->data->data + f->off, f->len, sl->px, (gsize) f->w * f->h * 4, f->w * 4) != NULL;
    else if (s->fmt == FMT_GIF)
      ok = gif_decode (s, f, sl->px);
    else
      ok = apng_decode (wk, f, sl->px);
    g_mutex_lock (&s->lock);
    if (sl->gen == gen && s->gen == gen) sl->state = ok ? 2 : -1;
    else sl->state = 0; /* reset meanwhile: the slot is free again */
    g_cond_broadcast (&s->cond);
  }
  g_mutex_unlock (&s->lock);
  if (wk->png_pipe) {
    gst_element_set_state (wk->png_pipe, GST_STATE_NULL);
    gst_object_unref (wk->png_pipe);
    wk->png_pipe = NULL;
  }
  return NULL;
}

static void
last_drop (CutepiAnimDec * s)
{
  if (!s->last) return;
  gst_buffer_unmap (s->last, &s->last_map);
  gst_buffer_unref (s->last);
  s->last = NULL;
  s->last_px = NULL;
}

/* Keep b (about to be pushed) as the base of the next frame. */
static void
last_keep (CutepiAnimDec * s, GstBuffer * b)
{
  last_drop (s);
  GstVideoMeta *vm = gst_buffer_get_video_meta (b);
  if (!gst_buffer_map (b, &s->last_map, GST_MAP_READ)) return;
  s->last = gst_buffer_ref (b);
  s->last_px = s->last_map.data + (vm ? vm->offset[0] : 0);
  s->last_stride = vm ? vm->stride[0] : s->w * 4;
}

static void
workers_start (CutepiAnimDec * s)
{
  gsize most = (gsize) s->w * s->h;
  for (guint i = 0; i < s->frames->len; i++) {
    const frame_info *f = &g_array_index (s->frames, frame_info, i);
    most = MAX (most, (gsize) f->sw * f->sh);
  }
  for (int i = 0; i < RING; i++) {
    s->ring[i].px = g_malloc (most * 4);
    s->ring[i].state = 0; s->ring[i].idx = -1;
  }
  s->saved = g_malloc0 ((gsize) s->w * s->h * 4);
  s->next_decode = s->next_show = 0;
  s->prev = -1;
  s->quit = FALSE;
  int n = s->frames->len > 1 ? WORKERS : 1;
  for (int i = 0; i < n; i++) {
    s->workers[i].s = s;
    s->workers[i].thread = g_thread_new ("animdec-decode", worker_main, &s->workers[i]);
  }
}

static void
workers_stop (CutepiAnimDec * s)
{
  g_mutex_lock (&s->lock);
  s->quit = TRUE;
  g_cond_broadcast (&s->cond);
  g_mutex_unlock (&s->lock);
  for (int i = 0; i < WORKERS; i++)
    if (s->workers[i].thread) { g_thread_join (s->workers[i].thread); s->workers[i].thread = NULL; }
  for (int i = 0; i < RING; i++) g_clear_pointer (&s->ring[i].px, g_free);
  last_drop (s);
  g_clear_pointer (&s->saved, g_free);
}

/* Back to frame 0: frames being decoded are dropped when they finish. */
static void
workers_reset (CutepiAnimDec * s)
{
  g_mutex_lock (&s->lock);
  s->gen++;
  for (int i = 0; i < RING; i++)
    if (s->ring[i].state != 1) s->ring[i].state = 0;
  /* Wait for in-flight decodes of the old generation to come back. */
  for (;;) {
    gboolean busy = FALSE;
    for (int i = 0; i < RING; i++) busy |= s->ring[i].state == 1;
    if (!busy) break;
    g_cond_wait (&s->cond, &s->lock);
  }
  s->next_decode = s->next_show = 0;
  s->prev = -1;
  last_drop (s);
  g_cond_broadcast (&s->cond);
  g_mutex_unlock (&s->lock);
}

/* Straight-alpha OVER, or a copy, of a w x h patch onto the canvas at x, y. */
static void
composite (guint8 * dst, int dstride, const guint8 * src, int stride, int x0, int y0, int w, int h, gboolean over)
{
  for (int y = 0; y < h; y++) {
    guint8 *d = dst + (gsize) (y0 + y) * dstride + (gsize) x0 * 4;
    const guint8 *p = src + (gsize) y * stride;
    if (!over) { memcpy (d, p, (gsize) w * 4); continue; }
    for (int x = 0; x < w; x++, d += 4, p += 4) {
      guint sa = p[3];
      if (sa == 255) { memcpy (d, p, 4); continue; }
      if (sa == 0) continue;
      guint da = d[3];
      guint oa = sa * 255 + da * (255 - sa); /* output alpha * 255 */
      for (int c = 0; c < 3; c++)
        d[c] = (guint8) ((p[c] * sa * 255 + d[c] * da * (255 - sa) + oa / 2) / oa);
      d[3] = (guint8) ((oa + 127) / 255);
    }
  }
}

/* Copy a w*4-wide picture between strides. */
static void
copy_picture (guint8 * dst, int dstride, const guint8 * src, int sstride, int w, int h)
{
  if (dst == src) return; /* a skipped frame built in this buffer */
  for (int y = 0; y < h; y++) memcpy (dst + (gsize) y * dstride, src + (gsize) y * sstride, (gsize) w * 4);
}

/* Build the next frame in dst: the picture the frames so far left (the last
 * frame pushed, after its disposal), with this frame composited over it. */
static gboolean
table_next (CutepiAnimDec * s, guint8 * dst, int stride, GstClockTime * dur)
{
  g_mutex_lock (&s->lock);
  int i = s->next_show;
  if (i >= (int) s->frames->len) { g_mutex_unlock (&s->lock); return FALSE; }
  slot *sl = &s->ring[i % RING];
  while (!(sl->idx == i && sl->gen == s->gen && (sl->state == 2 || sl->state == -1)))
    g_cond_wait (&s->cond, &s->lock);
  g_mutex_unlock (&s->lock);
  gboolean ok = sl->state == 2;
  const frame_info *f = &g_array_index (s->frames, frame_info, i);
  if (ok) {
    const frame_info *p = s->prev >= 0 ? &g_array_index (s->frames, frame_info, s->prev) : NULL;
    gboolean whole = f->x == 0 && f->y == 0 && (int) f->w == s->w && (int) f->h == s->h && f->blend == 0;
    /* The base picture, unless this frame replaces all of it (or must be saved first). */
    if (!whole || f->dispose == 2) {
      if (p && p->dispose == 2) copy_picture (dst, stride, s->saved, s->w * 4, s->w, s->h);
      else if (s->last_px) copy_picture (dst, stride, s->last_px, s->last_stride, s->w, s->h);
      else for (int y = 0; y < s->h; y++) memset (dst + (gsize) y * stride, 0, (gsize) s->w * 4);
      if (p && p->dispose == 1)
        for (guint32 y = 0; y < p->h; y++) memset (dst + (gsize) (p->y + y) * stride + (gsize) p->x * 4, 0, (gsize) p->w * 4);
      if (f->dispose == 2) copy_picture (s->saved, s->w * 4, dst, stride, s->w, s->h);
    }
    if (s->fmt == FMT_GIF) gif_composite (s, f, dst, stride, sl->px);
    else composite (dst, stride, sl->px, f->sw * 4, f->x, f->y, f->w, f->h, f->blend == 1);
    s->prev = i;
    *dur = f->dur;
  } else {
    GST_ELEMENT_WARNING (s, STREAM, DECODE, ("frame %d could not be decoded", i), (NULL));
  }
  g_mutex_lock (&s->lock);
  sl->state = 0; sl->idx = -1;
  s->next_show++;
  g_cond_broadcast (&s->cond);
  g_mutex_unlock (&s->lock);
  return ok;
}

static void
frames_free (CutepiAnimDec * s)
{
  if (!s->frames) return;
  for (guint i = 0; i < s->frames->len; i++) {
    frame_info *f = &g_array_index (s->frames, frame_info, i);
    if (f->data) g_array_unref (f->data);
  }
  g_array_set_size (s->frames, 0);
}

/* WebP: frame table from the demuxer (still or animation). */
static gboolean
webp_open (CutepiAnimDec * s)
{
  WebPData wd = { s->data->data, s->data->len };
  WebPDemuxer *dm = L.webp_demux (&wd, 0, NULL, WEBP_DEMUX_ABI_VERSION);
  if (!dm) return FALSE;
  s->w = L.webp_demux_get (dm, WEBP_FF_CANVAS_WIDTH);
  s->h = L.webp_demux_get (dm, WEBP_FF_CANVAS_HEIGHT);
  s->loops = L.webp_demux_get (dm, WEBP_FF_LOOP_COUNT);
  guint n = L.webp_demux_get (dm, WEBP_FF_FRAME_COUNT);
  GstClockTime total = 0;
  WebPIterator it;
  for (guint i = 1; i <= n; i++) {
    if (!L.webp_get_frame (dm, i, &it)) break;
    frame_info f = { 0 };
    f.x = it.x_offset; f.y = it.y_offset; f.w = f.sw = it.width; f.h = f.sh = it.height;
    /* As libwebp's animation decoder: a 0 ms frame is shown, briefly. */
    f.dur = (GstClockTime) MAX (it.duration, 10) * GST_MSECOND;
    f.dispose = it.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND ? 1 : 0;
    f.blend = it.blend_method == WEBP_MUX_BLEND ? 1 : 0;
    f.off = it.fragment.bytes - s->data->data;
    f.len = it.fragment.size;
    L.webp_release_iter (&it);
    if (f.x + f.w > (guint) s->w || f.y + f.h > (guint) s->h) break;
    g_array_append_val (s->frames, f);
    total += f.dur;
  }
  L.webp_demux_delete (dm);
  if (!s->frames->len || s->w <= 0 || s->h <= 0) return FALSE;
  s->nframes = s->frames->len;
  if (s->nframes > 1) s->duration = total;
  return TRUE;
}

/* APNG: frame table from the fcTL chunks and their IDAT / fdAT data. */
static gboolean
apng_open (CutepiAnimDec * s)
{
  const guint8 *d = s->data->data;
  if (s->data->len < 33 || memcmp (d + 12, "IHDR", 4)) return FALSE;
  s->w = GST_READ_UINT32_BE (d + 16); s->h = GST_READ_UINT32_BE (d + 20);
  s->apng_head = g_byte_array_new ();
  guint32 len; const guint8 *type, *body;
  GstClockTime total = 0;
  gboolean before_idat = TRUE;
  frame_info *cur = NULL;
  for (gsize p = 8; png_chunk (s->data, p, &len, &type, &body); p += 12 + len) {
    if (!memcmp (type, "IEND", 4)) break;
    if (!memcmp (type, "IDAT", 4)) before_idat = FALSE;
    if (!memcmp (type, "acTL", 4) && len >= 8) {
      s->loops = GST_READ_UINT32_BE (body + 4);
    } else if (!memcmp (type, "fcTL", 4) && len >= 26) {
      frame_info f = { 0 };
      f.w = GST_READ_UINT32_BE (body + 4); f.h = GST_READ_UINT32_BE (body + 8);
      f.x = GST_READ_UINT32_BE (body + 12); f.y = GST_READ_UINT32_BE (body + 16);
      f.sw = f.w; f.sh = f.h;
      guint16 dn = GST_READ_UINT16_BE (body + 20), dd = GST_READ_UINT16_BE (body + 22);
      f.dur = gst_util_uint64_scale (dn, GST_SECOND, dd ? dd : 100);
      if (!f.dur) f.dur = GST_SECOND / 100;
      f.dispose = body[24] <= 2 ? body[24] : 0;
      f.blend = body[25] == 1;
      if (!f.w || !f.h || f.x + f.w > (guint) s->w || f.y + f.h > (guint) s->h) return FALSE;
      if (!s->frames->len && f.dispose == 2) f.dispose = 1; /* the spec: first frame "previous" = background */
      f.data = g_array_new (FALSE, FALSE, sizeof (gsize));
      g_array_append_val (s->frames, f);
      cur = &g_array_index (s->frames, frame_info, s->frames->len - 1);
      total += f.dur;
    } else if ((!memcmp (type, "IDAT", 4) || !memcmp (type, "fdAT", 4)) && cur) {
      gsize o = (gsize) (body - d), l = len;
      if (type[0] == 'f') { if (l < 4) continue; o += 4; l -= 4; }
      g_array_append_val (cur->data, o);
      g_array_append_val (cur->data, l);
    } else if (before_idat && memcmp (type, "IHDR", 4) && memcmp (type, "acTL", 4)) {
      char t[5] = { type[0], type[1], type[2], type[3], 0 };
      append_chunk (s->apng_head, t, body, len); /* PLTE, tRNS, gAMA, sRGB, iCCP, ... */
    }
  }
  /* Drop frames without data (a truncated file). */
  while (s->frames->len && !g_array_index (s->frames, frame_info, s->frames->len - 1).data->len) {
    g_array_unref (g_array_index (s->frames, frame_info, s->frames->len - 1).data);
    g_array_set_size (s->frames, s->frames->len - 1);
  }
  if (!s->frames->len) return FALSE;
  s->nframes = s->frames->len;
  if (s->nframes > 1) s->duration = total;
  return TRUE;
}

/* ---- AVIF image sequences ------------------------------------------------------------
 *
 * libavif decodes a sequence with dav1d one frame at a time (max_frame_delay 1)
 * and the colour and alpha images one after the other: a 1080p30 sequence with
 * alpha played at 19.7-22.4 fps. Here the sequence's tracks (ISOBMFF, "avis")
 * feed two frame-threaded dav1d decoders, and libavif only converts each
 * picture pair to RGBA (libyuv inside). */

typedef struct { gsize off, size; GstClockTime dur; } av1_sample;

/* One box at p (inside [.., end)): its type, body and the next box; FALSE at the end. */
static gboolean
bmff_box (const guint8 * p, const guint8 * end, guint32 * type, const guint8 ** body, const guint8 ** next)
{
  if (end - p < 8) return FALSE;
  guint64 size = GST_READ_UINT32_BE (p);
  gsize hdr = 8;
  if (size == 1) {
    if (end - p < 16) return FALSE;
    size = GST_READ_UINT64_BE (p + 8);
    hdr = 16;
  } else if (size == 0) {
    size = end - p;
  }
  if (size < hdr || size > (guint64) (end - p)) return FALSE;
  *type = GST_READ_UINT32_BE (p + 4);
  *body = p + hdr;
  *next = p + size;
  return TRUE;
}

/* The first child box of the given type under [p, end), descending into the
 * container path given (e.g. "mdia", "minf", "stbl", then the box). */
static const guint8 *
bmff_find (const guint8 * p, const guint8 * end, const char *path[], int n, const guint8 ** bend)
{
  guint32 t; const guint8 *b, *nx;
  while (bmff_box (p, end, &t, &b, &nx)) {
    if (t == GST_READ_UINT32_BE (path[0])) {
      if (n == 1) { *bend = nx; return b; }
      return bmff_find (b, nx, path + 1, n - 1, bend);
    }
    p = nx;
  }
  return NULL;
}

/* A track's samples from its sample table: offsets, sizes, durations. */
static GArray *
avis_samples (const guint8 * file, gsize flen, const guint8 * trak, const guint8 * tend, guint32 timescale)
{
  const guint8 *stts, *stsz, *stsc, *stco, *co64 = NULL;
  const char *p_stts[] = { "mdia", "minf", "stbl", "stts" }, *p_stsz[] = { "mdia", "minf", "stbl", "stsz" };
  const char *p_stsc[] = { "mdia", "minf", "stbl", "stsc" }, *p_stco[] = { "mdia", "minf", "stbl", "stco" };
  const char *p_co64[] = { "mdia", "minf", "stbl", "co64" };
  const guint8 *stts_e, *stsz_e, *stsc_e, *stco_e;
  if (!(stts = bmff_find (trak, tend, p_stts, 4, &stts_e)) || !(stsz = bmff_find (trak, tend, p_stsz, 4, &stsz_e)) ||
      !(stsc = bmff_find (trak, tend, p_stsc, 4, &stsc_e)))
    return NULL;
  gboolean wide = FALSE;
  if (!(stco = bmff_find (trak, tend, p_stco, 4, &stco_e))) {
    if (!(co64 = bmff_find (trak, tend, p_co64, 4, &stco_e))) return NULL;
    stco = co64; wide = TRUE;
  }
  if (stsz_e - stsz < 12 || stco_e - stco < 8 || stsc_e - stsc < 8 || stts_e - stts < 8) return NULL;
  guint32 fixed = GST_READ_UINT32_BE (stsz + 4), n = GST_READ_UINT32_BE (stsz + 8);
  if (n == 0 || n > 1000000 || (!fixed && (gsize) (stsz_e - stsz - 12) / 4 < n)) return NULL;
  GArray *a = g_array_sized_new (FALSE, TRUE, sizeof (av1_sample), n);
  g_array_set_size (a, n);
  for (guint32 i = 0; i < n; i++)
    g_array_index (a, av1_sample, i).size = fixed ? fixed : GST_READ_UINT32_BE (stsz + 12 + 4 * i);
  /* Chunks: sample i's offset is its chunk's offset plus the sizes before it in the chunk. */
  guint32 nchunks = GST_READ_UINT32_BE (stco + 4), nsc = GST_READ_UINT32_BE (stsc + 4);
  if ((gsize) (stco_e - stco - 8) / (wide ? 8 : 4) < nchunks || (gsize) (stsc_e - stsc - 8) / 12 < nsc) goto fail;
  guint32 si = 0;
  for (guint32 e2 = 0; e2 < nsc && si < n; e2++) {
    guint32 first = GST_READ_UINT32_BE (stsc + 8 + 12 * e2), per = GST_READ_UINT32_BE (stsc + 12 + 12 * e2);
    guint32 last = e2 + 1 < nsc ? GST_READ_UINT32_BE (stsc + 8 + 12 * (e2 + 1)) : nchunks + 1;
    for (guint32 c = first; c < last && c <= nchunks && si < n; c++) {
      guint64 off = wide ? GST_READ_UINT64_BE (stco + 8 + 8 * (c - 1)) : GST_READ_UINT32_BE (stco + 8 + 4 * (c - 1));
      for (guint32 k = 0; k < per && si < n; k++, si++) {
        av1_sample *sm = &g_array_index (a, av1_sample, si);
        sm->off = off;
        off += sm->size;
        if (sm->off + sm->size > flen) goto fail;
      }
    }
  }
  if (si != n) goto fail;
  /* Durations (stts runs). */
  guint32 nts = GST_READ_UINT32_BE (stts + 4);
  if ((gsize) (stts_e - stts - 8) / 8 < nts) goto fail;
  si = 0;
  for (guint32 r = 0; r < nts && si < n; r++) {
    guint32 cnt = GST_READ_UINT32_BE (stts + 8 + 8 * r), delta = GST_READ_UINT32_BE (stts + 12 + 8 * r);
    for (guint32 k = 0; k < cnt && si < n; k++, si++)
      g_array_index (a, av1_sample, si).dur = gst_util_uint64_scale (delta, GST_SECOND, timescale ? timescale : 1);
  }
  return a;
fail:
  g_array_unref (a);
  return NULL;
}

/* Find the sequence's colour track and the alpha track that references it
 * (tref auxl). FALSE when the file has no image sequence. */
static gboolean
avis_open (CutepiAnimDec * s)
{
  const guint8 *d = s->data->data, *end = d + s->data->len, *mend;
  const char *p_moov[] = { "moov" };
  const guint8 *moov = bmff_find (d, end, p_moov, 1, &mend);
  if (!moov) return FALSE;
  struct { guint32 id, auxl; GArray *samples; } tr[8];
  int nt = 0;
  guint32 t; const guint8 *b, *nx;
  for (const guint8 *p = moov; nt < 8 && bmff_box (p, mend, &t, &b, &nx); p = nx) {
    if (t != GST_READ_UINT32_BE ("trak")) continue;
    const guint8 *e;
    const char *p_tkhd[] = { "tkhd" }, *p_hdlr[] = { "mdia", "hdlr" }, *p_mdhd[] = { "mdia", "mdhd" };
    const char *p_auxl[] = { "tref", "auxl" };
    const guint8 *tkhd = bmff_find (b, nx, p_tkhd, 1, &e), *hdlr = bmff_find (b, nx, p_hdlr, 2, &e);
    const guint8 *mdhd = bmff_find (b, nx, p_mdhd, 2, &e);
    if (!tkhd || !hdlr || !mdhd) continue;
    guint32 handler = GST_READ_UINT32_BE (hdlr + 8);
    if (handler != GST_READ_UINT32_BE ("pict") && handler != GST_READ_UINT32_BE ("auxv")) continue;
    guint32 id = GST_READ_UINT32_BE (tkhd + (tkhd[0] == 1 ? 20 : 12));
    guint32 ts = GST_READ_UINT32_BE (mdhd + (mdhd[0] == 1 ? 20 : 12));
    const guint8 *auxl = bmff_find (b, nx, p_auxl, 2, &e);
    GArray *sm = avis_samples (d, s->data->len, b, nx, ts);
    if (!sm) continue;
    tr[nt].id = id; tr[nt].auxl = auxl && e - auxl >= 4 ? GST_READ_UINT32_BE (auxl) : 0; tr[nt].samples = sm;
    nt++;
  }
  int color = -1, alpha = -1;
  for (int i = 0; i < nt; i++) if (!tr[i].auxl && color < 0) color = i;
  for (int i = 0; color >= 0 && i < nt; i++) if (tr[i].auxl == tr[color].id && alpha < 0) alpha = i;
  for (int i = 0; i < nt; i++)
    if (i != color && i != alpha) g_array_unref (tr[i].samples);
  if (color < 0) { if (alpha >= 0) g_array_unref (tr[alpha].samples); return FALSE; }
  memset (s->seq, 0, sizeof (s->seq));
  s->seq[0].samples = tr[color].samples;
  if (alpha >= 0) {
    if (tr[alpha].samples->len == tr[color].samples->len) s->seq[1].samples = tr[alpha].samples;
    else g_array_unref (tr[alpha].samples);
  }
  for (int k = 0; k < 2; k++) {
    if (!s->seq[k].samples) continue;
    Dav1dSettings ds;
    L.d_defaults (&ds);
    ds.n_threads = 4;
    ds.all_layers = 0;
    if (L.d_open (&s->seq[k].ctx, &ds) < 0) return FALSE;
  }
  GstClockTime total = 0;
  for (guint i = 0; i < s->seq[0].samples->len; i++) total += g_array_index (s->seq[0].samples, av1_sample, i).dur;
  s->nframes = s->seq[0].samples->len;
  if (s->nframes > 1) s->duration = total;
  s->loops = 0; /* as browsers: repeat */
  return TRUE;
}

static void
no_free (const guint8 * data, void *cookie)
{
}

/* The next picture of track k (dav1d frame threading: send samples until a
 * picture comes, then drain). */
static gboolean
avis_picture (CutepiAnimDec * s, int k, Dav1dPicture * pic)
{
  struct av1_track *t = &s->seq[k];
  for (;;) {
    if (!t->pending.sz && t->next < t->samples->len) {
      const av1_sample *sm = &g_array_index (t->samples, av1_sample, t->next++);
      if (L.d_wrap (&t->pending, s->data->data + sm->off, sm->size, no_free, NULL) < 0) return FALSE;
    }
    if (t->pending.sz) {
      int r = L.d_send (t->ctx, &t->pending);
      if (r < 0 && r != DAV1D_ERR (EAGAIN)) { L.d_data_unref (&t->pending); return FALSE; }
    }
    memset (pic, 0, sizeof (*pic));
    int r = L.d_get (t->ctx, pic);
    if (r == 0) return TRUE;
    if (r != DAV1D_ERR (EAGAIN)) return FALSE;
    if (!t->pending.sz && t->next >= t->samples->len) return FALSE; /* drained */
  }
}

static void
avis_reset (CutepiAnimDec * s)
{
  for (int k = 0; k < 2; k++) {
    if (!s->seq[k].ctx) continue;
    if (s->seq[k].pending.sz) L.d_data_unref (&s->seq[k].pending);
    L.d_flush (s->seq[k].ctx);
    s->seq[k].next = 0;
  }
}

static void
avis_close (CutepiAnimDec * s)
{
  for (int k = 0; k < 2; k++) {
    if (s->seq[k].pending.sz) L.d_data_unref (&s->seq[k].pending);
    if (s->seq[k].ctx) L.d_close (&s->seq[k].ctx);
    if (s->seq[k].samples) g_array_unref (s->seq[k].samples);
  }
  memset (s->seq, 0, sizeof (s->seq));
  s->avis = FALSE;
}

/* The next frame: colour (and alpha) pictures to RGBA in dst. */
static gboolean
avis_next (CutepiAnimDec * s, guint8 * dst, int stride, GstClockTime * dur)
{
  Dav1dPicture c, a;
  if (!avis_picture (s, 0, &c)) return FALSE;
  gboolean have_a = s->seq[1].ctx && avis_picture (s, 1, &a);
  avifImage *im = L.avif_image_empty ();
  gboolean ok = im != NULL;
  if (ok) {
    static const avifPixelFormat fmt[] = { AVIF_PIXEL_FORMAT_YUV400, AVIF_PIXEL_FORMAT_YUV420,
      AVIF_PIXEL_FORMAT_YUV422, AVIF_PIXEL_FORMAT_YUV444 };
    im->width = c.p.w; im->height = c.p.h; im->depth = c.p.bpc;
    im->yuvFormat = fmt[c.p.layout];
    im->yuvRange = c.seq_hdr->color_range ? AVIF_RANGE_FULL : AVIF_RANGE_LIMITED;
    im->colorPrimaries = c.seq_hdr->pri;
    im->transferCharacteristics = c.seq_hdr->trc;
    im->matrixCoefficients = c.seq_hdr->mtrx;
    for (int i = 0; i < 3; i++) {
      im->yuvPlanes[i] = c.data[i];
      im->yuvRowBytes[i] = (uint32_t) c.stride[i ? 1 : 0];
    }
    if (have_a && a.p.w == c.p.w && a.p.h == c.p.h && a.p.bpc == c.p.bpc) {
      im->alphaPlane = a.data[0];
      im->alphaRowBytes = (uint32_t) a.stride[0];
    }
    im->imageOwnsYUVPlanes = im->imageOwnsAlphaPlane = AVIF_FALSE;
    avifRGBImage rgb;
    L.avif_rgb_defaults (&rgb, im);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;
    rgb.pixels = dst;
    rgb.rowBytes = stride;
    rgb.maxThreads = 4;
    ok = (int) c.p.w == s->w && (int) c.p.h == s->h && L.avif_yuv_to_rgb (im, &rgb) == AVIF_RESULT_OK;
    for (int i = 0; i < 3; i++) im->yuvPlanes[i] = NULL;
    im->alphaPlane = NULL;
    L.avif_image_destroy (im);
  }
  /* Pictures come out in order, one per sample. */
  guint shown = s->avis_shown++;
  *dur = s->nframes > 1 && shown < s->seq[0].samples->len ?
      g_array_index (s->seq[0].samples, av1_sample, shown).dur : GST_CLOCK_TIME_NONE;
  L.d_pic_unref (&c);
  if (have_a) L.d_pic_unref (&a);
  return ok;
}

static gboolean
jxl_start (CutepiAnimDec * s)
{
  if (s->jxl) L.jxl_rewind (s->jxl);
  else {
    s->jxl = L.jxl_create (NULL);
    if (!s->jxl) return FALSE;
    if (L.jxl_runner_create && !s->jxl_runner) {
      s->jxl_runner = L.jxl_runner_create (NULL, 4);
      if (s->jxl_runner) L.jxl_set_runner (s->jxl, L.jxl_runner, s->jxl_runner);
    }
  }
  if (L.jxl_subscribe (s->jxl, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) return FALSE;
  if (L.jxl_set_input (s->jxl, s->data->data, s->data->len) != JXL_DEC_SUCCESS) return FALSE;
  L.jxl_close_input (s->jxl);
  return TRUE;
}

static void
dec_close (CutepiAnimDec * s)
{
  workers_stop (s);
  frames_free (s);
  if (s->apng_head) { g_byte_array_unref (s->apng_head); s->apng_head = NULL; }
  if (s->avif) { L.avif_destroy (s->avif); s->avif = NULL; }
  avis_close (s);
  if (s->jxl) { L.jxl_destroy (s->jxl); s->jxl = NULL; }
  if (s->jxl_runner) { L.jxl_runner_destroy (s->jxl_runner); s->jxl_runner = NULL; }
}

/* Open the collected file: size, frames, loop count, total duration. */
static gboolean
dec_open (CutepiAnimDec * s)
{
  const guint8 *d = s->data->data;
  gsize n = s->data->len;
  s->duration = GST_CLOCK_TIME_NONE;
  s->nframes = 0;
  switch (s->fmt) {
    case FMT_WEBP:
    case FMT_APNG:
    case FMT_GIF:
      if (!(s->fmt == FMT_WEBP ? webp_open (s) : s->fmt == FMT_GIF ? gif_open (s) : apng_open (s))) return FALSE;
      workers_start (s);
      return TRUE;
    case FMT_AVIF: {
      /* CUTEPI_ANIMDEC_LIBAVIF=1: libavif for sequences too (tests compare the two). */
      if (L.dav1d && !g_getenv ("CUTEPI_ANIMDEC_LIBAVIF") && avis_open (s)) {
        /* Size from libavif's parse of the container (also checks it). */
        avifDecoder *probe = L.avif_create ();
        gboolean ok = probe && L.avif_set_mem (probe, d, n) == AVIF_RESULT_OK && L.avif_parse (probe) == AVIF_RESULT_OK;
        if (ok) { s->w = probe->image->width; s->h = probe->image->height; }
        if (probe) L.avif_destroy (probe);
        if (ok && s->nframes > 1) {
          s->avis = TRUE;
          s->avis_shown = 0;
          return TRUE;
        }
        avis_close (s);
        s->duration = GST_CLOCK_TIME_NONE;
        s->nframes = 0;
      }
      s->avif = L.avif_create ();
      if (!s->avif) return FALSE;
      s->avif->maxThreads = 4;
      s->avif->ignoreExif = s->avif->ignoreXMP = AVIF_TRUE;
      avifResult r = L.avif_set_mem (s->avif, d, n);
      if (r == AVIF_RESULT_OK) r = L.avif_parse (s->avif);
      if (r != AVIF_RESULT_OK) {
        GST_ERROR_OBJECT (s, "AVIF: %s", L.avif_result_string (r));
        return FALSE;
      }
      s->w = s->avif->image->width; s->h = s->avif->image->height;
      s->nframes = s->avif->imageCount > 0 ? s->avif->imageCount : 1;
      s->loops = s->avif->repetitionCount < 0 ? 0 : (guint) s->avif->repetitionCount + 1;
      if (s->nframes > 1 && s->avif->duration > 0)
        s->duration = (GstClockTime) (s->avif->duration * GST_SECOND);
      return TRUE;
    }
    case FMT_JXL: {
      if (!jxl_start (s)) return FALSE;
      JxlBasicInfo bi;
      for (;;) {
        JxlDecoderStatus st = L.jxl_process (s->jxl);
        if (st == JXL_DEC_BASIC_INFO) break;
        if (st == JXL_DEC_ERROR || st == JXL_DEC_SUCCESS || st == JXL_DEC_NEED_MORE_INPUT) return FALSE;
      }
      if (L.jxl_basic_info (s->jxl, &bi) != JXL_DEC_SUCCESS) return FALSE;
      s->w = bi.xsize; s->h = bi.ysize;
      s->nframes = bi.have_animation ? 0 : 1;
      s->loops = bi.have_animation ? bi.animation.num_loops : 1;
      s->jxl_tick = bi.have_animation && bi.animation.tps_numerator ?
          (double) bi.animation.tps_denominator / bi.animation.tps_numerator : 0;
      s->jxl_fmt = (JxlPixelFormat) { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
      if (bi.have_animation) {
        /* Count frames and total ticks from the frame headers (no pixels). */
        JxlDecoder *probe = L.jxl_create (NULL);
        guint64 ticks = 0; guint frames = 0;
        if (probe && L.jxl_subscribe (probe, JXL_DEC_FRAME) == JXL_DEC_SUCCESS &&
            L.jxl_set_input (probe, d, n) == JXL_DEC_SUCCESS) {
          L.jxl_close_input (probe);
          while (L.jxl_process (probe) == JXL_DEC_FRAME) {
            JxlFrameHeader fh;
            if (L.jxl_frame_header (probe, &fh) == JXL_DEC_SUCCESS) { ticks += fh.duration; frames++; }
          }
        }
        if (probe) L.jxl_destroy (probe);
        s->nframes = frames;
        if (s->jxl_tick > 0) s->duration = (GstClockTime) (ticks * s->jxl_tick * GST_SECOND);
      }
      return TRUE;
    }
    default:
      return FALSE;
  }
}

/* Back to the first frame. */
static gboolean
dec_reset (CutepiAnimDec * s)
{
  s->pts = 0;
  switch (s->fmt) {
    case FMT_WEBP: case FMT_APNG: case FMT_GIF: workers_reset (s); return TRUE;
    case FMT_AVIF:
      if (s->avis) { avis_reset (s); s->avis_shown = 0; return TRUE; }
      return L.avif_reset (s->avif) == AVIF_RESULT_OK;
    case FMT_JXL: return jxl_start (s);
    default: return FALSE;
  }
}

/* Decode the next frame into dst (RGBA, stride). *dur: its display time
 * (NONE for a still). FALSE at the end or on error. */
static gboolean
dec_next (CutepiAnimDec * s, guint8 * dst, int stride, GstClockTime * dur)
{
  *dur = GST_CLOCK_TIME_NONE;
  switch (s->fmt) {
    case FMT_WEBP:
    case FMT_APNG:
    case FMT_GIF: {
      GstClockTime d;
      if (!table_next (s, dst, stride, &d)) return FALSE;
      if (s->nframes > 1) *dur = d;
      return TRUE;
    }
    case FMT_AVIF: {
      if (s->avis) return avis_next (s, dst, stride, dur);
      avifResult r = L.avif_next (s->avif);
      if (r != AVIF_RESULT_OK) {
        if (r != AVIF_RESULT_NO_IMAGES_REMAINING) GST_WARNING_OBJECT (s, "AVIF frame: %s", L.avif_result_string (r));
        return FALSE;
      }
      avifRGBImage rgb;
      L.avif_rgb_defaults (&rgb, s->avif->image);
      rgb.format = AVIF_RGB_FORMAT_RGBA;
      rgb.depth = 8;
      rgb.pixels = dst;
      rgb.rowBytes = stride;
      rgb.maxThreads = 4;
      if (L.avif_yuv_to_rgb (s->avif->image, &rgb) != AVIF_RESULT_OK) return FALSE;
      if (s->nframes > 1) *dur = (GstClockTime) (s->avif->imageTiming.duration * GST_SECOND);
      return TRUE;
    }
    case FMT_JXL: {
      JxlFrameHeader fh = { 0 };
      gboolean have_fh = FALSE;
      for (;;) {
        switch (L.jxl_process (s->jxl)) {
          case JXL_DEC_BASIC_INFO: continue;
          case JXL_DEC_FRAME:
            have_fh = L.jxl_frame_header (s->jxl, &fh) == JXL_DEC_SUCCESS;
            continue;
          case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            s->jxl_fmt.align = stride;
            size_t need;
            if (L.jxl_out_size (s->jxl, &s->jxl_fmt, &need) != JXL_DEC_SUCCESS || need > (size_t) stride * s->h) return FALSE;
            if (L.jxl_set_out (s->jxl, &s->jxl_fmt, dst, (size_t) stride * s->h) != JXL_DEC_SUCCESS) return FALSE;
            continue;
          }
          case JXL_DEC_FULL_IMAGE:
            if (s->jxl_tick > 0 && have_fh) *dur = (GstClockTime) (fh.duration * s->jxl_tick * GST_SECOND);
            if (s->nframes > 1 && (!GST_CLOCK_TIME_IS_VALID (*dur) || *dur == 0)) *dur = GST_SECOND / 100;
            return TRUE;
          default:
            return FALSE; /* SUCCESS (no more frames), ERROR, NEED_MORE_INPUT */
        }
      }
    }
    default:
      return FALSE;
  }
}

/* ---- streaming --------------------------------------------------------------------- */

static void
push_caps (CutepiAnimDec * s)
{
  gint fn = 0, fd = 1;
  if (s->nframes != 1) {
    /* A nominal rate for an animation (frames keep their own durations);
     * 0/1 would mark it a still. */
    if (s->nframes > 1 && GST_CLOCK_TIME_IS_VALID (s->duration) && s->duration > 0)
      gst_util_double_to_fraction ((double) s->nframes * GST_SECOND / s->duration, &fn, &fd);
    if (fn <= 0) { fn = 25; fd = 1; }
    /* keep it sane */
    if ((double) fn / fd > 1000) { fn = 1000; fd = 1; }
  }
  GstCaps *caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "RGBA",
      "width", G_TYPE_INT, s->w, "height", G_TYPE_INT, s->h,
      "framerate", GST_TYPE_FRACTION, fn, fd, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
  gst_pad_push_event (s->srcpad, gst_event_new_caps (caps));
  if (!s->pool) {
    GstVideoInfo vi;
    gst_video_info_from_caps (&vi, caps);
    s->pool = gst_video_buffer_pool_new ();
    GstStructure *cfg = gst_buffer_pool_get_config (s->pool);
    gst_buffer_pool_config_set_params (cfg, caps, vi.size, 4, 0);
    gst_buffer_pool_config_add_option (cfg, GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_buffer_pool_set_config (s->pool, cfg);
    gst_buffer_pool_set_active (s->pool, TRUE);
  }
  gst_caps_unref (caps);
}

static void
loop (CutepiAnimDec * s)
{
  if (s->need_caps) {
    push_caps (s);
    s->need_caps = FALSE;
  }
  if (s->need_segment) {
    gst_pad_push_event (s->srcpad, gst_event_new_segment (&s->segment));
    s->need_segment = FALSE;
  }
  GstBuffer *b = NULL;
  if (gst_buffer_pool_acquire_buffer (s->pool, &b, NULL) != GST_FLOW_OK) goto pause;
  GstVideoMeta *vm = gst_buffer_get_video_meta (b);
  GstMapInfo m;
  if (!gst_buffer_map (b, &m, GST_MAP_WRITE)) { gst_buffer_unref (b); goto pause; }
  int stride = vm ? vm->stride[0] : s->w * 4;
  GstClockTime dur;
  gboolean ok;
  guint8 *px = m.data + (vm ? vm->offset[0] : 0);
  for (;;) {
    ok = dec_next (s, px, stride, &dur);
    if (!ok) break;
    /* Skip frames that end before a seek target; the next one builds on
     * this one, in the same buffer. */
    if (GST_CLOCK_TIME_IS_VALID (s->seek_to) && GST_CLOCK_TIME_IS_VALID (dur) && s->pts + dur <= s->seek_to) {
      s->pts += dur;
      last_drop (s);
      s->last_px = px;
      s->last_stride = stride;
      continue;
    }
    break;
  }
  gst_buffer_unmap (b, &m);
  if (s->last_px == px) s->last_px = NULL;
  if (!ok) {
    gst_buffer_unref (b);
    gst_pad_push_event (s->srcpad, gst_event_new_eos ());
    goto pause;
  }
  s->seek_to = GST_CLOCK_TIME_NONE;
  GST_BUFFER_PTS (b) = s->pts;
  GST_BUFFER_DURATION (b) = dur;
  if (GST_CLOCK_TIME_IS_VALID (dur)) s->pts += dur;
  if (s->frames->len) last_keep (s, b); /* the next frame's base (WebP, APNG, GIF) */
  GstFlowReturn fr = gst_pad_push (s->srcpad, b);
  if (fr != GST_FLOW_OK) {
    if (fr == GST_FLOW_NOT_LINKED || fr < GST_FLOW_EOS) {
      GST_ELEMENT_FLOW_ERROR (s, fr);
      gst_pad_push_event (s->srcpad, gst_event_new_eos ());
    }
    goto pause;
  }
  if (!GST_CLOCK_TIME_IS_VALID (dur)) { /* a still: one frame, then EOS */
    gst_pad_push_event (s->srcpad, gst_event_new_eos ());
    goto pause;
  }
  return;
pause:
  gst_pad_pause_task (s->srcpad);
}

/* The file is complete: open it and start pushing frames. */
static gboolean
start_decoding (CutepiAnimDec * s)
{
  if (!dec_open (s)) {
    GST_ELEMENT_ERROR (s, STREAM, DECODE, ("could not decode this image"), ("format %d", s->fmt));
    return FALSE;
  }
  GST_INFO_OBJECT (s, "%dx%d, %u frames, %" GST_TIME_FORMAT ", loops %u", s->w, s->h, s->nframes,
      GST_TIME_ARGS (s->duration), s->loops);
  gst_segment_init (&s->segment, GST_FORMAT_TIME);
  if (GST_CLOCK_TIME_IS_VALID (s->duration)) s->segment.duration = s->duration;
  s->need_caps = s->need_segment = TRUE;
  s->pts = 0;
  s->seek_to = GST_CLOCK_TIME_NONE;
  return gst_pad_start_task (s->srcpad, (GstTaskFunction) loop, s, NULL);
}

static GstFlowReturn
sink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  CutepiAnimDec *s = (CutepiAnimDec *) parent;
  if (s->have_eos) { gst_buffer_unref (buf); return GST_FLOW_EOS; }
  GstMapInfo m;
  if (gst_buffer_map (buf, &m, GST_MAP_READ)) {
    if (s->data->len + m.size > MAX_FILE) {
      gst_buffer_unmap (buf, &m);
      gst_buffer_unref (buf);
      GST_ELEMENT_ERROR (s, STREAM, TOO_LAZY, ("image file larger than 512 MB"), (NULL));
      return GST_FLOW_ERROR;
    }
    g_byte_array_append (s->data, m.data, m.size);
    gst_buffer_unmap (buf, &m);
  }
  gst_buffer_unref (buf);
  return GST_FLOW_OK;
}

static gboolean
sink_event (GstPad * pad, GstObject * parent, GstEvent * ev)
{
  CutepiAnimDec *s = (CutepiAnimDec *) parent;
  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_CAPS: {
      GstCaps *caps;
      gst_event_parse_caps (ev, &caps);
      GstStructure *st = gst_caps_get_structure (caps, 0);
      const char *name = gst_structure_get_name (st);
      s->fmt = !strcmp (name, "image/gif") ? FMT_GIF : !strcmp (name, "image/webp") ? FMT_WEBP : !strcmp (name, "video/x-apng") ? FMT_APNG :
          !strcmp (name, "image/avif") ? FMT_AVIF : !strcmp (name, "image/jxl") ? FMT_JXL : FMT_NONE;
      gst_event_unref (ev);
      return s->fmt != FMT_NONE;
    }
    case GST_EVENT_SEGMENT:
      gst_event_unref (ev); /* bytes from the source; ours is TIME */
      return TRUE;
    case GST_EVENT_EOS:
      gst_event_unref (ev);
      if (s->have_eos) return TRUE;
      s->have_eos = TRUE;
      return start_decoding (s);
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_FLUSH_STOP:
      /* An upstream flush (not ours): the file is kept, nothing to redo. */
      gst_event_unref (ev);
      return TRUE;
    case GST_EVENT_STREAM_START:
    default:
      return gst_pad_event_default (pad, parent, ev);
  }
}

static gboolean
src_event (GstPad * pad, GstObject * parent, GstEvent * ev)
{
  CutepiAnimDec *s = (CutepiAnimDec *) parent;
  if (GST_EVENT_TYPE (ev) == GST_EVENT_QOS || GST_EVENT_TYPE (ev) == GST_EVENT_NAVIGATION ||
      GST_EVENT_TYPE (ev) == GST_EVENT_RECONFIGURE) {
    gst_event_unref (ev);
    return TRUE;
  }
  if (GST_EVENT_TYPE (ev) != GST_EVENT_SEEK) return gst_pad_event_default (pad, parent, ev);
  gdouble rate; GstFormat fmt; GstSeekFlags flags; GstSeekType st, et; gint64 start, stop;
  gst_event_parse_seek (ev, &rate, &fmt, &flags, &st, &start, &et, &stop);
  gst_event_unref (ev);
  if (fmt != GST_FORMAT_TIME || rate <= 0 || !s->have_eos) return FALSE;
  gboolean flush = flags & GST_SEEK_FLAG_FLUSH;
  if (flush) gst_pad_push_event (s->srcpad, gst_event_new_flush_start ());
  else gst_pad_pause_task (s->srcpad);
  GST_PAD_STREAM_LOCK (s->srcpad);
  if (flush) gst_pad_push_event (s->srcpad, gst_event_new_flush_stop (TRUE));
  gboolean update;
  gst_segment_do_seek (&s->segment, rate, fmt, flags, st, start, et, stop, &update);
  if (GST_CLOCK_TIME_IS_VALID (s->duration)) s->segment.duration = s->duration;
  gboolean ok = dec_reset (s);
  s->seek_to = s->segment.position > 0 ? s->segment.position : GST_CLOCK_TIME_NONE;
  s->segment.start = s->segment.time = s->segment.position;
  s->need_segment = TRUE;
  if (ok) gst_pad_start_task (s->srcpad, (GstTaskFunction) loop, s, NULL);
  GST_PAD_STREAM_UNLOCK (s->srcpad);
  return ok;
}

static gboolean
src_query (GstPad * pad, GstObject * parent, GstQuery * q)
{
  CutepiAnimDec *s = (CutepiAnimDec *) parent;
  switch (GST_QUERY_TYPE (q)) {
    case GST_QUERY_DURATION: {
      GstFormat f;
      gst_query_parse_duration (q, &f, NULL);
      if (f != GST_FORMAT_TIME || !GST_CLOCK_TIME_IS_VALID (s->duration)) return FALSE;
      gst_query_set_duration (q, GST_FORMAT_TIME, s->duration);
      return TRUE;
    }
    case GST_QUERY_SEEKING: {
      GstFormat f;
      gst_query_parse_seeking (q, &f, NULL, NULL, NULL);
      if (f != GST_FORMAT_TIME) return FALSE;
      gst_query_set_seeking (q, GST_FORMAT_TIME, s->have_eos, 0,
          GST_CLOCK_TIME_IS_VALID (s->duration) ? (gint64) s->duration : -1);
      return TRUE;
    }
    default:
      return gst_pad_query_default (pad, parent, q);
  }
}

static GstStateChangeReturn
change_state (GstElement * e, GstStateChange t)
{
  CutepiAnimDec *s = (CutepiAnimDec *) e;
  if (t == GST_STATE_CHANGE_READY_TO_PAUSED) {
    g_byte_array_set_size (s->data, 0);
    s->have_eos = FALSE;
  }
  GstStateChangeReturn r = GST_ELEMENT_CLASS (cutepi_anim_dec_parent_class)->change_state (e, t);
  if (t == GST_STATE_CHANGE_PAUSED_TO_READY) {
    gst_pad_stop_task (s->srcpad);
    dec_close (s);
    if (s->pool) {
      gst_buffer_pool_set_active (s->pool, FALSE);
      gst_object_unref (s->pool);
      s->pool = NULL;
    }
    g_byte_array_set_size (s->data, 0);
    s->have_eos = FALSE;
  }
  return r;
}

static void
finalize (GObject * o)
{
  CutepiAnimDec *s = (CutepiAnimDec *) o;
  dec_close (s);
  g_byte_array_unref (s->data);
  g_array_unref (s->frames);
  g_mutex_clear (&s->lock);
  g_cond_clear (&s->cond);
  G_OBJECT_CLASS (cutepi_anim_dec_parent_class)->finalize (o);
}

static void
cutepi_anim_dec_init (CutepiAnimDec * s)
{
  s->sinkpad = gst_pad_new_from_static_template (&sink_tmpl, "sink");
  gst_pad_set_chain_function (s->sinkpad, sink_chain);
  gst_pad_set_event_function (s->sinkpad, sink_event);
  gst_element_add_pad (GST_ELEMENT (s), s->sinkpad);
  s->srcpad = gst_pad_new_from_static_template (&src_tmpl, "src");
  gst_pad_set_event_function (s->srcpad, src_event);
  gst_pad_set_query_function (s->srcpad, src_query);
  gst_pad_use_fixed_caps (s->srcpad);
  gst_element_add_pad (GST_ELEMENT (s), s->srcpad);
  s->data = g_byte_array_new ();
  s->duration = GST_CLOCK_TIME_NONE;
  s->frames = g_array_new (FALSE, TRUE, sizeof (frame_info));
  g_mutex_init (&s->lock);
  g_cond_init (&s->cond);
}

static void
cutepi_anim_dec_class_init (CutepiAnimDecClass * k)
{
  GstElementClass *ek = GST_ELEMENT_CLASS (k);
  G_OBJECT_CLASS (k)->finalize = finalize;
  ek->change_state = change_state;
  gst_element_class_add_static_pad_template (ek, &sink_tmpl);
  gst_element_class_add_static_pad_template (ek, &src_tmpl);
  gst_element_class_set_static_metadata (ek, "CuTePi animated image decoder", "Codec/Decoder/Image",
      "Animated WebP, APNG, AVIF and JPEG XL (and their stills) to RGBA at the file's own frame timing",
      "CuTePi");
}

/* Register the element (the formats whose library loaded) and the type
 * finders. Returns a bit per format: 1 WebP, 2 APNG, 4 AVIF, 8 JPEG XL, 16 GIF. */
int
cutepi_animdec_register (GstPlugin * plugin, int rank)
{
  GST_DEBUG_CATEGORY_INIT (animdec_debug, "cutepianimdec", 0, "CuTePi animated image decoder");
  load_libs ();
  int have = (L.webp ? 1 : 0) | 2 | (L.avif ? 4 : 0) | (L.jxl ? 8 : 0) | 16;
  /* Above the png and qtdemux typefinders (PRIMARY), which would claim APNG and AVIF. */
  gst_type_find_register (plugin, "cutepi/apng", GST_RANK_PRIMARY + 1, apng_typefind, "png,apng",
      gst_static_caps_get (&apng_caps), NULL, NULL);
  if (L.avif)
    gst_type_find_register (plugin, "cutepi/avif", GST_RANK_PRIMARY + 1, avif_typefind, "avif,avifs",
        gst_static_caps_get (&avif_caps), NULL, NULL);
  if (L.jxl)
    gst_type_find_register (plugin, "cutepi/jxl", GST_RANK_PRIMARY + 1, jxl_typefind, "jxl",
        gst_static_caps_get (&jxl_caps), NULL, NULL);
  if (!gst_element_register (plugin, "cutepianimdec", rank, cutepi_anim_dec_get_type ())) return 0;
  return have;
}

/* For tests: the frames of a WebP animation as libwebp's own WebPAnimDecoder
 * composites them (RGBA, straight alpha), written one after another to out;
 * returns the frame count, or -1. */
int
cutepi_animdec_webp_reference (const guint8 * data, gsize len, guint8 * out, gsize out_size)
{
  void *h = dlopen ("libwebpdemux.so.2", RTLD_NOW | RTLD_LOCAL);
  WebPAnimDecoder *(*mk) (const WebPData *, const WebPAnimDecoderOptions *, int);
  int (*opts) (WebPAnimDecoderOptions *, int);
  int (*info) (const WebPAnimDecoder *, WebPAnimInfo *);
  int (*next) (WebPAnimDecoder *, uint8_t **, int *);
  int (*more) (const WebPAnimDecoder *);
  void (*del) (WebPAnimDecoder *);
  if (!h || !(*(void **) &mk = dlsym (h, "WebPAnimDecoderNewInternal")) ||
      !(*(void **) &opts = dlsym (h, "WebPAnimDecoderOptionsInitInternal")) ||
      !(*(void **) &info = dlsym (h, "WebPAnimDecoderGetInfo")) || !(*(void **) &next = dlsym (h, "WebPAnimDecoderGetNext")) ||
      !(*(void **) &more = dlsym (h, "WebPAnimDecoderHasMoreFrames")) || !(*(void **) &del = dlsym (h, "WebPAnimDecoderDelete")))
    return -1;
  WebPAnimDecoderOptions o;
  if (!opts (&o, WEBP_DEMUX_ABI_VERSION)) return -1;
  o.color_mode = MODE_RGBA;
  WebPData wd = { data, len };
  WebPAnimDecoder *d = mk (&wd, &o, WEBP_DEMUX_ABI_VERSION);
  WebPAnimInfo ai;
  if (!d || !info (d, &ai)) return -1;
  gsize fs = (gsize) ai.canvas_width * ai.canvas_height * 4;
  int n = 0;
  uint8_t *buf; int ts;
  while (more (d) && next (d, &buf, &ts) && (n + 1) * fs <= out_size) memcpy (out + n++ * fs, buf, fs);
  del (d);
  return n;
}

#ifdef STANDALONE_PLUGIN
/* Standalone GStreamer plugin build (see this folder's Makefile). */
#ifndef PACKAGE
#define PACKAGE "cutepianimdec"
#endif
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_animdec_register (p, GST_RANK_PRIMARY + 1);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepianimdec, "Animated GIF, APNG, WebP, AVIF and JPEG XL with alpha, frames decoded in parallel", plugin_init, "1.0", "LGPL",
    "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
