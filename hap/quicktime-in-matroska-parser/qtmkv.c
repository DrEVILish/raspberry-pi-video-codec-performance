/* cutepiqtmkvparse: QuickTime codecs in Matroska (DESIGN §12.16).
 *
 * Matroska carries QuickTime codecs (HAP, DNxHR/DNxHD, QuickTime Animation) as
 * V_QUICKTIME tracks whose CodecPrivate is the QuickTime sample description.
 * GStreamer's matroskademux does not map them: their pads say only
 * video/x-unknown, codec-id=V_QUICKTIME, without the fourcc, size or frame
 * rate, so nothing decodes them (FFmpeg reads the same files). This parser is
 * autoplugged on those pads. It takes the track number from the stream-id
 * (matroskademux ends it with "/<track number>"), asks upstream for the
 * file's URI, reads that track's header from the file itself (Tracks only,
 * a few kilobytes), and sets the caps qtdemux gives the same stream in a
 * MOV, so the decoder the MOV gets is plugged. Frames pass through as they
 * are. */
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (qtmkv_debug);
#define GST_CAT_DEFAULT qtmkv_debug

typedef struct {
  GstElement parent;
  GstPad *sinkpad, *srcpad;
  guint64 track;                /* 0: unknown */
  gboolean have_caps;
} CutepiQtMkv;
typedef struct { GstElementClass parent; } CutepiQtMkvClass;
GType cutepi_qtmkv_get_type (void);
G_DEFINE_TYPE (CutepiQtMkv, cutepi_qtmkv, GST_TYPE_ELEMENT);

static GstStaticPadTemplate sink_t = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-unknown, codec-id=(string)V_QUICKTIME"));
static GstStaticPadTemplate src_t = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-hap; video/x-dnxhd; video/x-rle, layout=(string)quicktime"));

/* ---- a minimal EBML reader -------------------------------------------------- */

#define ID_SEGMENT 0x18538067
#define ID_TRACKS 0x1654AE6B
#define ID_CLUSTER 0x1F43B675
#define ID_TRACKENTRY 0xAE
#define ID_TRACKNUMBER 0xD7
#define ID_CODECID 0x86
#define ID_CODECPRIVATE 0x63A2
#define ID_DEFAULTDURATION 0x23E383
#define ID_VIDEO 0xE0
#define ID_PIXELWIDTH 0xB0
#define ID_PIXELHEIGHT 0xBA
#define UNKNOWN_SIZE G_MAXUINT64

/* An EBML variable-length integer at p (n bytes left): its value (the length
 * marker kept for IDs, removed for sizes) and length, or 0 on error. */
static int
vint (const guint8 * p, gsize n, gboolean keep_marker, guint64 * v)
{
  if (n == 0 || p[0] == 0) return 0;
  int len = 1;
  while (!(p[0] & (0x80 >> (len - 1)))) len++;
  if ((gsize) len > n || len > 8) return 0;
  guint64 x = keep_marker ? p[0] : (p[0] & (0xFF >> len));
  gboolean all_ones = (p[0] | (0xFF << (8 - len))) == 0xFF;
  for (int i = 1; i < len; i++) {
    x = (x << 8) | p[i];
    all_ones = all_ones && p[i] == 0xFF;
  }
  *v = (!keep_marker && all_ones) ? UNKNOWN_SIZE : x;
  return len;
}

static guint64
uint_be (const guint8 * p, guint64 n)
{
  guint64 x = 0;
  for (guint64 i = 0; i < n && i < 8; i++) x = (x << 8) | p[i];
  return x;
}

typedef struct {
  guint64 num, width, height, default_duration;
  gboolean quicktime;
  const guint8 *priv;
  gsize priv_len;
} Track;

/* One TrackEntry's fields. */
static void
parse_track (const guint8 * p, gsize n, Track * t)
{
  memset (t, 0, sizeof *t);
  for (gsize o = 0; o < n;) {
    guint64 id, sz;
    int a = vint (p + o, n - o, TRUE, &id); if (!a) return;
    int b = vint (p + o + a, n - o - a, FALSE, &sz); if (!b) return;
    o += a + b;
    if (sz > n - o) return;
    const guint8 *d = p + o;
    switch (id) {
      case ID_TRACKNUMBER: t->num = uint_be (d, sz); break;
      case ID_CODECID: t->quicktime = sz >= 11 && memcmp (d, "V_QUICKTIME", 11) == 0; break;
      case ID_CODECPRIVATE: t->priv = d; t->priv_len = sz; break;
      case ID_DEFAULTDURATION: t->default_duration = uint_be (d, sz); break;
      case ID_VIDEO:
        for (gsize v = 0; v < sz;) {
          guint64 vid, vsz;
          int c = vint (d + v, sz - v, TRUE, &vid); if (!c) break;
          int e = vint (d + v + c, sz - v - c, FALSE, &vsz); if (!e) break;
          v += c + e;
          if (vsz > sz - v) break;
          if (vid == ID_PIXELWIDTH) t->width = uint_be (d + v, vsz);
          else if (vid == ID_PIXELHEIGHT) t->height = uint_be (d + v, vsz);
          v += vsz;
        }
        break;
    }
    o += sz;
  }
}

/* The caps qtdemux gives a QuickTime sample description, or NULL. */
static GstCaps *
caps_for (const Track * t)
{
  const guint8 *p = t->priv;
  gsize n = t->priv_len;
  if (!p || n < 4) return NULL;
  /* The sample description, with or without its leading size field. */
  gboolean sized = n >= 8 && GST_READ_UINT32_BE (p) <= n + 4 && g_ascii_isprint (p[4]) && g_ascii_isprint (p[7]);
  const guint8 *desc = sized ? p + 4 : p;
  gsize dn = sized ? n - 4 : n;
  gchar fourcc[5] = { desc[0], desc[1], desc[2], desc[3], 0 };
  GstCaps *c = NULL;
  if (g_str_has_prefix (fourcc, "Hap"))
    c = gst_caps_new_simple ("video/x-hap", "variant", G_TYPE_STRING, fourcc, NULL);
  else if (strcmp (fourcc, "AVdh") == 0 || strcmp (fourcc, "AVdn") == 0)
    c = gst_caps_new_empty_simple ("video/x-dnxhd");   /* DNxHR / DNxHD */
  else if (strcmp (fourcc, "rle ") == 0) {
    /* depth: 78 bytes into the description (after the fourcc). */
    /* FFmpeg's Matroska muxer writes the description zeroed after the
     * fourcc: no depth, and the decoder cannot tell RGB from ARGB. */
    int depth = dn >= 80 ? GST_READ_UINT16_BE (desc + 78) : 0;
    if (depth == 0) {
      GST_WARNING ("V_QUICKTIME track %" G_GUINT64_FORMAT ": QuickTime Animation without a depth", t->num);
      return NULL;
    }
    c = gst_caps_new_simple ("video/x-rle", "layout", G_TYPE_STRING, "quicktime", "depth", G_TYPE_INT, depth, NULL);
  }
  if (!c) {
    GST_WARNING ("V_QUICKTIME track %" G_GUINT64_FORMAT ": fourcc '%s' not mapped", t->num, fourcc);
    return NULL;
  }
  if (t->width && t->height)
    gst_caps_set_simple (c, "width", G_TYPE_INT, (gint) t->width, "height", G_TYPE_INT, (gint) t->height, NULL);
  if (t->default_duration) {
    gint fn, fd;
    gst_util_double_to_fraction (1e9 / (gdouble) t->default_duration, &fn, &fd);
    gst_caps_set_simple (c, "framerate", GST_TYPE_FRACTION, fn, fd, NULL);
  }
  gst_caps_set_simple (c, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
  return c;
}

/* The caps for V_QUICKTIME track number track (0: the first such track) of
 * the Matroska file at path, or NULL. */
GstCaps *
cutepi_qtmkv_caps_from_file (const gchar * path, guint64 track)
{
  FILE *f = fopen (path, "rb");
  if (!f) return NULL;
  GstCaps *caps = NULL;
  guint8 hdr[16];
  guint64 pos = 0, end = G_MAXUINT64;
  /* Top level: skip the EBML header, enter the Segment, walk its children
   * up to Tracks (it comes before the clusters in practice). */
  for (int steps = 0; steps < 64 && !caps; steps++) {
    if (fseeko (f, (off_t) pos, SEEK_SET) || fread (hdr, 1, sizeof hdr, f) < 2) break;
    guint64 id, sz;
    int a = vint (hdr, sizeof hdr, TRUE, &id); if (!a) break;
    int b = vint (hdr + a, sizeof hdr - a, FALSE, &sz); if (!b) break;
    guint64 body = pos + a + b;
    if (id == ID_SEGMENT) { pos = body; end = sz == UNKNOWN_SIZE ? G_MAXUINT64 : body + sz; continue; }
    if (id == ID_CLUSTER || sz == UNKNOWN_SIZE || body >= end) break;
    if (id == ID_TRACKS && sz < 4 * 1024 * 1024) {
      guint8 *buf = g_malloc (sz);
      if (fseeko (f, (off_t) body, SEEK_SET) == 0 && fread (buf, 1, sz, f) == sz) {
        for (gsize o = 0; o < sz && !caps;) {
          guint64 eid, esz;
          int c = vint (buf + o, sz - o, TRUE, &eid); if (!c) break;
          int e = vint (buf + o + c, sz - o - c, FALSE, &esz); if (!e) break;
          o += c + e;
          if (esz > sz - o) break;
          if (eid == ID_TRACKENTRY) {
            Track t;
            parse_track (buf + o, esz, &t);
            if (t.quicktime && (track == 0 || t.num == track)) caps = caps_for (&t);
          }
          o += esz;
        }
      }
      g_free (buf);
      break;
    }
    pos = body + sz;
  }
  fclose (f);
  return caps;
}

/* ---- the element ------------------------------------------------------------ */

static gboolean
qtmkv_set_caps (CutepiQtMkv * q)
{
  GstQuery *uq = gst_query_new_uri ();
  gchar *uri = NULL;
  if (gst_pad_peer_query (q->sinkpad, uq)) gst_query_parse_uri (uq, &uri);
  gst_query_unref (uq);
  gchar *path = uri ? g_filename_from_uri (uri, NULL, NULL) : NULL;
  GstCaps *caps = path ? cutepi_qtmkv_caps_from_file (path, q->track) : NULL;
  if (!caps) {
    GST_ELEMENT_ERROR (q, STREAM, CODEC_NOT_FOUND, ("QuickTime codec in Matroska not recognised"),
        ("uri %s, track %" G_GUINT64_FORMAT, uri ? uri : "(none)", q->track));
    g_free (uri); g_free (path);
    return FALSE;
  }
  GST_INFO_OBJECT (q, "track %" G_GUINT64_FORMAT " of %s: %" GST_PTR_FORMAT, q->track, path, caps);
  g_free (uri); g_free (path);
  gboolean ok = gst_pad_push_event (q->srcpad, gst_event_new_caps (caps));
  gst_caps_unref (caps);
  q->have_caps = ok;
  return ok;
}

static gboolean
qtmkv_sink_event (GstPad * pad, GstObject * parent, GstEvent * ev)
{
  CutepiQtMkv *q = (CutepiQtMkv *) parent;
  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_STREAM_START: {
      const gchar *sid = NULL;
      gst_event_parse_stream_start (ev, &sid);
      const gchar *slash = sid ? strrchr (sid, '/') : NULL;
      q->track = slash ? g_ascii_strtoull (slash + 1, NULL, 10) : 0;
      return gst_pad_event_default (pad, parent, ev);
    }
    case GST_EVENT_CAPS:
      gst_event_unref (ev);
      return q->have_caps || qtmkv_set_caps (q);
    default:
      return gst_pad_event_default (pad, parent, ev);
  }
}

static GstFlowReturn
qtmkv_chain (GstPad * pad, GstObject * parent, GstBuffer * b)
{
  CutepiQtMkv *q = (CutepiQtMkv *) parent;
  if (!q->have_caps) { gst_buffer_unref (b); return GST_FLOW_NOT_NEGOTIATED; }
  return gst_pad_push (q->srcpad, b);
}

static GstStateChangeReturn
qtmkv_change_state (GstElement * e, GstStateChange t)
{
  if (t == GST_STATE_CHANGE_READY_TO_PAUSED) ((CutepiQtMkv *) e)->have_caps = FALSE;
  return GST_ELEMENT_CLASS (cutepi_qtmkv_parent_class)->change_state (e, t);
}

static void
cutepi_qtmkv_class_init (CutepiQtMkvClass * k)
{
  GST_ELEMENT_CLASS (k)->change_state = qtmkv_change_state;
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (k), &sink_t);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (k), &src_t);
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (k), "CuTePi QuickTime-in-Matroska parser",
      "Codec/Parser/Video", "Caps for Matroska V_QUICKTIME tracks (HAP, DNxHR, QuickTime Animation) from the track header",
      "CuTePi");
}

static void
cutepi_qtmkv_init (CutepiQtMkv * q)
{
  q->sinkpad = gst_pad_new_from_static_template (&sink_t, "sink");
  gst_pad_set_chain_function (q->sinkpad, qtmkv_chain);
  gst_pad_set_event_function (q->sinkpad, qtmkv_sink_event);
  gst_element_add_pad (GST_ELEMENT (q), q->sinkpad);
  q->srcpad = gst_pad_new_from_static_template (&src_t, "src");
  gst_pad_use_fixed_caps (q->srcpad);
  gst_element_add_pad (GST_ELEMENT (q), q->srcpad);
}

int
cutepi_qtmkv_register (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (qtmkv_debug, "cutepiqtmkvparse", 0, "CuTePi QuickTime-in-Matroska parser");
  return gst_element_register (plugin, "cutepiqtmkvparse", GST_RANK_PRIMARY, cutepi_qtmkv_get_type ());
}

#ifdef STANDALONE_PLUGIN
/* Standalone GStreamer plugin build (see this folder's Makefile). */
#ifndef PACKAGE
#define PACKAGE "cutepiqtmkvparse"
#endif
static gboolean
plugin_init (GstPlugin * p)
{
  return cutepi_qtmkv_register (p);
}
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, cutepiqtmkvparse, "Caps for QuickTime codecs (HAP, DNxHR, QuickTime Animation) in Matroska", plugin_init, "1.0", "LGPL",
    "raspberry-pi-video-codec-performance", "https://github.com/DrEVILish/raspberry-pi-video-codec-performance")
#endif
