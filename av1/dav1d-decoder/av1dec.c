/* cutepidav1ddec: AV1 decoding with dav1d, as a GStreamer video decoder.
 *
 * GStreamer on Debian trixie has only libaom for AV1 (av1dec, 34 fps at 1080p on a Pi 4); dav1d decodes the same
 * stream at 134 fps, but its GStreamer element (gst-plugins-rs dav1ddec) is not packaged. libdav1d.so.7 is (FFmpeg
 * pulls it in), so this element loads it at run time (dlopen) against the vendored 1.5.1 headers
 * (third_party/dav1d): CuTePi builds without dav1d and the element registers only where the library is.
 *
 * One temporal unit per input buffer (video/x-av1 obu-stream, alignment tu: av1parse makes that from any
 * container). GstVideoDecoder's frame number travels with the data (Dav1dDataProps.offset) and comes back on the
 * picture, which may be later (frame threading) or never (frames that are not shown). Pictures are copied into
 * the output buffer plane by plane. */
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include "dav1d/dav1d.h"

GST_DEBUG_CATEGORY_STATIC (av1dec_debug);
#define GST_CAT_DEFAULT av1dec_debug

/* libdav1d entry points, resolved once. */
static struct {
  const char *(*version) (void);
  void (*default_settings) (Dav1dSettings *);
  int (*open) (Dav1dContext **, const Dav1dSettings *);
  int (*send_data) (Dav1dContext *, Dav1dData *);
  int (*get_picture) (Dav1dContext *, Dav1dPicture *);
  void (*close) (Dav1dContext **);
  void (*flush) (Dav1dContext *);
  uint8_t *(*data_create) (Dav1dData *, size_t);
  void (*data_unref) (Dav1dData *);
  void (*picture_unref) (Dav1dPicture *);
} lib;

typedef struct {
  GstVideoDecoder parent;
  Dav1dContext *ctx;
  gint n_threads, max_frame_delay;
  gint out_w, out_h, out_layout, out_bpc;       /* the negotiated output, to renegotiate on change */
  GstVideoCodecState *input_state;              /* frame rate and pixel aspect for the output caps */
  gboolean zero_copy;                           /* downstream takes GstVideoMeta: hand out dav1d's own buffers */
  gboolean allow_zero_copy;                     /* property zero-copy (off: always copy, for comparison) */
} CutepiDav1dDec;
typedef struct { GstVideoDecoderClass parent; } CutepiDav1dDecClass;

GType cutepi_dav1d_dec_get_type (void);
G_DEFINE_TYPE (CutepiDav1dDec, cutepi_dav1d_dec, GST_TYPE_VIDEO_DECODER);

enum { PROP_0, PROP_N_THREADS, PROP_MAX_FRAME_DELAY, PROP_ZERO_COPY };

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-av1, stream-format=(string)obu-stream, alignment=(string)tu"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string){ I420, I420_10LE, Y42B, I422_10LE, Y444, Y444_10LE, GRAY8, GRAY10_LE16 }"));

/* dav1d decodes into GstMemory we allocate (same layout as its default allocator: 128-aligned size, 64-aligned
 * strides, +64 bytes on strides that are a multiple of 1024), so a finished picture can go downstream without a
 * copy. dav1d keeps reading a picture while it is a reference; downstream only reads too. */
static int
alloc_picture (Dav1dPicture * p, void *cookie)
{
  int hbd = p->p.bpc > 8;
  int aw = (p->p.w + 127) & ~127, ah = (p->p.h + 127) & ~127;
  int ss_ver = p->p.layout == DAV1D_PIXEL_LAYOUT_I420;
  int ss_hor = p->p.layout != DAV1D_PIXEL_LAYOUT_I444;
  ptrdiff_t ys = aw << hbd, uvs = p->p.layout != DAV1D_PIXEL_LAYOUT_I400 ? ys >> ss_hor : 0;
  if (!(ys & 1023))
    ys += DAV1D_PICTURE_ALIGNMENT;
  if (uvs && !(uvs & 1023))
    uvs += DAV1D_PICTURE_ALIGNMENT;
  size_t ysz = (size_t) ys * ah, uvsz = (size_t) uvs * (ah >> ss_ver);
  GstAllocationParams ap = { .align = DAV1D_PICTURE_ALIGNMENT - 1 };
  GstMemory *m = gst_allocator_alloc (NULL, ysz + 2 * uvsz + DAV1D_PICTURE_ALIGNMENT, &ap);
  if (!m)
    return DAV1D_ERR (ENOMEM);
  GstMapInfo mi;
  gst_memory_map (m, &mi, GST_MAP_READWRITE);
  p->data[0] = mi.data;
  p->data[1] = uvs ? mi.data + ysz : NULL;
  p->data[2] = uvs ? mi.data + ysz + uvsz : NULL;
  p->stride[0] = ys;
  p->stride[1] = uvs;
  gst_memory_unmap (m, &mi);    /* system memory: the pointer stays valid while the memory lives */
  p->allocator_data = m;
  return 0;
}

static void
release_picture (Dav1dPicture * p, void *cookie)
{
  gst_memory_unref ((GstMemory *) p->allocator_data);
}

static gboolean
d_decide_allocation (GstVideoDecoder * dec, GstQuery * q)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) dec;
  self->zero_copy = self->allow_zero_copy && gst_query_find_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL);
  GST_INFO_OBJECT (self, "downstream %s GstVideoMeta: %s", self->zero_copy ? "takes" : "lacks",
      self->zero_copy ? "zero-copy output" : "copying each picture");
  return GST_VIDEO_DECODER_CLASS (cutepi_dav1d_dec_parent_class)->decide_allocation (dec, q);
}

static gboolean
d_start (GstVideoDecoder * dec)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) dec;
  Dav1dSettings s;
  lib.default_settings (&s);
  s.n_threads = self->n_threads;        /* 0: one per core */
  s.max_frame_delay = self->max_frame_delay;
  s.apply_grain = 1;
  s.allocator.cookie = self;
  s.allocator.alloc_picture_callback = alloc_picture;
  s.allocator.release_picture_callback = release_picture;
  int r = lib.open (&self->ctx, &s);
  if (r < 0) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, ("dav1d_open failed: %d", r), (NULL));
    return FALSE;
  }
  self->out_w = self->out_h = self->out_layout = self->out_bpc = -1;
  return TRUE;
}

static gboolean
d_stop (GstVideoDecoder * dec)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) dec;
  if (self->ctx)
    lib.close (&self->ctx);
  g_clear_pointer (&self->input_state, gst_video_codec_state_unref);
  return TRUE;
}

static gboolean
d_set_format (GstVideoDecoder * dec, GstVideoCodecState * state)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) dec;
  g_clear_pointer (&self->input_state, gst_video_codec_state_unref);
  self->input_state = gst_video_codec_state_ref (state);
  self->out_w = -1;     /* renegotiate on the next picture */
  return TRUE;
}

static gboolean
d_flush (GstVideoDecoder * dec)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) dec;
  if (self->ctx)
    lib.flush (self->ctx);
  return TRUE;
}

static GstVideoFormat
picture_format (const Dav1dPicture * p)
{
  gboolean hi = p->p.bpc > 8;
  switch (p->p.layout) {
    case DAV1D_PIXEL_LAYOUT_I400: return hi ? GST_VIDEO_FORMAT_GRAY10_LE16 : GST_VIDEO_FORMAT_GRAY8;
    case DAV1D_PIXEL_LAYOUT_I420: return hi ? GST_VIDEO_FORMAT_I420_10LE : GST_VIDEO_FORMAT_I420;
    case DAV1D_PIXEL_LAYOUT_I422: return hi ? GST_VIDEO_FORMAT_I422_10LE : GST_VIDEO_FORMAT_Y42B;
    case DAV1D_PIXEL_LAYOUT_I444: return hi ? GST_VIDEO_FORMAT_Y444_10LE : GST_VIDEO_FORMAT_Y444;
  }
  return GST_VIDEO_FORMAT_UNKNOWN;
}

/* Colour description from the sequence header, so downstream converts with the right matrix and range. */
static void
set_colorimetry (GstVideoCodecState * st, const Dav1dPicture * p)
{
  const Dav1dSequenceHeader *sh = p->seq_hdr;
  if (!sh)
    return;
  GstVideoColorimetry *c = &st->info.colorimetry;
  c->range = sh->color_range ? GST_VIDEO_COLOR_RANGE_0_255 : GST_VIDEO_COLOR_RANGE_16_235;
  if (sh->mtrx != DAV1D_MC_UNKNOWN)
    c->matrix = gst_video_color_matrix_from_iso (sh->mtrx);
  if (sh->trc != DAV1D_TRC_UNKNOWN)
    c->transfer = gst_video_transfer_function_from_iso (sh->trc);
  if (sh->pri != DAV1D_COLOR_PRI_UNKNOWN)
    c->primaries = gst_video_color_primaries_from_iso (sh->pri);
}

/* Hand one decoded picture to its GstVideoCodecFrame (copying the planes) and finish it. */
static GstFlowReturn
output_picture (CutepiDav1dDec * self, Dav1dPicture * p)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  GstVideoCodecFrame *frame = gst_video_decoder_get_frame (dec, (int) p->m.offset);
  GstFlowReturn ret = GST_FLOW_OK;
  if (!frame) {
    GST_DEBUG_OBJECT (self, "picture for unknown frame %" G_GINT64_FORMAT, p->m.offset);
    lib.picture_unref (p);
    return GST_FLOW_OK;
  }
  if (p->p.w != self->out_w || p->p.h != self->out_h || (int) p->p.layout != self->out_layout || p->p.bpc != self->out_bpc) {
    GstVideoFormat fmt = picture_format (p);
    if (fmt == GST_VIDEO_FORMAT_UNKNOWN) {
      lib.picture_unref (p);
      gst_video_decoder_release_frame (dec, frame);
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("unsupported AV1 layout %d / %d bits", p->p.layout, p->p.bpc), (NULL));
      return GST_FLOW_ERROR;
    }
    GstVideoCodecState *st = gst_video_decoder_set_output_state (dec, fmt, p->p.w, p->p.h, self->input_state);
    set_colorimetry (st, p);
    gst_video_codec_state_unref (st);
    if (!gst_video_decoder_negotiate (dec)) {
      lib.picture_unref (p);
      gst_video_decoder_release_frame (dec, frame);
      return GST_FLOW_NOT_NEGOTIATED;
    }
    self->out_w = p->p.w;
    self->out_h = p->p.h;
    self->out_layout = p->p.layout;
    self->out_bpc = p->p.bpc;
  }
  if (self->zero_copy) {
    /* Wrap dav1d's picture: a new buffer holding a reference to its memory, the layout in a GstVideoMeta. */
    GstMemory *m = p->allocator_data;
    GstBuffer *out = gst_buffer_new ();
    gst_buffer_append_memory (out, gst_memory_ref (m));
    GstMapInfo mi;
    gst_memory_map (m, &mi, GST_MAP_READ);
    gsize off[GST_VIDEO_MAX_PLANES] = { 0 };
    gint str[GST_VIDEO_MAX_PLANES] = { 0 };
    int np = p->p.layout == DAV1D_PIXEL_LAYOUT_I400 ? 1 : 3;
    for (int i = 0; i < np; i++) {
      off[i] = (const uint8_t *) p->data[i] - mi.data;
      str[i] = (gint) p->stride[i ? 1 : 0];
    }
    gst_memory_unmap (m, &mi);
    gst_buffer_add_video_meta_full (out, GST_VIDEO_FRAME_FLAG_NONE, picture_format (p), p->p.w, p->p.h, np, off, str);
    frame->output_buffer = out;
    lib.picture_unref (p);
    return gst_video_decoder_finish_frame (dec, frame);
  }
  ret = gst_video_decoder_allocate_output_frame (dec, frame);
  if (ret != GST_FLOW_OK) {
    lib.picture_unref (p);
    gst_video_decoder_release_frame (dec, frame);
    return ret;
  }
  GstVideoCodecState *st = gst_video_decoder_get_output_state (dec);
  GstVideoFrame vf;
  if (!gst_video_frame_map (&vf, &st->info, frame->output_buffer, GST_MAP_WRITE)) {
    gst_video_codec_state_unref (st);
    lib.picture_unref (p);
    gst_video_decoder_release_frame (dec, frame);
    return GST_FLOW_ERROR;
  }
  gst_video_codec_state_unref (st);
  int bytes = p->p.bpc > 8 ? 2 : 1;
  int ss_hor = p->p.layout == DAV1D_PIXEL_LAYOUT_I420 || p->p.layout == DAV1D_PIXEL_LAYOUT_I422;
  int ss_ver = p->p.layout == DAV1D_PIXEL_LAYOUT_I420;
  int planes = p->p.layout == DAV1D_PIXEL_LAYOUT_I400 ? 1 : 3;
  for (int i = 0; i < planes; i++) {
    int w = i ? (p->p.w + ss_hor) >> ss_hor : p->p.w;
    int h = i ? (p->p.h + ss_ver) >> ss_ver : p->p.h;
    const uint8_t *src = p->data[i];
    ptrdiff_t sstride = p->stride[i ? 1 : 0];
    uint8_t *dst = GST_VIDEO_FRAME_PLANE_DATA (&vf, i);
    int dstride = GST_VIDEO_FRAME_PLANE_STRIDE (&vf, i);
    size_t row = (size_t) w * bytes;
    if (sstride == dstride && (ptrdiff_t) row == sstride)
      memcpy (dst, src, row * h);
    else
      for (int y = 0; y < h; y++)
        memcpy (dst + (size_t) y * dstride, src + y * sstride, row);
  }
  gst_video_frame_unmap (&vf);
  lib.picture_unref (p);
  return gst_video_decoder_finish_frame (dec, frame);
}

/* Collect every picture dav1d has ready. Returns OK when it wants more data. */
static GstFlowReturn
drain_pictures (CutepiDav1dDec * self)
{
  for (;;) {
    Dav1dPicture p = { 0 };
    int r = lib.get_picture (self->ctx, &p);
    if (r == DAV1D_ERR (EAGAIN))
      return GST_FLOW_OK;
    if (r < 0) {
      GstFlowReturn ret = GST_FLOW_OK;
      GST_VIDEO_DECODER_ERROR (self, 1, STREAM, DECODE, ("dav1d_get_picture failed"), ("error %d", r), ret);
      return ret;
    }
    GstFlowReturn ret = output_picture (self, &p);
    if (ret != GST_FLOW_OK)
      return ret;
  }
}

static GstFlowReturn
d_handle_frame (GstVideoDecoder * dec, GstVideoCodecFrame * frame)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) dec;
  GstMapInfo mi;
  if (!gst_buffer_map (frame->input_buffer, &mi, GST_MAP_READ)) {
    gst_video_decoder_release_frame (dec, frame);
    return GST_FLOW_ERROR;
  }
  Dav1dData d = { 0 };
  uint8_t *buf = lib.data_create (&d, mi.size);
  if (!buf) {
    gst_buffer_unmap (frame->input_buffer, &mi);
    gst_video_decoder_release_frame (dec, frame);
    return GST_FLOW_ERROR;
  }
  memcpy (buf, mi.data, mi.size);
  gst_buffer_unmap (frame->input_buffer, &mi);
  d.m.offset = frame->system_frame_number;
  d.m.timestamp = GST_CLOCK_TIME_IS_VALID (frame->pts) ? (int64_t) frame->pts : INT64_MIN;
  gst_video_codec_frame_unref (frame);  /* the decoder keeps its own reference until the picture comes out */

  GstFlowReturn ret = GST_FLOW_OK;
  while (d.sz > 0) {
    int r = lib.send_data (self->ctx, &d);
    if (r < 0 && r != DAV1D_ERR (EAGAIN)) {
      lib.data_unref (&d);
      GST_VIDEO_DECODER_ERROR (self, 1, STREAM, DECODE, ("dav1d_send_data failed"), ("error %d", r), ret);
      return ret;
    }
    /* EAGAIN: dav1d holds pictures it must hand out before it takes more data. */
    ret = drain_pictures (self);
    if (ret != GST_FLOW_OK) {
      lib.data_unref (&d);
      return ret;
    }
  }
  return GST_FLOW_OK;
}

/* End of stream: with no more data, dav1d returns its remaining pictures until EAGAIN. */
static GstFlowReturn
d_drain (GstVideoDecoder * dec)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) dec;
  return self->ctx ? drain_pictures (self) : GST_FLOW_OK;
}

static void
d_set_property (GObject * o, guint id, const GValue * v, GParamSpec * ps)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) o;
  if (id == PROP_N_THREADS)
    self->n_threads = g_value_get_int (v);
  else if (id == PROP_MAX_FRAME_DELAY)
    self->max_frame_delay = g_value_get_int (v);
  else if (id == PROP_ZERO_COPY)
    self->allow_zero_copy = g_value_get_boolean (v);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
d_get_property (GObject * o, guint id, GValue * v, GParamSpec * ps)
{
  CutepiDav1dDec *self = (CutepiDav1dDec *) o;
  if (id == PROP_N_THREADS)
    g_value_set_int (v, self->n_threads);
  else if (id == PROP_MAX_FRAME_DELAY)
    g_value_set_int (v, self->max_frame_delay);
  else if (id == PROP_ZERO_COPY)
    g_value_set_boolean (v, self->allow_zero_copy);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
cutepi_dav1d_dec_class_init (CutepiDav1dDecClass * k)
{
  GObjectClass *ok = G_OBJECT_CLASS (k);
  GstElementClass *ek = GST_ELEMENT_CLASS (k);
  GstVideoDecoderClass *dk = GST_VIDEO_DECODER_CLASS (k);
  ok->set_property = d_set_property;
  ok->get_property = d_get_property;
  g_object_class_install_property (ok, PROP_N_THREADS, g_param_spec_int ("n-threads", "Threads",
          "dav1d worker threads (0: one per core)", 0, 256, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (ok, PROP_MAX_FRAME_DELAY, g_param_spec_int ("max-frame-delay", "Max frame delay",
          "frames decoded in parallel (0: automatic)", 0, 256, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (ok, PROP_ZERO_COPY, g_param_spec_boolean ("zero-copy", "Zero copy",
          "hand dav1d's picture buffers downstream when it takes GstVideoMeta (off: copy every picture)", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  gst_element_class_add_static_pad_template (ek, &sink_tmpl);
  gst_element_class_add_static_pad_template (ek, &src_tmpl);
  gst_element_class_set_static_metadata (ek, "AV1 decoder (dav1d)", "Codec/Decoder/Video",
      "Decodes AV1 with libdav1d, loaded at run time", "CuTePi");
  dk->start = d_start;
  dk->stop = d_stop;
  dk->flush = d_flush;
  dk->set_format = d_set_format;
  dk->decide_allocation = d_decide_allocation;
  dk->handle_frame = d_handle_frame;
  dk->drain = d_drain;
  dk->finish = d_drain;
  GST_DEBUG_CATEGORY_INIT (av1dec_debug, "cutepidav1ddec", 0, "dav1d AV1 decoder");
}

static void
cutepi_dav1d_dec_init (CutepiDav1dDec * self)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
  gst_video_decoder_set_needs_format (GST_VIDEO_DECODER (self), TRUE);
  self->allow_zero_copy = TRUE;
}

/* Loads libdav1d.so.7; 0 if it is missing or lacks an entry point. */
static int
load_lib (void)
{
  void *h = dlopen ("libdav1d.so.7", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    return 0;
#define SYM(field, name) if (!(*(void **) &lib.field = dlsym (h, name))) return 0
  SYM (version, "dav1d_version");
  SYM (default_settings, "dav1d_default_settings");
  SYM (open, "dav1d_open");
  SYM (send_data, "dav1d_send_data");
  SYM (get_picture, "dav1d_get_picture");
  SYM (close, "dav1d_close");
  SYM (flush, "dav1d_flush");
  SYM (data_create, "dav1d_data_create");
  SYM (data_unref, "dav1d_data_unref");
  SYM (picture_unref, "dav1d_picture_unref");
#undef SYM
  return 1;
}

/* Registers cutepidav1ddec in this process (plugin NULL) or in a plugin; returns 0 when libdav1d is absent. */
int
cutepi_av1dec_register (GstPlugin * plugin, int rank)
{
  static int loaded = -1;
  if (loaded < 0)
    loaded = load_lib ();
  if (!loaded)
    return 0;
  return gst_element_register (plugin, "cutepidav1ddec", rank, cutepi_dav1d_dec_get_type ());
}

const char *
cutepi_av1dec_version (void)
{
  return lib.version ? lib.version () : "";
}

#ifdef AV1DEC_PLUGIN
#define PACKAGE "cutepidav1d"
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_av1dec_register (p, GST_RANK_PRIMARY + 1);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepidav1d, "AV1 decoding with dav1d", plugin_init, "1.0",
    "LGPL", "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
