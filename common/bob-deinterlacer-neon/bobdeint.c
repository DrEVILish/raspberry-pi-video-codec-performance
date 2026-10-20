/* cutepibob: interlaced video to one picture per field, on the CPU with NEON
 * (DESIGN §12.16).
 *
 * Each input frame (two fields woven) gives two output pictures, in the field
 * order: the field's own rows as they are, the rows between the rounded mean of
 * the field's rows above and below (line-interpolated "bob", as
 * `deinterlace method=linear fields=all`). One pass over the input: every input
 * row is read once (into a small cached ring; the hardware decoder's frames
 * sit in uncached memory, where GStreamer's deinterlace read each frame twice
 * and topped out at 60 pictures a second on a whole core) and both pictures
 * are written from it.
 *
 * 8-bit formats, planar or packed, alpha included: each plane is rows of
 * bytes (a packed row is all its pixels' bytes), and for 4:2:0 the chroma rows
 * alternate between the fields like the luma rows, so every plane is treated
 * the same way.
 *
 * With dmabuf=true (the GPU wall) the pictures are written into the element's
 * own CMA dma-heap buffers (cached; the allocator syncs the cache on map and
 * unmap) and leave as DMA_DRM DMABufs the GPU samples directly. Before, the
 * ISP copied every picture from system memory into a DMABuf: 60 copies a
 * second on the VideoCore beside the H.264 decoder, which held 1080i H.264 at
 * 55 pictures a second. I420 only in that mode. */
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

GST_DEBUG_CATEGORY_STATIC (bob_debug);
#define GST_CAT_DEFAULT bob_debug

typedef struct {
  GstElement parent;
  GstPad *sinkpad, *srcpad;
  gboolean tff, dmabuf;
  GstVideoInfo in, out;
  GstBufferPool *pool;
} CutepiBob;
typedef struct { GstElementClass parent; } CutepiBobClass;
GType cutepi_bob_get_type (void);
G_DEFINE_TYPE (CutepiBob, cutepi_bob, GST_TYPE_ELEMENT);

enum { PROP_0, PROP_TFF, PROP_DMABUF };

#define FORMATS "{ I420, YV12, Y42B, Y444, NV12, NV21, GRAY8, A420, A444, GBR, GBRA, AYUV, " \
  "RGBA, BGRA, ARGB, ABGR, RGBx, BGRx, xRGB, xBGR, RGB, BGR, YUY2, UYVY, YVYU }"
static GstStaticPadTemplate sink_t = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (FORMATS)));
static GstStaticPadTemplate src_t = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (FORMATS) ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_DMABUF, "DMA_DRM")));
/* What the sink takes in dmabuf mode. */
/* What the sink takes in dmabuf mode: I420 (YU12), the layout the ISP gave
 * the wall before; a converter ahead normalises the rest. */
static GstStaticCaps dmabuf_in = GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("I420"));

/* ---- a pool of CMA dma-heap buffers --------------------------------------- */
typedef struct {
  GstBufferPool parent;
  GstAllocator *alloc;
  GstVideoInfo info;            /* aligned layout */
  int heap;
} BobDmaPool;
typedef struct { GstBufferPoolClass parent; } BobDmaPoolClass;
GType bob_dma_pool_get_type (void);
G_DEFINE_TYPE (BobDmaPool, bob_dma_pool, GST_TYPE_BUFFER_POOL);

static GstFlowReturn
bob_dma_pool_alloc (GstBufferPool * bp, GstBuffer ** out, GstBufferPoolAcquireParams * params)
{
  BobDmaPool *p = (BobDmaPool *) bp;
  struct dma_heap_allocation_data a = {.len = GST_VIDEO_INFO_SIZE (&p->info),.fd_flags = O_RDWR | O_CLOEXEC };
  if (ioctl (p->heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0) {
    GST_ERROR_OBJECT (p, "dma-heap allocation of %" G_GSIZE_FORMAT " bytes failed", (gsize) a.len);
    return GST_FLOW_ERROR;
  }
  GstBuffer *b = gst_buffer_new ();
  gst_buffer_append_memory (b, gst_dmabuf_allocator_alloc (p->alloc, a.fd, a.len)); /* owns the fd */
  GstVideoMeta *m = gst_buffer_add_video_meta_full (b, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_INFO_FORMAT (&p->info),
      GST_VIDEO_INFO_WIDTH (&p->info), GST_VIDEO_INFO_HEIGHT (&p->info), GST_VIDEO_INFO_N_PLANES (&p->info),
      p->info.offset, p->info.stride);
  GST_META_FLAG_SET (m, GST_META_FLAG_POOLED);
  *out = b;
  return GST_FLOW_OK;
}

static void
bob_dma_pool_finalize (GObject * o)
{
  BobDmaPool *p = (BobDmaPool *) o;
  if (p->heap >= 0) close (p->heap);
  gst_clear_object (&p->alloc);
  G_OBJECT_CLASS (bob_dma_pool_parent_class)->finalize (o);
}

static void
bob_dma_pool_class_init (BobDmaPoolClass * k)
{
  G_OBJECT_CLASS (k)->finalize = bob_dma_pool_finalize;
  GST_BUFFER_POOL_CLASS (k)->alloc_buffer = bob_dma_pool_alloc;
}

static void
bob_dma_pool_init (BobDmaPool * p)
{
  p->heap = -1;
}

/* A pool of DMABufs laid out as info (strides aligned for the GPU), or NULL
 * without a CMA dma-heap. */
static GstBufferPool *
bob_dma_pool_new (const GstVideoInfo * info, GstCaps * caps)
{
  int heap = open ("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
  if (heap < 0) return NULL;
  BobDmaPool *p = g_object_new (bob_dma_pool_get_type (), NULL);
  gst_object_ref_sink (p);
  p->heap = heap;
  p->alloc = gst_dmabuf_allocator_new ();
  p->info = *info;
  GstStructure *cfg = gst_buffer_pool_get_config (GST_BUFFER_POOL (p));
  /* Bounded: the queue after the element, the appsink and the wall hold
   * the rest; a stalled consumer then blocks here instead of draining CMA. */
  gst_buffer_pool_config_set_params (cfg, caps, GST_VIDEO_INFO_SIZE (info), 6, 16);
  gst_buffer_pool_set_config (GST_BUFFER_POOL (p), cfg);
  return GST_BUFFER_POOL (p);
}

/* d = rounded mean of a and b, n bytes. */
static void
avg_row (guint8 * d, const guint8 * a, const guint8 * b, int n)
{
  int x = 0;
#ifdef __aarch64__
  for (; x + 16 <= n; x += 16)
    vst1q_u8 (d + x, vrhaddq_u8 (vld1q_u8 (a + x), vld1q_u8 (b + x)));
#endif
  for (; x < n; x++) d[x] = (guint8) ((a[x] + b[x] + 1) >> 1);
}

/* One plane: rows h, row bytes n. o0 shows field f0 (0 top, 1 bottom), o1 the other. */
static void
bob_plane (const guint8 * src, int ss, guint8 * o0, int s0, guint8 * o1, int s1, int n, int h, int f0, guint8 * ring)
{
  guint8 *r[3] = { ring, ring + n, ring + 2 * n };
  guint8 *out[2] = { o0, o1 };
  int stride[2] = { s0, s1 };
  /* out[k] keeps rows of parity field[k]. */
  int field[2] = { f0, 1 - f0 };
  memcpy (r[0], src, n);              /* row 0 */
  if (h > 1) memcpy (r[1], src + ss, n); /* row 1 */
  for (int y = 0; y < h; y++) {
    /* r[(y+2)%3] gets row y+1 ahead: rows y-1, y, y+1 are in the ring. */
    const guint8 *cur = r[y % 3];
    if (y + 1 < h && y >= 1) memcpy (r[(y + 1) % 3], src + (gsize) (y + 1) * ss, n);
    const guint8 *up = y > 0 ? r[(y - 1) % 3] : NULL;
    const guint8 *dn = y + 1 < h ? r[(y + 1) % 3] : NULL;
    for (int k = 0; k < 2; k++) {
      guint8 *d = out[k] + (gsize) y * stride[k];
      if ((y & 1) == field[k]) memcpy (d, cur, n);
      else if (up && dn) avg_row (d, up, dn, n);
      else memcpy (d, up ? up : dn, n);
    }
  }
}

static GstFlowReturn
bob_chain (GstPad * pad, GstObject * parent, GstBuffer * in)
{
  CutepiBob *b = (CutepiBob *) parent;
  GstBuffer *o[2] = { NULL, NULL };
  for (int k = 0; k < 2; k++) {
    GstFlowReturn r = gst_buffer_pool_acquire_buffer (b->pool, &o[k], NULL);
    if (r != GST_FLOW_OK) {
      if (o[0]) gst_buffer_unref (o[0]);
      gst_buffer_unref (in);
      return r;
    }
  }
  GstVideoFrame fi, f0, f1;
  gboolean ok = gst_video_frame_map (&fi, &b->in, in, GST_MAP_READ);
  if (ok && !gst_video_frame_map (&f0, &b->out, o[0], GST_MAP_WRITE)) { gst_video_frame_unmap (&fi); ok = FALSE; }
  if (ok && !gst_video_frame_map (&f1, &b->out, o[1], GST_MAP_WRITE)) {
    gst_video_frame_unmap (&fi); gst_video_frame_unmap (&f0); ok = FALSE;
  }
  if (!ok) {
    gst_buffer_unref (o[0]); gst_buffer_unref (o[1]); gst_buffer_unref (in);
    GST_ELEMENT_ERROR (b, STREAM, FAILED, ("could not map frames"), (NULL));
    return GST_FLOW_ERROR;
  }
  int f0f = b->tff ? 0 : 1;
  for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES (&fi); p++) {
    int n = GST_VIDEO_FRAME_COMP_WIDTH (&fi, p) * GST_VIDEO_FRAME_COMP_PSTRIDE (&fi, p);
    if (GST_VIDEO_INFO_FORMAT (&b->in) == GST_VIDEO_FORMAT_NV12 || GST_VIDEO_INFO_FORMAT (&b->in) == GST_VIDEO_FORMAT_NV21)
      n = p == 0 ? GST_VIDEO_FRAME_WIDTH (&fi) : GST_VIDEO_FRAME_COMP_WIDTH (&fi, 1) * 2;
    int h = GST_VIDEO_FRAME_COMP_HEIGHT (&fi, p);
    guint8 *ring = g_malloc (3 * (gsize) n);
    bob_plane (GST_VIDEO_FRAME_PLANE_DATA (&fi, p), GST_VIDEO_FRAME_PLANE_STRIDE (&fi, p),
        GST_VIDEO_FRAME_PLANE_DATA (&f0, p), GST_VIDEO_FRAME_PLANE_STRIDE (&f0, p),
        GST_VIDEO_FRAME_PLANE_DATA (&f1, p), GST_VIDEO_FRAME_PLANE_STRIDE (&f1, p), n, h, f0f, ring);
    g_free (ring);
  }
  gst_video_frame_unmap (&fi); gst_video_frame_unmap (&f0); gst_video_frame_unmap (&f1);
  GstClockTime pts = GST_BUFFER_PTS (in), dur = GST_BUFFER_DURATION (in);
  if (!GST_CLOCK_TIME_IS_VALID (dur) && GST_VIDEO_INFO_FPS_N (&b->in) > 0)
    dur = gst_util_uint64_scale (GST_SECOND, GST_VIDEO_INFO_FPS_D (&b->in), GST_VIDEO_INFO_FPS_N (&b->in));
  for (int k = 0; k < 2; k++) {
    GST_BUFFER_PTS (o[k]) = pts;
    GST_BUFFER_DURATION (o[k]) = GST_CLOCK_TIME_IS_VALID (dur) ? dur / 2 : GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS (o[k]) = GST_CLOCK_TIME_NONE;
  }
  if (GST_CLOCK_TIME_IS_VALID (pts) && GST_CLOCK_TIME_IS_VALID (dur)) GST_BUFFER_PTS (o[1]) = pts + dur / 2;
  if (GST_BUFFER_FLAG_IS_SET (in, GST_BUFFER_FLAG_DISCONT)) GST_BUFFER_FLAG_SET (o[0], GST_BUFFER_FLAG_DISCONT);
  gst_buffer_unref (in);
  GstFlowReturn r = gst_pad_push (b->srcpad, o[0]);
  if (r != GST_FLOW_OK) { gst_buffer_unref (o[1]); return r; }
  return gst_pad_push (b->srcpad, o[1]);
}

static gboolean
bob_setcaps (CutepiBob * b, GstCaps * caps)
{
  if (!gst_video_info_from_caps (&b->in, caps)) return FALSE;
  b->out = b->in;
  GST_VIDEO_INFO_INTERLACE_MODE (&b->out) = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
  if (GST_VIDEO_INFO_FPS_N (&b->out) > 0) GST_VIDEO_INFO_FPS_N (&b->out) *= 2;
  if (b->pool) { gst_buffer_pool_set_active (b->pool, FALSE); gst_clear_object (&b->pool); }
  GstCaps *oc;
  if (b->dmabuf) {
    GstVideoFormat f = GST_VIDEO_INFO_FORMAT (&b->in);
    guint32 fourcc = gst_video_dma_drm_fourcc_from_format (f);
    GstStaticCaps *ok_in = &dmabuf_in;
    GstCaps *okc = gst_static_caps_get (ok_in);
    gboolean fits = gst_caps_can_intersect (caps, okc);
    gst_caps_unref (okc);
    if (!fits || fourcc == 0) {
      GST_ELEMENT_ERROR (b, CORE, NEGOTIATION, ("%s cannot leave as a DMABuf", gst_video_format_to_string (f)), (NULL));
      return FALSE;
    }
    /* Luma rows on 64 bytes, chroma on 32, as the ISP's DMABufs. */
    GstVideoAlignment al;
    gst_video_alignment_reset (&al);
    for (guint i = 0; i < GST_VIDEO_MAX_PLANES; i++) al.stride_align[i] = i == 0 ? 63 : 31;
    if (!gst_video_info_align (&b->out, &al)) return FALSE;
    oc = gst_video_info_to_caps (&b->out);
    gchar *drm = gst_video_dma_drm_fourcc_to_string (fourcc, 0 /* linear */);
    gst_caps_set_simple (oc, "format", G_TYPE_STRING, "DMA_DRM", "drm-format", G_TYPE_STRING, drm,
        "interlace-mode", G_TYPE_STRING, "progressive", NULL);
    g_free (drm);
    gst_caps_set_features_simple (oc, gst_caps_features_new_single_static_str (GST_CAPS_FEATURE_MEMORY_DMABUF));
    b->pool = bob_dma_pool_new (&b->out, oc);
    if (!b->pool) {
      gst_caps_unref (oc);
      GST_ELEMENT_ERROR (b, RESOURCE, OPEN_READ_WRITE, ("no CMA dma-heap"), ("%s", g_strerror (errno)));
      return FALSE;
    }
  } else {
    oc = gst_video_info_to_caps (&b->out);
    gst_caps_set_simple (oc, "interlace-mode", G_TYPE_STRING, "progressive", NULL);
    b->pool = gst_video_buffer_pool_new ();
    GstStructure *cfg = gst_buffer_pool_get_config (b->pool);
    gst_buffer_pool_config_set_params (cfg, oc, b->out.size, 4, 0);
    gst_buffer_pool_config_add_option (cfg, GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_buffer_pool_set_config (b->pool, cfg);
  }
  gboolean ok = gst_pad_push_event (b->srcpad, gst_event_new_caps (oc));
  gst_buffer_pool_set_active (b->pool, TRUE);
  gst_caps_unref (oc);
  return ok;
}

static gboolean
bob_sink_event (GstPad * pad, GstObject * parent, GstEvent * ev)
{
  CutepiBob *b = (CutepiBob *) parent;
  if (GST_EVENT_TYPE (ev) == GST_EVENT_CAPS) {
    GstCaps *c;
    gst_event_parse_caps (ev, &c);
    gboolean ok = bob_setcaps (b, c);
    gst_event_unref (ev);
    return ok;
  }
  return gst_pad_event_default (pad, parent, ev);
}

/* Upstream may offer any frame rate and interlacing: downstream's caps with
 * those fields left open. */
static gboolean
bob_sink_query (GstPad * pad, GstObject * parent, GstQuery * q)
{
  CutepiBob *b = (CutepiBob *) parent;
  if (GST_QUERY_TYPE (q) == GST_QUERY_CAPS) {
    GstCaps *filter, *r;
    gst_query_parse_caps (q, &filter);
    if (b->dmabuf) {
      /* Downstream takes DMABufs (the wall imports any size). */
      r = gst_static_caps_get (&dmabuf_in);
    } else {
      GstCaps *peer = gst_pad_peer_query_caps (b->srcpad, NULL), *tmpl = gst_pad_get_pad_template_caps (pad);
      r = gst_caps_intersect (peer, tmpl);
      gst_caps_unref (peer); gst_caps_unref (tmpl);
    }
    r = gst_caps_make_writable (r);
    for (guint i = 0; i < gst_caps_get_size (r); i++)
      gst_structure_remove_fields (gst_caps_get_structure (r, i), "framerate", "interlace-mode", "field-order", NULL);
    if (filter) {
      GstCaps *t = gst_caps_intersect_full (filter, r, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (r);
      r = t;
    }
    gst_query_set_caps_result (q, r);
    gst_caps_unref (r);
    return TRUE;
  }
  if (GST_QUERY_TYPE (q) == GST_QUERY_ACCEPT_CAPS) {
    GstCaps *c;
    gst_query_parse_accept_caps (q, &c);
    GstCaps *tmpl = b->dmabuf ? gst_static_caps_get (&dmabuf_in) : gst_pad_get_pad_template_caps (pad);
    gst_query_set_accept_caps_result (q, gst_caps_can_intersect (c, tmpl));
    gst_caps_unref (tmpl);
    return TRUE;
  }
  if (GST_QUERY_TYPE (q) == GST_QUERY_ALLOCATION) {
    /* Upstream's own buffers, read once each (no pool proposed; their video
     * meta carries padded strides): the hardware decoder then hands over its
     * frames without copying them first. */
    gst_query_add_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL);
    return TRUE;
  }
  return gst_pad_query_default (pad, parent, q);
}

static GstStateChangeReturn
bob_change_state (GstElement * e, GstStateChange t)
{
  CutepiBob *b = (CutepiBob *) e;
  GstStateChangeReturn r = GST_ELEMENT_CLASS (cutepi_bob_parent_class)->change_state (e, t);
  if (t == GST_STATE_CHANGE_PAUSED_TO_READY && b->pool) {
    gst_buffer_pool_set_active (b->pool, FALSE);
    gst_clear_object (&b->pool);
  }
  return r;
}

static void
bob_set_property (GObject * o, guint id, const GValue * v, GParamSpec * ps)
{
  if (id == PROP_TFF) ((CutepiBob *) o)->tff = g_value_get_boolean (v);
  else if (id == PROP_DMABUF) ((CutepiBob *) o)->dmabuf = g_value_get_boolean (v);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
bob_get_property (GObject * o, guint id, GValue * v, GParamSpec * ps)
{
  if (id == PROP_TFF) g_value_set_boolean (v, ((CutepiBob *) o)->tff);
  else if (id == PROP_DMABUF) g_value_set_boolean (v, ((CutepiBob *) o)->dmabuf);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
cutepi_bob_class_init (CutepiBobClass * k)
{
  GObjectClass *ok = G_OBJECT_CLASS (k);
  ok->set_property = bob_set_property;
  ok->get_property = bob_get_property;
  g_object_class_install_property (ok, PROP_TFF, g_param_spec_boolean ("tff", "Top field first",
          "The top field is shown first", TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (ok, PROP_DMABUF, g_param_spec_boolean ("dmabuf", "DMABuf output",
          "Write the pictures into CMA dma-heap buffers and pass them on as DMA_DRM DMABufs (I420 only)", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  GST_ELEMENT_CLASS (k)->change_state = bob_change_state;
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (k), &sink_t);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (k), &src_t);
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (k), "CuTePi bob deinterlacer", "Filter/Effect/Video/Deinterlace",
      "One picture per field, the rows between line-interpolated (NEON, one pass)", "CuTePi");
}

static void
cutepi_bob_init (CutepiBob * b)
{
  b->tff = TRUE;
  b->sinkpad = gst_pad_new_from_static_template (&sink_t, "sink");
  gst_pad_set_chain_function (b->sinkpad, bob_chain);
  gst_pad_set_event_function (b->sinkpad, bob_sink_event);
  gst_pad_set_query_function (b->sinkpad, bob_sink_query);
  gst_element_add_pad (GST_ELEMENT (b), b->sinkpad);
  b->srcpad = gst_pad_new_from_static_template (&src_t, "src");
  gst_pad_use_fixed_caps (b->srcpad);
  gst_element_add_pad (GST_ELEMENT (b), b->srcpad);
}

int
cutepi_bob_register (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (bob_debug, "cutepibob", 0, "CuTePi bob deinterlacer");
  return gst_element_register (plugin, "cutepibob", GST_RANK_NONE, cutepi_bob_get_type ());
}

#ifdef STANDALONE_PLUGIN
/* Standalone GStreamer plugin build (see this folder's Makefile). */
#ifndef PACKAGE
#define PACKAGE "cutepibob"
#endif
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_bob_register (p);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepibob, "NEON bob deinterlacer, one picture per field", plugin_init, "1.0", "LGPL",
    "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
