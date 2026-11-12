/* cutepipngvdec: PNG video (PNG frames in MOV/MKV) decoded on several cores
 * (DESIGN §12.16).
 *
 * decodebin picks pngdec, which decodes one frame at a time: 21-22 ms a 1080p
 * RGBA frame on a Pi 4, so a 1080p60 PNG video (alpha or not) played at 33-39
 * fps. Every frame is a complete PNG, so N workers decode frames in parallel
 * with libpng's simplified API (straight into the output buffer: any colour
 * type, palette transparency and 16-bit to 8-bit straight-alpha RGBA) and the
 * element finishes them in order. libpng16 is loaded at run time (the runtime
 * package has no headers); stills (framerate 0/1) are not taken: they keep
 * pngdec. */
#include <dlfcn.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (pngvdec_debug);
#define GST_CAT_DEFAULT pngvdec_debug

#define MAX_WORKERS 4

/* libpng 1.6's simplified API (png.h: png_image, PNG_IMAGE_VERSION 1). */
typedef struct {
  void *opaque;
  guint32 version, width, height, format, flags, colormap_entries, warning_or_error;
  char message[64];
} png_image_t;
#define PNG_IMAGE_VERSION_1 1
#define PNG_FORMAT_RGBA_ 3 /* PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_ALPHA: straight alpha */

static struct {
  int (*begin) (png_image_t *, const void *, size_t);
  int (*finish) (png_image_t *, const void *, void *, gint32, void *);
  void (*free) (png_image_t *);
} L;

typedef struct _CutepiPngvDec CutepiPngvDec;

typedef struct {
  GstVideoCodecFrame *frame;
  GstMapInfo in;
  GstVideoFrame out;
  gboolean done, ok;
  char message[64];
} Job;

typedef struct {
  CutepiPngvDec *self;
  GThread *thread;
  Job *job;
  gboolean quit;
} Worker;

struct _CutepiPngvDec {
  GstVideoDecoder parent;
  gint n_workers;
  Worker workers[MAX_WORKERS];
  guint64 seq;
  GQueue pending;
  GMutex lock;
  GCond cond;
  GstVideoCodecState *input_state;
  gboolean negotiated;
};

typedef struct { GstVideoDecoderClass parent; } CutepiPngvDecClass;
GType cutepi_pngv_dec_get_type (void);
G_DEFINE_TYPE (CutepiPngvDec, cutepi_pngv_dec, GST_TYPE_VIDEO_DECODER);

enum { PROP_0, PROP_N_WORKERS };

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/png, width=(int)[1, 16384], height=(int)[1, 16384], framerate=(fraction)[1/10000, 10000/1]"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string)RGBA"));

static gpointer
worker_main (gpointer data)
{
  Worker *w = data;
  CutepiPngvDec *self = w->self;
  g_mutex_lock (&self->lock);
  for (;;) {
    while (!w->quit && !w->job)
      g_cond_wait (&self->cond, &self->lock);
    if (w->quit) break;
    Job *j = w->job;
    g_mutex_unlock (&self->lock);
    png_image_t im = { 0 };
    im.version = PNG_IMAGE_VERSION_1;
    gboolean ok = L.begin (&im, j->in.data, j->in.size) &&
        im.width == (guint32) GST_VIDEO_FRAME_WIDTH (&j->out) && im.height == (guint32) GST_VIDEO_FRAME_HEIGHT (&j->out);
    if (ok) {
      im.format = PNG_FORMAT_RGBA_;
      ok = L.finish (&im, NULL, GST_VIDEO_FRAME_PLANE_DATA (&j->out, 0), GST_VIDEO_FRAME_PLANE_STRIDE (&j->out, 0), NULL);
    }
    if (!ok) g_strlcpy (j->message, im.message[0] ? im.message : "size differs from the stream", sizeof (j->message));
    L.free (&im);
    g_mutex_lock (&self->lock);
    j->ok = ok;
    j->done = TRUE;
    w->job = NULL;
    g_cond_broadcast (&self->cond);
  }
  g_mutex_unlock (&self->lock);
  return NULL;
}

static gboolean
p_start (GstVideoDecoder * dec)
{
  CutepiPngvDec *self = (CutepiPngvDec *) dec;
  self->negotiated = FALSE;
  for (int i = 0; i < self->n_workers; i++) {
    Worker *w = &self->workers[i];
    w->self = self; w->quit = FALSE; w->job = NULL;
    w->thread = g_thread_new ("pngv-decode", worker_main, w);
  }
  return TRUE;
}

static void
job_free (CutepiPngvDec * self, Job * j)
{
  gst_video_frame_unmap (&j->out);
  gst_buffer_unmap (j->frame->input_buffer, &j->in);
  g_free (j);
}

static void
wait_all (CutepiPngvDec * self)
{
  g_mutex_lock (&self->lock);
  for (GList * l = self->pending.head; l; l = l->next)
    while (!((Job *) l->data)->done)
      g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);
}

static gboolean
p_flush (GstVideoDecoder * dec)
{
  CutepiPngvDec *self = (CutepiPngvDec *) dec;
  wait_all (self);
  Job *j;
  while ((j = g_queue_pop_head (&self->pending))) {
    GstVideoCodecFrame *f = j->frame;
    job_free (self, j);
    gst_video_decoder_release_frame (dec, f);
  }
  return TRUE;
}

static gboolean
p_stop (GstVideoDecoder * dec)
{
  CutepiPngvDec *self = (CutepiPngvDec *) dec;
  p_flush (dec);
  g_mutex_lock (&self->lock);
  for (int i = 0; i < self->n_workers; i++) self->workers[i].quit = TRUE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  for (int i = 0; i < self->n_workers; i++)
    if (self->workers[i].thread) { g_thread_join (self->workers[i].thread); self->workers[i].thread = NULL; }
  if (self->input_state) { gst_video_codec_state_unref (self->input_state); self->input_state = NULL; }
  return TRUE;
}

static gboolean
p_set_format (GstVideoDecoder * dec, GstVideoCodecState * state)
{
  CutepiPngvDec *self = (CutepiPngvDec *) dec;
  if (self->input_state) gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);
  GstVideoCodecState *st = gst_video_decoder_set_output_state (dec, GST_VIDEO_FORMAT_RGBA,
      state->info.width, state->info.height, state);
  gst_video_codec_state_unref (st);
  self->negotiated = gst_video_decoder_negotiate (dec);
  return self->negotiated;
}

static GstFlowReturn
finish_oldest (CutepiPngvDec * self)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  g_mutex_lock (&self->lock);
  Job *j = g_queue_pop_head (&self->pending);
  if (!j) { g_mutex_unlock (&self->lock); return GST_FLOW_OK; }
  while (!j->done)
    g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);
  GstVideoCodecFrame *f = j->frame;
  gboolean ok = j->ok;
  if (!ok) GST_WARNING_OBJECT (self, "frame %u: %s", f->system_frame_number, j->message);
  job_free (self, j);
  return ok ? gst_video_decoder_finish_frame (dec, f) : gst_video_decoder_drop_frame (dec, f);
}

static GstFlowReturn
finish_all (CutepiPngvDec * self)
{
  GstFlowReturn ret = GST_FLOW_OK;
  while (self->pending.length) {
    GstFlowReturn r = finish_oldest (self);
    if (r != GST_FLOW_OK && ret == GST_FLOW_OK) ret = r;
  }
  return ret;
}

static GstFlowReturn
p_handle_frame (GstVideoDecoder * dec, GstVideoCodecFrame * frame)
{
  CutepiPngvDec *self = (CutepiPngvDec *) dec;
  Worker *w = &self->workers[self->seq++ % self->n_workers];
  for (;;) {
    g_mutex_lock (&self->lock);
    gboolean busy = w->job != NULL || self->pending.length >= (guint) self->n_workers;
    g_mutex_unlock (&self->lock);
    if (!busy) break;
    GstFlowReturn r = finish_oldest (self);
    if (r != GST_FLOW_OK) {
      gst_video_decoder_release_frame (dec, frame);
      return r;
    }
  }
  GstFlowReturn a = gst_video_decoder_allocate_output_frame (dec, frame);
  if (a != GST_FLOW_OK) {
    gst_video_decoder_release_frame (dec, frame);
    return a;
  }
  Job *j = g_new0 (Job, 1);
  j->frame = frame;
  GstVideoCodecState *st = gst_video_decoder_get_output_state (dec);
  gboolean mapped = gst_buffer_map (frame->input_buffer, &j->in, GST_MAP_READ);
  if (mapped && !gst_video_frame_map (&j->out, &st->info, frame->output_buffer, GST_MAP_WRITE)) {
    gst_buffer_unmap (frame->input_buffer, &j->in);
    mapped = FALSE;
  }
  gst_video_codec_state_unref (st);
  if (!mapped) {
    g_free (j);
    return gst_video_decoder_drop_frame (dec, frame);
  }
  g_mutex_lock (&self->lock);
  g_queue_push_tail (&self->pending, j);
  w->job = j;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  return GST_FLOW_OK;
}

static GstFlowReturn
p_finish (GstVideoDecoder * dec)
{
  return finish_all ((CutepiPngvDec *) dec);
}

static gboolean
p_decide_allocation (GstVideoDecoder * dec, GstQuery * q)
{
  /* Enough output buffers for every worker plus what downstream holds. */
  if (!GST_VIDEO_DECODER_CLASS (cutepi_pngv_dec_parent_class)->decide_allocation (dec, q)) return FALSE;
  if (gst_query_get_n_allocation_pools (q) > 0) {
    GstBufferPool *pool; guint size, min, max;
    gst_query_parse_nth_allocation_pool (q, 0, &pool, &size, &min, &max);
    min = MAX (min, (guint) ((CutepiPngvDec *) dec)->n_workers + 4);
    if (max && max < min) max = min;
    gst_query_set_nth_allocation_pool (q, 0, pool, size, min, max);
    if (pool) gst_object_unref (pool);
  }
  return TRUE;
}

static void
p_set_property (GObject * o, guint id, const GValue * v, GParamSpec * ps)
{
  if (id == PROP_N_WORKERS) ((CutepiPngvDec *) o)->n_workers = g_value_get_int (v);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
p_get_property (GObject * o, guint id, GValue * v, GParamSpec * ps)
{
  if (id == PROP_N_WORKERS) g_value_set_int (v, ((CutepiPngvDec *) o)->n_workers);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
p_finalize (GObject * o)
{
  CutepiPngvDec *self = (CutepiPngvDec *) o;
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  G_OBJECT_CLASS (cutepi_pngv_dec_parent_class)->finalize (o);
}

static void
cutepi_pngv_dec_class_init (CutepiPngvDecClass * k)
{
  GObjectClass *ok = G_OBJECT_CLASS (k);
  GstElementClass *ek = GST_ELEMENT_CLASS (k);
  GstVideoDecoderClass *dk = GST_VIDEO_DECODER_CLASS (k);
  ok->set_property = p_set_property;
  ok->get_property = p_get_property;
  ok->finalize = p_finalize;
  g_object_class_install_property (ok, PROP_N_WORKERS, g_param_spec_int ("n-workers", "Workers",
          "Frames decoded in parallel", 1, MAX_WORKERS, 4, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  gst_element_class_add_static_pad_template (ek, &sink_tmpl);
  gst_element_class_add_static_pad_template (ek, &src_tmpl);
  gst_element_class_set_static_metadata (ek, "CuTePi PNG video decoder", "Codec/Decoder/Video",
      "PNG frames of a video decoded in parallel by libpng, in order, to straight-alpha RGBA", "CuTePi");
  dk->start = p_start;
  dk->stop = p_stop;
  dk->set_format = p_set_format;
  dk->handle_frame = p_handle_frame;
  dk->finish = p_finish;
  dk->drain = p_finish;
  dk->flush = p_flush;
  dk->decide_allocation = p_decide_allocation;
}

static void
cutepi_pngv_dec_init (CutepiPngvDec * self)
{
  self->n_workers = 4;
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  g_queue_init (&self->pending);
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
}

int
cutepi_pngvdec_register (GstPlugin * plugin, int rank)
{
  GST_DEBUG_CATEGORY_INIT (pngvdec_debug, "cutepipngvdec", 0, "CuTePi PNG video decoder");
  void *h = dlopen ("libpng16.so.16", RTLD_NOW | RTLD_LOCAL);
  if (!h || !(*(void **) &L.begin = dlsym (h, "png_image_begin_read_from_memory")) ||
      !(*(void **) &L.finish = dlsym (h, "png_image_finish_read")) || !(*(void **) &L.free = dlsym (h, "png_image_free")))
    return 0;
  return gst_element_register (plugin, "cutepipngvdec", rank, cutepi_pngv_dec_get_type ());
}

#ifdef STANDALONE_PLUGIN
/* Standalone GStreamer plugin build (see this folder's Makefile). */
#ifndef PACKAGE
#define PACKAGE "cutepipngvdec"
#endif
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_pngvdec_register (p, GST_RANK_PRIMARY + 1);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepipngvdec, "PNG video decoded on several cores by libpng", plugin_init, "1.0", "LGPL",
    "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
