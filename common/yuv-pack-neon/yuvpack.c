/* cutepiyuvpack: 10-bit 4:2:0/4:2:2 and 8-bit 4:2:2 frames to 8-bit I420, one NEON pass.
 *
 * kmssink cannot allocate display buffers for 4:2:2 (its pool fails to activate), so on the KMS wall a ProRes or
 * DNxHR frame (10-bit 4:2:2) was converted by videoconvert, to RGB: 0.8-14 fps (README codec table). The planes do
 * take I420. This element packs luma with a rounding, saturating shift-and-narrow and averages chroma rows in
 * pairs: 2.35 ms for a 1080p 10-bit 4:2:2 frame on one Pi 4 core, against 20 ms for videoconvert to I420 (h264-pi4
 * results/36). Any other format passes through untouched. Portable C where NEON is absent. */
#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <stdint.h>
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

typedef struct {
  GstBaseTransform parent;
  GstVideoInfo in, out;
} CutepiYuvPack;
typedef struct { GstBaseTransformClass parent; } CutepiYuvPackClass;
GType cutepi_yuv_pack_get_type (void);
G_DEFINE_TYPE (CutepiYuvPack, cutepi_yuv_pack, GST_TYPE_BASE_TRANSFORM);

#define PACKED_FORMATS "{ I420_10LE, I422_10LE, Y42B }"
static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(ANY)"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(ANY)"));

static gboolean
is_packed (const gchar * f)
{
  return f && (!strcmp (f, "I420_10LE") || !strcmp (f, "I422_10LE") || !strcmp (f, "Y42B"));
}

/* Downstream: a packed format becomes I420 (or stays itself: passthrough is allowed). Upstream: I420 may come from
 * any packed format. Everything else maps to itself. */
static GstCaps *
p_transform_caps (GstBaseTransform * bt, GstPadDirection dir, GstCaps * caps, GstCaps * filter)
{
  GstCaps *out = gst_caps_new_empty ();
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);
    GstCapsFeatures *f = gst_caps_get_features (caps, i);
    gst_caps_append_structure_full (out, gst_structure_copy (s), f ? gst_caps_features_copy (f) : NULL);
    if (f && !gst_caps_features_is_equal (f, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
      continue;
    const GValue *fv = gst_structure_get_value (s, "format");
    gboolean any = !fv;
    gboolean packed = FALSE, i420 = FALSE;
    if (fv && G_VALUE_HOLDS_STRING (fv)) {
      packed = is_packed (g_value_get_string (fv));
      i420 = !strcmp (g_value_get_string (fv), "I420");
    } else if (fv && GST_VALUE_HOLDS_LIST (fv)) {
      for (guint k = 0; k < gst_value_list_get_size (fv); k++) {
        const gchar *n = g_value_get_string (gst_value_list_get_value (fv, k));
        packed |= is_packed (n);
        i420 |= !strcmp (n, "I420");
      }
    }
    if (dir == GST_PAD_SINK && (packed || any)) {
      GstStructure *t = gst_structure_copy (s);
      gst_structure_set (t, "format", G_TYPE_STRING, "I420", NULL);
      gst_caps_append_structure (out, t);
    } else if (dir == GST_PAD_SRC && (i420 || any)) {
      GstStructure *t = gst_structure_copy (s);
      GValue l = G_VALUE_INIT;
      g_value_init (&l, GST_TYPE_LIST);
      gst_value_deserialize (&l, PACKED_FORMATS);
      gst_structure_set_value (t, "format", &l);
      g_value_unset (&l);
      gst_caps_append_structure (out, t);
    }
  }
  out = gst_caps_simplify (out);
  if (filter) {
    GstCaps *t = gst_caps_intersect_full (filter, out, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (out);
    out = t;
  }
  return out;
}

/* Prefer I420 downstream when the input is a packed format (the point of the element). */
static GstCaps *
p_fixate_caps (GstBaseTransform * bt, GstPadDirection dir, GstCaps * caps, GstCaps * othercaps)
{
  othercaps = gst_caps_make_writable (othercaps);
  if (dir == GST_PAD_SINK) {
    const gchar *f = gst_structure_get_string (gst_caps_get_structure (caps, 0), "format");
    if (is_packed (f)) {
      GstCaps *want = gst_caps_from_string ("video/x-raw, format=(string)I420");
      GstCaps *t = gst_caps_intersect_full (othercaps, want, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (want);
      if (!gst_caps_is_empty (t)) {
        gst_caps_unref (othercaps);
        othercaps = t;
      } else
        gst_caps_unref (t);
    }
  }
  return gst_caps_fixate (othercaps);
}

static gboolean
p_set_caps (GstBaseTransform * bt, GstCaps * incaps, GstCaps * outcaps)
{
  CutepiYuvPack *self = (CutepiYuvPack *) bt;
  if (!gst_video_info_from_caps (&self->in, incaps) || !gst_video_info_from_caps (&self->out, outcaps))
    return FALSE;
  gst_base_transform_set_passthrough (bt, gst_caps_is_equal (incaps, outcaps));
  return TRUE;
}

static gboolean
p_propose_allocation (GstBaseTransform * bt, GstQuery * decide, GstQuery * q)
{
  /* Strided input is fine: we read through GstVideoFrame. */
  gst_query_add_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL);
  return GST_BASE_TRANSFORM_CLASS (cutepi_yuv_pack_parent_class)->propose_allocation (bt, decide, q);
}

static inline uint8_t
narrow (unsigned v, int shift)
{
  unsigned t = shift ? (v + (1u << (shift - 1))) >> shift : v;
  return t > 255 ? 255 : t;
}

/* One row: 16-bit 10-bit samples to 8 bits. */
static void
row10 (const uint16_t * s, uint8_t * d, int w)
{
  int x = 0;
#ifdef __ARM_NEON
  for (; x + 16 <= w; x += 16) {
    uint16x8x2_t v = vld1q_u16_x2 (s + x);
    vst1q_u8 (d + x, vcombine_u8 (vqrshrn_n_u16 (v.val[0], 2), vqrshrn_n_u16 (v.val[1], 2)));
  }
#endif
  for (; x < w; x++)
    d[x] = narrow (s[x], 2);
}

/* Two 10-bit chroma rows averaged, to 8 bits. */
static void
row10_avg (const uint16_t * a, const uint16_t * b, uint8_t * d, int w)
{
  int x = 0;
#ifdef __ARM_NEON
  for (; x + 16 <= w; x += 16) {
    uint16x8x2_t va = vld1q_u16_x2 (a + x), vb = vld1q_u16_x2 (b + x);
    vst1q_u8 (d + x, vcombine_u8 (vqrshrn_n_u16 (vrhaddq_u16 (va.val[0], vb.val[0]), 2),
            vqrshrn_n_u16 (vrhaddq_u16 (va.val[1], vb.val[1]), 2)));
  }
#endif
  for (; x < w; x++)
    d[x] = narrow ((a[x] + b[x] + 1) >> 1, 2);
}

/* Two 8-bit chroma rows averaged. */
static void
row8_avg (const uint8_t * a, const uint8_t * b, uint8_t * d, int w)
{
  int x = 0;
#ifdef __ARM_NEON
  for (; x + 16 <= w; x += 16)
    vst1q_u8 (d + x, vrhaddq_u8 (vld1q_u8 (a + x), vld1q_u8 (b + x)));
#endif
  for (; x < w; x++)
    d[x] = (a[x] + b[x] + 1) >> 1;
}

static GstFlowReturn
p_transform (GstBaseTransform * bt, GstBuffer * inbuf, GstBuffer * outbuf)
{
  CutepiYuvPack *self = (CutepiYuvPack *) bt;
  GstVideoFrame s, d;
  if (!gst_video_frame_map (&s, &self->in, inbuf, GST_MAP_READ))
    return GST_FLOW_ERROR;
  if (!gst_video_frame_map (&d, &self->out, outbuf, GST_MAP_WRITE)) {
    gst_video_frame_unmap (&s);
    return GST_FLOW_ERROR;
  }
  GstVideoFormat f = GST_VIDEO_INFO_FORMAT (&self->in);
  int w = GST_VIDEO_INFO_WIDTH (&self->in), h = GST_VIDEO_INFO_HEIGHT (&self->in);
  int hi = f != GST_VIDEO_FORMAT_Y42B;
  int ch422 = f != GST_VIDEO_FORMAT_I420_10LE;  /* source chroma has full height */
#define ROW(fr, p, y) ((uint8_t *) GST_VIDEO_FRAME_PLANE_DATA (&fr, p) + (size_t) (y) * GST_VIDEO_FRAME_PLANE_STRIDE (&fr, p))
  for (int y = 0; y < h; y++) {
    if (hi)
      row10 ((const uint16_t *) ROW (s, 0, y), ROW (d, 0, y), w);
    else
      memcpy (ROW (d, 0, y), ROW (s, 0, y), w);
  }
  int cw = (w + 1) / 2, chh = (h + 1) / 2;
  for (int p = 1; p < 3; p++)
    for (int y = 0; y < chh; y++) {
      if (!ch422)
        row10 ((const uint16_t *) ROW (s, p, y), ROW (d, p, y), cw);
      else {
        int y1 = MIN (2 * y + 1, h - 1);
        if (hi)
          row10_avg ((const uint16_t *) ROW (s, p, 2 * y), (const uint16_t *) ROW (s, p, y1), ROW (d, p, y), cw);
        else
          row8_avg (ROW (s, p, 2 * y), ROW (s, p, y1), ROW (d, p, y), cw);
      }
    }
#undef ROW
  gst_video_frame_unmap (&d);
  gst_video_frame_unmap (&s);
  return GST_FLOW_OK;
}

static void
cutepi_yuv_pack_class_init (CutepiYuvPackClass * k)
{
  GstElementClass *ek = GST_ELEMENT_CLASS (k);
  GstBaseTransformClass *bk = GST_BASE_TRANSFORM_CLASS (k);
  gst_element_class_add_static_pad_template (ek, &sink_tmpl);
  gst_element_class_add_static_pad_template (ek, &src_tmpl);
  gst_element_class_set_static_metadata (ek, "10-bit/4:2:2 to I420", "Filter/Converter/Video",
      "Packs 10-bit 4:2:0/4:2:2 and 8-bit 4:2:2 frames to 8-bit I420 in one NEON pass", "CuTePi");
  bk->transform_caps = p_transform_caps;
  bk->fixate_caps = p_fixate_caps;
  bk->set_caps = p_set_caps;
  bk->propose_allocation = p_propose_allocation;
  bk->transform = p_transform;
  bk->passthrough_on_same_caps = TRUE;
}

static void
cutepi_yuv_pack_init (CutepiYuvPack * self)
{
}

int
cutepi_yuvpack_register (GstPlugin * plugin)
{
  return gst_element_register (plugin, "cutepiyuvpack", GST_RANK_NONE, cutepi_yuv_pack_get_type ());
}

#ifdef YUVPACK_PLUGIN
#define PACKAGE "cutepiyuvpack"
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_yuvpack_register (p);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepiyuvpack, "10-bit/4:2:2 to I420", plugin_init, "1.0",
    "LGPL", "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
