/* cutepimjpegdec: Motion JPEG decoded on several cores (DESIGN §12.16).
 *
 * GStreamer's jpegdec (libjpeg-turbo) decodes one frame at a time: 8.1 ms a
 * 1080p 4:2:0 frame and 10.3 ms a high-quality 4:2:2 one on a Pi 4, 12.5 ms in
 * the service beside the GPU wall, so a 1080p60 camera stream sits close to the
 * limit and a second one cannot play. Every MJPEG frame is a complete JPEG, so
 * this element hands frames round-robin to N workers, each driving its own
 * jpegdec in a small internal pipeline (appsrc ! jpegdec ! appsink), and
 * finishes them in decode order. The decoded buffers go downstream as they
 * are: no copy. Stills (framerate 0/1) are not taken: they keep jpegdec. */
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC (mjpegdec_debug);
#define GST_CAT_DEFAULT mjpegdec_debug

#define MAX_WORKERS 4

typedef struct _CutepiMjpegDec CutepiMjpegDec;

typedef struct {
  GstVideoCodecFrame *frame;
  GstBuffer *in;     /* the JPEG */
  GstSample *out;    /* jpegdec's picture */
  gboolean done;
} Job;

typedef struct {
  CutepiMjpegDec *self;
  GThread *thread;
  GstElement *pipe, *src, *sink;
  Job *job;          /* handed to this worker, NULL when idle */
  gboolean quit;
} Worker;

struct _CutepiMjpegDec {
  GstVideoDecoder parent;
  gint n_workers;
  Worker workers[MAX_WORKERS];
  guint64 seq;       /* frames handed out */
  GQueue pending;    /* Jobs in decode order */
  GMutex lock;
  GCond cond;
  GstVideoCodecState *input_state;
  GstCaps *out_caps; /* jpegdec's last output caps (the output state follows them) */
};

typedef struct { GstVideoDecoderClass parent; } CutepiMjpegDecClass;
GType cutepi_mjpeg_dec_get_type (void);
G_DEFINE_TYPE (CutepiMjpegDec, cutepi_mjpeg_dec, GST_TYPE_VIDEO_DECODER);

enum { PROP_0, PROP_N_WORKERS };

/* Moving pictures only: a still (framerate 0/1) stays with jpegdec. */
static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg, parsed=(boolean)true, framerate=(fraction)[1/10000, 10000/1]"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string){ I420, Y42B, Y444, GRAY8, RGB, BGR, xRGB, xBGR, RGBx, BGRx }"));

static gpointer
worker_main (gpointer data)
{
  Worker *w = data;
  CutepiMjpegDec *self = w->self;
  g_mutex_lock (&self->lock);
  for (;;) {
    while (!w->quit && !w->job)
      g_cond_wait (&self->cond, &self->lock);
    if (w->quit)
      break;
    Job *j = w->job;
    g_mutex_unlock (&self->lock);
    GstSample *out = NULL;
    if (gst_app_src_push_buffer (GST_APP_SRC (w->src), gst_buffer_ref (j->in)) == GST_FLOW_OK)
      out = gst_app_sink_try_pull_sample (GST_APP_SINK (w->sink), 5 * GST_SECOND);
    g_mutex_lock (&self->lock);
    j->out = out;
    j->done = TRUE;
    w->job = NULL;
    g_cond_broadcast (&self->cond);
  }
  g_mutex_unlock (&self->lock);
  return NULL;
}

static gboolean
m_start (GstVideoDecoder * dec)
{
  CutepiMjpegDec *self = (CutepiMjpegDec *) dec;
  for (int i = 0; i < self->n_workers; i++) {
    Worker *w = &self->workers[i];
    GError *e = NULL;
    w->self = self;
    w->quit = FALSE;
    w->job = NULL;
    w->pipe = gst_parse_launch ("appsrc name=s format=time ! jpegdec ! appsink name=k sync=false max-buffers=2", &e);
    if (!w->pipe) {
      GST_ERROR_OBJECT (self, "jpegdec pipeline: %s", e ? e->message : "?");
      g_clear_error (&e);
      return FALSE;
    }
    w->src = gst_bin_get_by_name (GST_BIN (w->pipe), "s");
    w->sink = gst_bin_get_by_name (GST_BIN (w->pipe), "k");
    gst_element_set_state (w->pipe, GST_STATE_PLAYING);
    w->thread = g_thread_new ("mjpeg-decode", worker_main, w);
  }
  return TRUE;
}

static void
job_free (Job * j)
{
  if (j->in) gst_buffer_unref (j->in);
  if (j->out) gst_sample_unref (j->out);
  g_free (j);
}

/* Wait for every handed-out job (no frames are finished). */
static void
wait_all (CutepiMjpegDec * self)
{
  g_mutex_lock (&self->lock);
  for (GList * l = self->pending.head; l; l = l->next)
    while (!((Job *) l->data)->done)
      g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);
}

static gboolean
m_stop (GstVideoDecoder * dec)
{
  CutepiMjpegDec *self = (CutepiMjpegDec *) dec;
  wait_all (self);
  Job *j;
  while ((j = g_queue_pop_head (&self->pending))) {
    gst_video_decoder_release_frame (dec, j->frame);
    job_free (j);
  }
  g_mutex_lock (&self->lock);
  for (int i = 0; i < self->n_workers; i++) self->workers[i].quit = TRUE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  for (int i = 0; i < self->n_workers; i++) {
    Worker *w = &self->workers[i];
    if (w->thread) { g_thread_join (w->thread); w->thread = NULL; }
    if (w->pipe) {
      gst_element_set_state (w->pipe, GST_STATE_NULL);
      gst_clear_object (&w->src);
      gst_clear_object (&w->sink);
      gst_clear_object (&w->pipe);
    }
  }
  if (self->input_state) { gst_video_codec_state_unref (self->input_state); self->input_state = NULL; }
  gst_clear_caps (&self->out_caps);
  return TRUE;
}

static gboolean
m_set_format (GstVideoDecoder * dec, GstVideoCodecState * state)
{
  CutepiMjpegDec *self = (CutepiMjpegDec *) dec;
  if (self->input_state) gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);
  for (int i = 0; i < self->n_workers; i++)
    if (self->workers[i].src) g_object_set (self->workers[i].src, "caps", state->caps, NULL);
  return TRUE;
}

/* Wait for the oldest job and hand its picture downstream (or drop the frame). */
static GstFlowReturn
finish_oldest (CutepiMjpegDec * self)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  g_mutex_lock (&self->lock);
  Job *j = g_queue_pop_head (&self->pending);
  if (!j) { g_mutex_unlock (&self->lock); return GST_FLOW_OK; }
  while (!j->done)
    g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);
  GstVideoCodecFrame *f = j->frame;
  if (!j->out) {
    job_free (j);
    GST_WARNING_OBJECT (self, "frame %u could not be decoded", f->system_frame_number);
    return gst_video_decoder_drop_frame (dec, f);
  }
  GstCaps *caps = gst_sample_get_caps (j->out);
  if (!self->out_caps || !gst_caps_is_equal (caps, self->out_caps)) {
    GstVideoInfo vi;
    if (!gst_video_info_from_caps (&vi, caps)) {
      job_free (j);
      return gst_video_decoder_drop_frame (dec, f);
    }
    GstVideoCodecState *st = gst_video_decoder_set_output_state (dec, GST_VIDEO_INFO_FORMAT (&vi),
        vi.width, vi.height, self->input_state);
    /* jpegdec's colorimetry (full-range JPEG) and interlacing carry over. */
    st->info.colorimetry = vi.colorimetry;
    st->info.interlace_mode = vi.interlace_mode;
    gst_video_codec_state_unref (st);
    if (!gst_video_decoder_negotiate (dec)) {
      job_free (j);
      gst_video_decoder_release_frame (dec, f);
      return GST_FLOW_NOT_NEGOTIATED;
    }
    gst_caps_replace (&self->out_caps, caps);
  }
  /* jpegdec's buffer itself (with its video meta for padded strides). */
  f->output_buffer = gst_buffer_ref (gst_sample_get_buffer (j->out));
  job_free (j);
  return gst_video_decoder_finish_frame (dec, f);
}

static GstFlowReturn
finish_all (CutepiMjpegDec * self)
{
  GstFlowReturn ret = GST_FLOW_OK;
  while (self->pending.length) {
    GstFlowReturn r = finish_oldest (self);
    if (r != GST_FLOW_OK && ret == GST_FLOW_OK) ret = r;
  }
  return ret;
}

static GstFlowReturn
m_handle_frame (GstVideoDecoder * dec, GstVideoCodecFrame * frame)
{
  CutepiMjpegDec *self = (CutepiMjpegDec *) dec;
  Worker *w = &self->workers[self->seq++ % self->n_workers];
  GstFlowReturn ret = GST_FLOW_OK;
  /* One job per worker: finish the oldest until this one is free. */
  for (;;) {
    g_mutex_lock (&self->lock);
    gboolean busy = w->job != NULL || self->pending.length >= (guint) self->n_workers;
    g_mutex_unlock (&self->lock);
    if (!busy) break;
    ret = finish_oldest (self);
    if (ret != GST_FLOW_OK) {
      gst_video_decoder_release_frame (dec, frame);
      return ret;
    }
  }
  Job *j = g_new0 (Job, 1);
  j->frame = frame;
  j->in = gst_buffer_ref (frame->input_buffer);
  g_mutex_lock (&self->lock);
  g_queue_push_tail (&self->pending, j);
  w->job = j;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  return GST_FLOW_OK;
}

static GstFlowReturn
m_finish (GstVideoDecoder * dec)
{
  return finish_all ((CutepiMjpegDec *) dec);
}

static gboolean
m_flush (GstVideoDecoder * dec)
{
  CutepiMjpegDec *self = (CutepiMjpegDec *) dec;
  wait_all (self);
  Job *j;
  while ((j = g_queue_pop_head (&self->pending))) {
    gst_video_decoder_release_frame (dec, j->frame);
    job_free (j);
  }
  return TRUE;
}

static void
m_set_property (GObject * o, guint id, const GValue * v, GParamSpec * ps)
{
  if (id == PROP_N_WORKERS) ((CutepiMjpegDec *) o)->n_workers = g_value_get_int (v);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
m_get_property (GObject * o, guint id, GValue * v, GParamSpec * ps)
{
  if (id == PROP_N_WORKERS) g_value_set_int (v, ((CutepiMjpegDec *) o)->n_workers);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
m_finalize (GObject * o)
{
  CutepiMjpegDec *self = (CutepiMjpegDec *) o;
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  G_OBJECT_CLASS (cutepi_mjpeg_dec_parent_class)->finalize (o);
}

static void
cutepi_mjpeg_dec_class_init (CutepiMjpegDecClass * k)
{
  GObjectClass *ok = G_OBJECT_CLASS (k);
  GstElementClass *ek = GST_ELEMENT_CLASS (k);
  GstVideoDecoderClass *dk = GST_VIDEO_DECODER_CLASS (k);
  ok->set_property = m_set_property;
  ok->get_property = m_get_property;
  ok->finalize = m_finalize;
  g_object_class_install_property (ok, PROP_N_WORKERS, g_param_spec_int ("n-workers", "Workers",
          "jpegdec instances decoding frames in parallel", 1, MAX_WORKERS, 3,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  gst_element_class_add_static_pad_template (ek, &sink_tmpl);
  gst_element_class_add_static_pad_template (ek, &src_tmpl);
  gst_element_class_set_static_metadata (ek, "CuTePi Motion JPEG decoder", "Codec/Decoder/Video",
      "Motion JPEG on several cores: jpegdec instances in parallel, frames in order", "CuTePi");
  dk->start = m_start;
  dk->stop = m_stop;
  dk->set_format = m_set_format;
  dk->handle_frame = m_handle_frame;
  dk->finish = m_finish;
  dk->drain = m_finish;
  dk->flush = m_flush;
}

static void
cutepi_mjpeg_dec_init (CutepiMjpegDec * self)
{
  self->n_workers = 3;
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  g_queue_init (&self->pending);
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
}

int
cutepi_mjpegdec_register (GstPlugin * plugin, int rank)
{
  GST_DEBUG_CATEGORY_INIT (mjpegdec_debug, "cutepimjpegdec", 0, "CuTePi Motion JPEG decoder");
#ifndef STANDALONE_PLUGIN
  /* In-process registration: only with jpegdec there. (A standalone plugin
   * cannot ask: the plugin scanner runs this before the registry is complete,
   * and the element's internal pipelines report a missing jpegdec anyway.) */
  GstElementFactory *f = gst_element_factory_find ("jpegdec");
  if (!f) return 0;
  gst_object_unref (f);
#endif
  return gst_element_register (plugin, "cutepimjpegdec", rank, cutepi_mjpeg_dec_get_type ());
}

#ifdef STANDALONE_PLUGIN
/* Standalone GStreamer plugin build (see this folder's Makefile). */
#ifndef PACKAGE
#define PACKAGE "cutepimjpegdec"
#endif
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_mjpegdec_register (p, GST_RANK_PRIMARY + 1);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepimjpegdec, "Motion JPEG decoded on several cores, frames in order", plugin_init, "1.0", "LGPL",
    "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
