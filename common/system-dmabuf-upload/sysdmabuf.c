/* cutepisysdmabuf: software-decoded frames (system memory) into dmabufs the GPU wall imports, by one CPU copy.
 *
 * On the GPU wall a software decoder's frames went through the ISP (v4l2convert): GStreamer copies each frame into
 * the ISP's input buffers (write-combined), the VideoCore converts it, and YU12 dmabufs come out for glupload. Here
 * each frame is copied, plane by plane, into a buffer from the system dma-heap (cached memory, not CMA; V3D has an
 * MMU, so the buffer need not be contiguous) and handed on as DMA_DRM in the same layout; glupload imports it as it
 * imports the ISP's frames. The CPU writes are bracketed by DMA_BUF_IOCTL_SYNC so the GPU sees them. The VideoCore
 * is left to the hardware decoders, which share it with the ISP (h264-pi4 results/45: ISP + H.264 decode at once,
 * the decode drops from 92 to 70-79 fps).
 *
 * Buffers are allocated as needed, up to RING per element (the GPU wall keeps up to ~16 frames of a cue queued);
 * a buffer is reused once the GstBuffer carrying it is freed. A caps change drops the free buffers at once and
 * the ones still downstream when they come back. */
#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define RING 24
#define HEAP "/dev/dma_heap/system"

GST_DEBUG_CATEGORY_STATIC (sysdmabuf_debug);
#define GST_CAT_DEFAULT sysdmabuf_debug

typedef struct _SysDmabuf SysDmabuf;
typedef struct {
  SysDmabuf *self;
  int fd;                       /* owned by mem */
  uint8_t *map;
  gsize size;
  GstMemory *mem;
  gboolean busy, orphan;        /* orphan: dropped by a caps change while downstream; freed on release */
} Slot;

struct _SysDmabuf {
  GstBaseTransform parent;
  GstVideoInfo in;
  GstAllocator *alloc;
  int heap;
  Slot *slots[RING];
  int nslots;
  gsize size;
  gsize offset[GST_VIDEO_MAX_PLANES];
  gint stride[GST_VIDEO_MAX_PLANES];
  GMutex lock;
  GCond cond;
  gboolean flushing;
};
typedef struct { GstBaseTransformClass parent; } SysDmabufClass;
GType cutepi_sys_dmabuf_get_type (void);
G_DEFINE_TYPE (SysDmabuf, cutepi_sys_dmabuf, GST_TYPE_BASE_TRANSFORM);

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string){ I420, NV12 }"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(memory:DMABuf), format=(string)DMA_DRM, drm-format=(string){ YU12, NV12 }"));

static const char *
drm_name (const gchar * f)
{
  return !g_strcmp0 (f, "I420") ? "YU12" : !g_strcmp0 (f, "NV12") ? "NV12" : NULL;
}

static const char *
gst_name (const gchar * d)
{
  return !g_strcmp0 (d, "YU12") ? "I420" : !g_strcmp0 (d, "NV12") ? "NV12" : NULL;
}

/* format (system memory) <-> DMA_DRM drm-format (DMABuf); every other field is kept. */
static GstCaps *
s_transform_caps (GstBaseTransform * bt, GstPadDirection dir, GstCaps * caps, GstCaps * filter)
{
  static const gchar *all_in[] = { "I420", "NV12" }, *all_out[] = { "YU12", "NV12" };
  GstCaps *out = gst_caps_new_empty ();
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *s = gst_structure_copy (gst_caps_get_structure (caps, i));
    const GValue *fv = gst_structure_get_value (s, dir == GST_PAD_SINK ? "format" : "drm-format");
    const gchar *names[4] = { 0 };
    int n = 0;
    if (fv && G_VALUE_HOLDS_STRING (fv))
      names[n++] = g_value_get_string (fv);
    else if (fv && GST_VALUE_HOLDS_LIST (fv))
      for (guint k = 0; k < gst_value_list_get_size (fv) && n < 4; k++) {
        const GValue *v = gst_value_list_get_value (fv, k);
        if (G_VALUE_HOLDS_STRING (v))
          names[n++] = g_value_get_string (v);
      }
    else
      for (; n < 2; n++)
        names[n] = dir == GST_PAD_SINK ? all_in[n] : all_out[n];
    GValue list = G_VALUE_INIT;
    g_value_init (&list, GST_TYPE_LIST);
    for (int k = 0; k < n; k++) {
      const char *m = dir == GST_PAD_SINK ? drm_name (names[k]) : gst_name (names[k]);
      if (!m)
        continue;
      GValue v = G_VALUE_INIT;
      g_value_init (&v, G_TYPE_STRING);
      g_value_set_string (&v, m);
      gst_value_list_append_and_take_value (&list, &v);
    }
    if (gst_value_list_get_size (&list) == 0) {
      g_value_unset (&list);
      gst_structure_free (s);
      continue;
    }
    if (dir == GST_PAD_SINK) {
      gst_structure_set (s, "format", G_TYPE_STRING, "DMA_DRM", NULL);
      gst_structure_take_value (s, "drm-format", &list);
      gst_caps_append_structure_full (out, s, gst_caps_features_new_single_static_str (GST_CAPS_FEATURE_MEMORY_DMABUF));
    } else {
      gst_structure_remove_field (s, "drm-format");
      gst_structure_take_value (s, "format", &list);
      gst_caps_append_structure_full (out, s, gst_caps_features_new_single_static_str (GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY));
    }
  }
  if (filter) {
    GstCaps *t = gst_caps_intersect_full (filter, out, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (out);
    out = t;
  }
  return out;
}

static void
slot_free (Slot * s)
{
  if (s->map)
    munmap (s->map, s->size);
  if (s->mem)
    gst_memory_unref (s->mem);  /* closes the fd */
  g_free (s);
}

/* Drops every slot: free ones now, busy ones when their buffer is freed. Called with the lock held. */
static void
slots_drop (SysDmabuf * self)
{
  for (int i = 0; i < self->nslots; i++) {
    if (self->slots[i]->busy)
      self->slots[i]->orphan = TRUE;
    else
      slot_free (self->slots[i]);
    self->slots[i] = NULL;
  }
  self->nslots = 0;
}

/* A new slot of self->size bytes, or NULL. Called with the lock held. */
static Slot *
slot_new (SysDmabuf * self)
{
  struct dma_heap_allocation_data a = {.len = self->size,.fd_flags = O_RDWR | O_CLOEXEC };
  if (self->heap < 0 || ioctl (self->heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0) {
    GST_WARNING_OBJECT (self, "dma-heap allocation of %zu bytes failed: %s", self->size, g_strerror (errno));
    return NULL;
  }
  Slot *s = g_new0 (Slot, 1);
  s->self = self;
  s->fd = a.fd;
  s->size = self->size;
  s->mem = gst_dmabuf_allocator_alloc (self->alloc, a.fd, self->size);
  s->map = mmap (NULL, self->size, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
  if (s->map == MAP_FAILED) {
    GST_WARNING_OBJECT (self, "mmap of a dma-heap buffer failed: %s", g_strerror (errno));
    s->map = NULL;
    slot_free (s);
    return NULL;
  }
  return s;
}

static gboolean
s_set_caps (GstBaseTransform * bt, GstCaps * incaps, GstCaps * outcaps)
{
  SysDmabuf *self = (SysDmabuf *) bt;
  GstVideoInfo in;
  if (!gst_video_info_from_caps (&in, incaps))
    return FALSE;
  /* The output layout: each plane's rows padded to 64 bytes, planes one after the other. */
  gsize off = 0, offset[GST_VIDEO_MAX_PLANES] = { 0 };
  gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
  for (guint p = 0; p < GST_VIDEO_INFO_N_PLANES (&in); p++) {
    gint comp[GST_VIDEO_MAX_COMPONENTS];
    gst_video_format_info_component (in.finfo, p, comp);
    int h = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (in.finfo, comp[0], GST_VIDEO_INFO_HEIGHT (&in));
    stride[p] = GST_ROUND_UP_64 (GST_VIDEO_INFO_PLANE_STRIDE (&in, p));
    offset[p] = off;
    off += (gsize) stride[p] * h;
  }
  g_mutex_lock (&self->lock);
  self->in = in;
  memcpy (self->offset, offset, sizeof offset);
  memcpy (self->stride, stride, sizeof stride);
  self->size = GST_ROUND_UP_N (off, 4096);
  slots_drop (self);
  g_mutex_unlock (&self->lock);
  return self->heap >= 0;
}

static GQuark slot_quark;

static void
slot_released (gpointer data)
{
  Slot *s = data;
  SysDmabuf *self = s->self;
  g_mutex_lock (&self->lock);
  if (s->orphan)
    slot_free (s);
  else {
    s->busy = FALSE;
    g_cond_signal (&self->cond);
  }
  g_mutex_unlock (&self->lock);
  gst_object_unref (self);
}

/* A free slot (allocating one while fewer than RING exist), waiting up to 2 s for one to come back. */
static Slot *
slot_take (SysDmabuf * self)
{
  Slot *slot = NULL;
  gint64 end = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
  g_mutex_lock (&self->lock);
  while (!self->flushing) {
    for (int i = 0; i < self->nslots && !slot; i++)
      if (!self->slots[i]->busy)
        slot = self->slots[i];
    if (!slot && self->nslots < RING && (slot = slot_new (self)))
      self->slots[self->nslots++] = slot;
    if (slot || !g_cond_wait_until (&self->cond, &self->lock, end))
      break;
  }
  if (slot)
    slot->busy = TRUE;
  g_mutex_unlock (&self->lock);
  return slot;
}

static GstFlowReturn
s_prepare_output_buffer (GstBaseTransform * bt, GstBuffer * inbuf, GstBuffer ** outbuf)
{
  SysDmabuf *self = (SysDmabuf *) bt;
  Slot *slot = slot_take (self);
  if (!slot) {
    if (self->flushing)
      return GST_FLOW_FLUSHING;
    GST_ELEMENT_ERROR (self, RESOURCE, NO_SPACE_LEFT, ("no dma-heap buffer free"), (NULL));
    return GST_FLOW_ERROR;
  }

  GstVideoFrame f;
  if (!gst_video_frame_map (&f, &self->in, inbuf, GST_MAP_READ)) {
    g_mutex_lock (&self->lock);
    if (slot->orphan)
      slot_free (slot);
    else
      slot->busy = FALSE;
    g_mutex_unlock (&self->lock);
    return GST_FLOW_ERROR;
  }
  struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
  ioctl (slot->fd, DMA_BUF_IOCTL_SYNC, &sync);
  for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES (&f); p++) {
    const uint8_t *src = GST_VIDEO_FRAME_PLANE_DATA (&f, p);
    int sstr = GST_VIDEO_FRAME_PLANE_STRIDE (&f, p);
    /* bytes per row: NV12's UV plane holds two bytes per chroma sample */
    int rowb = GST_VIDEO_FRAME_COMP_WIDTH (&f, p) * (GST_VIDEO_INFO_FORMAT (&self->in) == GST_VIDEO_FORMAT_NV12 && p == 1 ?
        2 : GST_VIDEO_FRAME_COMP_PSTRIDE (&f, p));
    int h = GST_VIDEO_FRAME_COMP_HEIGHT (&f, p);
    uint8_t *dst = slot->map + self->offset[p];
    if (sstr == self->stride[p])
      memcpy (dst, src, (size_t) sstr * h);
    else
      for (int y = 0; y < h; y++)
        memcpy (dst + (size_t) y * self->stride[p], src + (size_t) y * sstr, rowb);
  }
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
  ioctl (slot->fd, DMA_BUF_IOCTL_SYNC, &sync);
  gst_video_frame_unmap (&f);

  GstBuffer *out = gst_buffer_new ();
  gst_buffer_append_memory (out, gst_memory_ref (slot->mem));
  gst_buffer_add_video_meta_full (out, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_INFO_FORMAT (&self->in),
      GST_VIDEO_INFO_WIDTH (&self->in), GST_VIDEO_INFO_HEIGHT (&self->in), GST_VIDEO_INFO_N_PLANES (&self->in),
      self->offset, self->stride);
  gst_buffer_copy_into (out, inbuf, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
  gst_object_ref (self);
  gst_mini_object_set_qdata (GST_MINI_OBJECT (out), slot_quark, slot, slot_released);
  *outbuf = out;
  return GST_FLOW_OK;
}

static GstFlowReturn
s_transform (GstBaseTransform * bt, GstBuffer * in, GstBuffer * out)
{
  return GST_FLOW_OK;           /* done in prepare_output_buffer */
}

static gboolean
s_propose_allocation (GstBaseTransform * bt, GstQuery * decide, GstQuery * q)
{
  gst_query_add_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL);     /* strided decoder output is fine */
  return TRUE;
}

static gboolean
s_start (GstBaseTransform * bt)
{
  SysDmabuf *self = (SysDmabuf *) bt;
  self->heap = open (HEAP, O_RDWR | O_CLOEXEC);
  if (self->heap < 0) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE, ("cannot open " HEAP), ("%s", g_strerror (errno)));
    return FALSE;
  }
  self->alloc = gst_dmabuf_allocator_new ();
  self->flushing = FALSE;
  return TRUE;
}

static gboolean
s_stop (GstBaseTransform * bt)
{
  SysDmabuf *self = (SysDmabuf *) bt;
  g_mutex_lock (&self->lock);
  self->flushing = TRUE;
  g_cond_broadcast (&self->cond);
  slots_drop (self);
  g_mutex_unlock (&self->lock);
  gst_clear_object (&self->alloc);      /* memories still downstream hold their own reference */
  if (self->heap >= 0)
    close (self->heap);
  self->heap = -1;
  return TRUE;
}

static gboolean
s_sink_event (GstBaseTransform * bt, GstEvent * ev)
{
  SysDmabuf *self = (SysDmabuf *) bt;
  if (GST_EVENT_TYPE (ev) == GST_EVENT_FLUSH_START || GST_EVENT_TYPE (ev) == GST_EVENT_FLUSH_STOP) {
    g_mutex_lock (&self->lock);
    self->flushing = GST_EVENT_TYPE (ev) == GST_EVENT_FLUSH_START;
    g_cond_broadcast (&self->cond);
    g_mutex_unlock (&self->lock);
  }
  return GST_BASE_TRANSFORM_CLASS (cutepi_sys_dmabuf_parent_class)->sink_event (bt, ev);
}

static void
s_finalize (GObject * o)
{
  SysDmabuf *self = (SysDmabuf *) o;
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  G_OBJECT_CLASS (cutepi_sys_dmabuf_parent_class)->finalize (o);
}

static void
cutepi_sys_dmabuf_class_init (SysDmabufClass * k)
{
  GstElementClass *ek = GST_ELEMENT_CLASS (k);
  GstBaseTransformClass *bk = GST_BASE_TRANSFORM_CLASS (k);
  G_OBJECT_CLASS (k)->finalize = s_finalize;
  gst_element_class_add_static_pad_template (ek, &sink_tmpl);
  gst_element_class_add_static_pad_template (ek, &src_tmpl);
  gst_element_class_set_static_metadata (ek, "System memory to dmabuf", "Filter/Video",
      "Copies software-decoded frames into system dma-heap buffers for the GPU", "CuTePi");
  bk->transform_caps = s_transform_caps;
  bk->set_caps = s_set_caps;
  bk->prepare_output_buffer = s_prepare_output_buffer;
  bk->transform = s_transform;
  bk->propose_allocation = s_propose_allocation;
  bk->start = s_start;
  bk->stop = s_stop;
  bk->sink_event = s_sink_event;
  bk->passthrough_on_same_caps = FALSE;
  slot_quark = g_quark_from_static_string ("cutepi-sysdmabuf-slot");
  GST_DEBUG_CATEGORY_INIT (sysdmabuf_debug, "cutepisysdmabuf", 0, "system memory to dmabuf");
}

static void
cutepi_sys_dmabuf_init (SysDmabuf * self)
{
  self->heap = -1;
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
}

/* 1 when the element registered and the system dma-heap can be opened by this process. */
int
cutepi_sysdmabuf_register (GstPlugin * plugin)
{
  int fd = open (HEAP, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return 0;
  close (fd);
  return gst_element_register (plugin, "cutepisysdmabuf", GST_RANK_NONE, cutepi_sys_dmabuf_get_type ());
}

#ifdef STANDALONE_PLUGIN
/* Standalone GStreamer plugin build (see this folder's Makefile). */
#ifndef PACKAGE
#define PACKAGE "cutepisysdmabuf"
#endif
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_sysdmabuf_register (p);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepisysdmabuf, "Copy video into system dma-heap buffers for zero-copy GPU import", plugin_init, "1.0", "LGPL",
    "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
