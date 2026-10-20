/* cutepicfhddec: CineForm decoding with the GoPro CineForm SDK (third_party/cineform-sdk, ported to AArch64).
 *
 * FFmpeg's decoder (avdec_cfhd) does 19-23 fps for 1080p CineForm on a Pi 4; the SDK does 48 on one core. Its own
 * multithreading is not usable here: output differs by +-1 from run to run (on x86 too), and a sample it rejects
 * leaves its thread queue broken (abort or crash a few samples later). CineForm is intra-only, so this element
 * runs N single-threaded decoders on alternate frames instead (frame parallelism): 82 fps with 2 decoders, 92 with
 * 3, 98 with 4 (h264-pi4 results/38). Output is deterministic for a given decoder count; the SDK's 10-to-8-bit dither
 * depends on each decoder's own frame history, so 1 and 3 decoders differ by +-1 in ~1% of samples, at identical
 * quality (62.41 dB against the source either way). Its rand()-based dither is made per-frame in the library
 * (third_party/cineform-sdk/Codec/cutepi_rand.c).
 *
 * The library is loaded at run time (dlopen libcutepi-cfhd.so, built by third_party/cineform-sdk/Makefile), so
 * CuTePi builds without it and the element registers only where it is installed. Output: YUY2 for YUV and RGB
 * CineForm (feeds the ISP and the display planes directly), BGRA for RGBA CineForm (keeps the alpha). Each
 * worker decodes straight into the output buffer. A sample the SDK rejects is dropped with a warning. */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"

GST_DEBUG_CATEGORY_STATIC (cfhd_debug);
#define GST_CAT_DEFAULT cfhd_debug

#define MAX_WORKERS 4
#define SAMPLE_PAD 65536        /* the SDK's entropy decoder reads past the end of a sample */

typedef struct {
  CFHD_Error (*open) (CFHD_DecoderRef *, CFHD_ALLOCATOR *);
  CFHD_Error (*prepare) (CFHD_DecoderRef, int, int, CFHD_PixelFormat, CFHD_DecodedResolution, CFHD_DecodingFlags,
      void *, size_t, int *, int *, CFHD_PixelFormat *);
  CFHD_Error (*decode) (CFHD_DecoderRef, void *, size_t, void *, int);
  CFHD_Error (*close) (CFHD_DecoderRef);
  CFHD_Error (*sample_info) (CFHD_DecoderRef, void *, size_t, CFHD_SampleInfoTag, void *, size_t);
  CFHD_Error (*pitch) (int, CFHD_PixelFormat, int *);
  CFHD_Error (*md_open) (CFHD_MetadataRef *);
  CFHD_Error (*md_init) (CFHD_MetadataRef, CFHD_MetadataTrack, void *, size_t);
  CFHD_Error (*md_set) (CFHD_DecoderRef, CFHD_MetadataRef, unsigned int, CFHD_MetadataType, void *, size_t);
  CFHD_Error (*md_close) (CFHD_MetadataRef);
} CfhdLib;

/* The SDK keeps global state that concurrent decoder instances share: two decoders on alternate frames gave
 * output differing by +-1 from a single decoder (1% of samples). Each worker therefore loads its own copy of the
 * library in a separate link namespace (dlmopen LM_ID_NEWLM), so no globals are shared. */
static CfhdLib libs[MAX_WORKERS];
static int nlibs;


typedef struct _Worker Worker;
typedef struct {
  GstVideoCodecFrame *frame;
  GstVideoFrame vf;             /* the mapped output buffer */
  uint8_t *sample;              /* padded copy of the input */
  size_t size;
  Worker *w;
  gboolean done, ok;
} Job;

typedef struct _CutepiCfhdDec CutepiCfhdDec;
struct _Worker {
  CutepiCfhdDec *self;
  CfhdLib *lib;
  CFHD_DecoderRef dec;
  CFHD_MetadataRef md;
  GThread *thread;
  Job *job;                     /* the job handed to this worker, NULL when idle */
  gboolean quit;
};

struct _CutepiCfhdDec {
  GstVideoDecoder parent;
  gint n_workers;
  Worker workers[MAX_WORKERS];
  guint64 seq;                  /* frames handed out */
  GQueue pending;               /* Jobs in decode order */
  GMutex lock;
  GCond cond;
  gboolean prepared;
  CFHD_PixelFormat pixfmt;
  GstVideoCodecState *input_state;
  guint64 rejected;
  /* Fallback for streams the SDK rejects (CineForm written by FFmpeg's encoder: two thirds of a 4:2:2 file's
   * samples): FFmpeg's decoder in a small internal pipeline, used for the rest of the stream from the first
   * rejected sample on. */
  gboolean fallback;
  GstElement *fb, *fb_src, *fb_sink;
};
typedef struct { GstVideoDecoderClass parent; } CutepiCfhdDecClass;
GType cutepi_cfhd_dec_get_type (void);
G_DEFINE_TYPE (CutepiCfhdDec, cutepi_cfhd_dec, GST_TYPE_VIDEO_DECODER);

enum { PROP_0, PROP_N_WORKERS };

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-cineform"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string){ YUY2, BGRA }"));

static gpointer
worker_main (gpointer data)
{
  Worker *w = data;
  CutepiCfhdDec *self = w->self;
  g_mutex_lock (&self->lock);
  for (;;) {
    while (!w->quit && !w->job)
      g_cond_wait (&self->cond, &self->lock);
    if (w->quit)
      break;
    Job *j = w->job;
    g_mutex_unlock (&self->lock);
    int pitch = GST_VIDEO_FRAME_PLANE_STRIDE (&j->vf, 0);
    uint8_t *out = GST_VIDEO_FRAME_PLANE_DATA (&j->vf, 0);
    if (self->pixfmt == CFHD_PIXEL_FORMAT_BGRA) {
      /* The SDK writes RGB formats bottom-up: hand it the last row and a negative pitch. */
      out += (size_t) pitch * (GST_VIDEO_FRAME_HEIGHT (&j->vf) - 1);
      pitch = -pitch;
    }
    CFHD_Error e = w->lib->decode (w->dec, j->sample, j->size, out, pitch);
    g_mutex_lock (&self->lock);
    j->ok = e == CFHD_ERROR_OKAY;
    j->done = TRUE;
    w->job = NULL;
    g_cond_broadcast (&self->cond);
  }
  g_mutex_unlock (&self->lock);
  return NULL;
}

static gboolean
c_start (GstVideoDecoder * dec)
{
  CutepiCfhdDec *self = (CutepiCfhdDec *) dec;
  if (self->n_workers > nlibs)
    self->n_workers = nlibs;
  self->prepared = FALSE;
  self->fallback = FALSE;
  self->seq = 0;
  self->rejected = 0;
  for (int i = 0; i < self->n_workers; i++) {
    Worker *w = &self->workers[i];
    memset (w, 0, sizeof *w);
    w->self = self;
    w->lib = &libs[i];
    if (w->lib->open (&w->dec, NULL) || w->lib->md_open (&w->md)) {
      GST_ELEMENT_ERROR (self, LIBRARY, INIT, ("CineForm decoder could not be opened"), (NULL));
      return FALSE;
    }
    w->thread = g_thread_new ("cfhd-worker", worker_main, w);
  }
  return TRUE;
}

static void job_free (Job * j);

/* Decodes one sample with FFmpeg's CineForm decoder into vf. */
static gboolean
fb_decode (CutepiCfhdDec * self, const uint8_t * sample, size_t size, GstVideoFrame * vf)
{
  if (!self->fb) {
    gchar *d = g_strdup_printf ("appsrc name=s format=time ! avdec_cfhd ! videoconvert ! video/x-raw,format=%s ! "
        "appsink name=k sync=false max-buffers=2", self->pixfmt == CFHD_PIXEL_FORMAT_BGRA ? "BGRA" : "YUY2");
    GError *e = NULL;
    self->fb = gst_parse_launch (d, &e);
    g_free (d);
    if (!self->fb) {
      GST_ERROR_OBJECT (self, "no FFmpeg CineForm fallback: %s", e ? e->message : "?");
      g_clear_error (&e);
      return FALSE;
    }
    self->fb_src = gst_bin_get_by_name (GST_BIN (self->fb), "s");
    self->fb_sink = gst_bin_get_by_name (GST_BIN (self->fb), "k");
    g_object_set (self->fb_src, "caps", self->input_state->caps, NULL);
    gst_element_set_state (self->fb, GST_STATE_PLAYING);
  }
  gst_app_src_push_buffer (GST_APP_SRC (self->fb_src), gst_buffer_new_memdup (sample, size));
  GstSample *smp = gst_app_sink_try_pull_sample (GST_APP_SINK (self->fb_sink), 5 * GST_SECOND);
  if (!smp)
    return FALSE;
  GstVideoInfo vi;
  GstVideoFrame in;
  gboolean ok = gst_video_info_from_caps (&vi, gst_sample_get_caps (smp)) &&
      gst_video_frame_map (&in, &vi, gst_sample_get_buffer (smp), GST_MAP_READ);
  if (ok) {
    gst_video_frame_copy (vf, &in);
    gst_video_frame_unmap (&in);
  }
  gst_sample_unref (smp);
  return ok;
}

static void
fb_stop (CutepiCfhdDec * self)
{
  if (!self->fb)
    return;
  gst_element_set_state (self->fb, GST_STATE_NULL);
  gst_clear_object (&self->fb_src);
  gst_clear_object (&self->fb_sink);
  gst_clear_object (&self->fb);
}

/* Wait for the oldest job and hand its frame downstream (or drop it). Called with the stream lock. */
static GstFlowReturn
finish_oldest (CutepiCfhdDec * self)
{
  g_mutex_lock (&self->lock);
  Job *j = g_queue_pop_head (&self->pending);
  if (!j) {
    g_mutex_unlock (&self->lock);
    return GST_FLOW_OK;
  }
  while (!j->done)
    g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);
  gboolean ok = j->ok;
  if (!ok) {
    self->rejected++;
    if (!self->fallback)
      GST_WARNING_OBJECT (self, "CineForm sample %u rejected by the SDK: FFmpeg's decoder takes the rest of the stream",
          j->frame->system_frame_number);
    self->fallback = TRUE;
    ok = fb_decode (self, j->sample, j->size, &j->vf);
  }
  gst_video_frame_unmap (&j->vf);
  GstVideoCodecFrame *f = j->frame;
  job_free (j);
  if (!ok)
    return gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), f);
  return gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), f);
}

static void
job_free (Job * j)
{
  free (j->sample);
  g_free (j);
}

static GstFlowReturn
finish_all (CutepiCfhdDec * self)
{
  GstFlowReturn ret = GST_FLOW_OK;
  while (self->pending.length) {
    GstFlowReturn r = finish_oldest (self);
    if (r != GST_FLOW_OK && ret == GST_FLOW_OK)
      ret = r;
  }
  return ret;
}

static gboolean
c_stop (GstVideoDecoder * dec)
{
  CutepiCfhdDec *self = (CutepiCfhdDec *) dec;
  finish_all (self);
  g_mutex_lock (&self->lock);
  for (int i = 0; i < self->n_workers; i++)
    self->workers[i].quit = TRUE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  for (int i = 0; i < self->n_workers; i++) {
    Worker *w = &self->workers[i];
    if (w->thread)
      g_thread_join (w->thread);
    if (w->md)
      w->lib->md_close (w->md);
    if (w->dec)
      w->lib->close (w->dec);
    memset (w, 0, sizeof *w);
  }
  fb_stop (self);
  self->fallback = FALSE;
  g_clear_pointer (&self->input_state, gst_video_codec_state_unref);
  return TRUE;
}

static gboolean
c_set_format (GstVideoDecoder * dec, GstVideoCodecState * state)
{
  CutepiCfhdDec *self = (CutepiCfhdDec *) dec;
  g_clear_pointer (&self->input_state, gst_video_codec_state_unref);
  self->input_state = gst_video_codec_state_ref (state);
  self->prepared = FALSE;
  return TRUE;
}

/* First sample: choose the output (BGRA when the stream carries alpha, else YUY2), prepare every decoder
 * single-threaded, and negotiate. */
static gboolean
prepare (CutepiCfhdDec * self, uint8_t * sample, size_t size)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  CFHD_EncodedFormat enc = CFHD_ENCODED_FORMAT_YUV_422;
  self->workers[0].lib->sample_info (self->workers[0].dec, sample, size, CFHD_SAMPLE_ENCODED_FORMAT, &enc, sizeof enc);
  self->pixfmt = enc == CFHD_ENCODED_FORMAT_RGBA_4444 ? CFHD_PIXEL_FORMAT_BGRA : CFHD_PIXEL_FORMAT_YUY2;
  int w = 0, h = 0;
  for (int i = 0; i < self->n_workers; i++) {
    Worker *wk = &self->workers[i];
    CFHD_PixelFormat actual;
    if (wk->lib->prepare (wk->dec, 0, 0, self->pixfmt, CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
            sample, size, &w, &h, &actual) || actual != self->pixfmt)
      return FALSE;
    unsigned int one = 1;
    wk->lib->md_init (wk->md, METADATATYPE_ORIGINAL, sample, size);
    wk->lib->md_set (wk->dec, wk->md, TAG_CPU_MAX, METADATATYPE_UINT32, &one, sizeof one);
  }
  GstVideoCodecState *st = gst_video_decoder_set_output_state (dec,
      self->pixfmt == CFHD_PIXEL_FORMAT_BGRA ? GST_VIDEO_FORMAT_BGRA : GST_VIDEO_FORMAT_YUY2, w, h, self->input_state);
  gst_video_codec_state_unref (st);
  GST_INFO_OBJECT (self, "CineForm %dx%d (encoded format %d) to %s with %d decoders", w, h, enc,
      self->pixfmt == CFHD_PIXEL_FORMAT_BGRA ? "BGRA" : "YUY2", self->n_workers);
  return gst_video_decoder_negotiate (dec);
}

static GstFlowReturn
c_handle_frame (GstVideoDecoder * dec, GstVideoCodecFrame * frame)
{
  CutepiCfhdDec *self = (CutepiCfhdDec *) dec;
  GstMapInfo mi;
  if (!gst_buffer_map (frame->input_buffer, &mi, GST_MAP_READ))
    return gst_video_decoder_drop_frame (dec, frame);
  Job *j = g_new0 (Job, 1);
  j->size = mi.size;
  j->sample = aligned_alloc (64, (mi.size + SAMPLE_PAD + 63) & ~(size_t) 63);
  memcpy (j->sample, mi.data, mi.size);
  memset (j->sample + mi.size, 0, SAMPLE_PAD);
  gst_buffer_unmap (frame->input_buffer, &mi);

  if (!self->prepared) {
    if (!prepare (self, j->sample, j->size)) {
      job_free (j);
      GST_ELEMENT_ERROR (self, STREAM, DECODE, ("not a CineForm sample this decoder can prepare for"), (NULL));
      gst_video_decoder_release_frame (dec, frame);
      return GST_FLOW_NOT_NEGOTIATED;
    }
    self->prepared = TRUE;
  }
  if (self->fallback) {
    GstFlowReturn r = finish_all (self);
    GstFlowReturn a = gst_video_decoder_allocate_output_frame (dec, frame);
    if (r != GST_FLOW_OK || a != GST_FLOW_OK) {
      job_free (j);
      gst_video_decoder_release_frame (dec, frame);
      return r != GST_FLOW_OK ? r : a;
    }
    GstVideoCodecState *st = gst_video_decoder_get_output_state (dec);
    gboolean ok = gst_video_frame_map (&j->vf, &st->info, frame->output_buffer, GST_MAP_WRITE);
    gst_video_codec_state_unref (st);
    if (ok) {
      ok = fb_decode (self, j->sample, j->size, &j->vf);
      gst_video_frame_unmap (&j->vf);
    }
    job_free (j);
    return ok ? gst_video_decoder_finish_frame (dec, frame) : gst_video_decoder_drop_frame (dec, frame);
  }
  /* At most one job per decoder in flight: finish the oldest before taking a busy one. */
  Worker *w = &self->workers[self->seq++ % self->n_workers];
  GstFlowReturn ret = GST_FLOW_OK;
  for (;;) {
    g_mutex_lock (&self->lock);
    gboolean busy = w->job != NULL || self->pending.length >= (guint) self->n_workers;
    g_mutex_unlock (&self->lock);
    if (!busy)
      break;
    ret = finish_oldest (self);
    if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING) {
      job_free (j);
      gst_video_decoder_release_frame (dec, frame);
      return ret;
    }
  }
  GstFlowReturn a = gst_video_decoder_allocate_output_frame (dec, frame);
  if (a != GST_FLOW_OK) {
    job_free (j);
    gst_video_decoder_release_frame (dec, frame);
    return a;
  }
  GstVideoCodecState *st = gst_video_decoder_get_output_state (dec);
  gboolean mapped = gst_video_frame_map (&j->vf, &st->info, frame->output_buffer, GST_MAP_WRITE);
  gst_video_codec_state_unref (st);
  if (!mapped) {
    job_free (j);
    return gst_video_decoder_drop_frame (dec, frame);
  }
  j->frame = frame;
  j->w = w;
  g_mutex_lock (&self->lock);
  g_queue_push_tail (&self->pending, j);
  w->job = j;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  return ret == GST_FLOW_FLUSHING ? GST_FLOW_OK : ret;
}

static GstFlowReturn
c_finish (GstVideoDecoder * dec)
{
  return finish_all ((CutepiCfhdDec *) dec);
}

static gboolean
c_flush (GstVideoDecoder * dec)
{
  CutepiCfhdDec *self = (CutepiCfhdDec *) dec;
  /* Let the decoders finish what they hold, then throw the frames away. */
  g_mutex_lock (&self->lock);
  for (GList * l = self->pending.head; l; l = l->next)
    while (!((Job *) l->data)->done)
      g_cond_wait (&self->cond, &self->lock);
  Job *j;
  while ((j = g_queue_pop_head (&self->pending))) {
    gst_video_frame_unmap (&j->vf);
    gst_video_decoder_release_frame (dec, j->frame);
    job_free (j);
  }
  g_mutex_unlock (&self->lock);
  return TRUE;
}

static void
c_set_property (GObject * o, guint id, const GValue * v, GParamSpec * ps)
{
  if (id == PROP_N_WORKERS)
    ((CutepiCfhdDec *) o)->n_workers = g_value_get_int (v);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
c_get_property (GObject * o, guint id, GValue * v, GParamSpec * ps)
{
  if (id == PROP_N_WORKERS)
    g_value_set_int (v, ((CutepiCfhdDec *) o)->n_workers);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
c_finalize (GObject * o)
{
  CutepiCfhdDec *self = (CutepiCfhdDec *) o;
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  G_OBJECT_CLASS (cutepi_cfhd_dec_parent_class)->finalize (o);
}

static void
cutepi_cfhd_dec_class_init (CutepiCfhdDecClass * k)
{
  GObjectClass *ok = G_OBJECT_CLASS (k);
  GstElementClass *ek = GST_ELEMENT_CLASS (k);
  GstVideoDecoderClass *dk = GST_VIDEO_DECODER_CLASS (k);
  ok->set_property = c_set_property;
  ok->get_property = c_get_property;
  ok->finalize = c_finalize;
  g_object_class_install_property (ok, PROP_N_WORKERS, g_param_spec_int ("n-workers", "Decoders",
          "single-threaded decoders working on alternate frames (at most the library copies loaded)", 1, MAX_WORKERS, 3,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  gst_element_class_add_static_pad_template (ek, &sink_tmpl);
  gst_element_class_add_static_pad_template (ek, &src_tmpl);
  gst_element_class_set_static_metadata (ek, "CineForm decoder (CineForm SDK)", "Codec/Decoder/Video",
      "Decodes CineForm with the GoPro CineForm SDK, frame-parallel", "CuTePi");
  dk->start = c_start;
  dk->stop = c_stop;
  dk->set_format = c_set_format;
  dk->handle_frame = c_handle_frame;
  dk->finish = c_finish;
  dk->drain = c_finish;
  dk->flush = c_flush;
  GST_DEBUG_CATEGORY_INIT (cfhd_debug, "cutepicfhddec", 0, "CineForm SDK decoder");
}

static void
cutepi_cfhd_dec_init (CutepiCfhdDec * self)
{
  self->n_workers = 3;
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  g_queue_init (&self->pending);
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
}

/* Loads one copy of the library in a new link namespace into l. */
static int
load_copy (const char *path, CfhdLib * l)
{
  void *h = dlmopen (LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    GST_WARNING ("CineForm library %s: %s", path, dlerror ());
    return 0;
  }
#define SYM(field, name) if (!(*(void **) &l->field = dlsym (h, name))) return 0
  SYM (open, "CFHD_OpenDecoder");
  SYM (prepare, "CFHD_PrepareToDecode");
  SYM (decode, "CFHD_DecodeSample");
  SYM (close, "CFHD_CloseDecoder");
  SYM (sample_info, "CFHD_GetSampleInfo");
  SYM (pitch, "CFHD_GetImagePitch");
  SYM (md_open, "CFHD_OpenMetadata");
  SYM (md_init, "CFHD_InitSampleMetadata");
  SYM (md_set, "CFHD_SetActiveMetadata");
  SYM (md_close, "CFHD_CloseMetadata");
#undef SYM
  return 1;
}

/* Registers cutepicfhddec when the CineForm library loads from path; 0 otherwise. */
int
cutepi_cfhd_register (GstPlugin * plugin, const char *path, int rank)
{
  /* One copy per possible worker (MAX_WORKERS); glibc allows 16 link namespaces in all. */
  if (!nlibs) {
    GST_DEBUG_CATEGORY_INIT (cfhd_debug, "cutepicfhddec", 0, "CineForm SDK decoder");
    while (nlibs < MAX_WORKERS && load_copy (path, &libs[nlibs]))
      nlibs++;
  }
  if (!nlibs)
    return 0;
  return gst_element_register (plugin, "cutepicfhddec", rank, cutepi_cfhd_dec_get_type ());
}

#ifdef CFHD_PLUGIN
#define PACKAGE "cutepicfhd"
static gboolean
plugin_init (GstPlugin * p)
{
  const char *path = g_getenv ("CUTEPI_CFHD_LIB");
  return cutepi_cfhd_register (p, path ? path : "/usr/local/lib/cutepi/libcutepi-cfhd.so", GST_RANK_PRIMARY + 1);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepicfhd, "CineForm decoding with the CineForm SDK",
    plugin_init, "1.0", "LGPL", "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
