/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DFF's chronological, byte-interleaved DSD -> ALSA-native words or DoP.
 * PCM passes unchanged. No DSD-to-PCM decoding or digital gain occurs here.
 */
#include "dsd_output.h"
#include <gst/base/gstbasetransform.h>
#include <string.h>

#define RAW "audio/x-raw,layout=(string)interleaved"
#define DSD_IN "audio/x-dsd,format=(string)DSDU8,layout=(string)interleaved,reversed-bytes=(boolean)false,channels=(int)2"
#define DSD_OUT "audio/x-dsd,format=(string){DSDU32BE,DSDU32LE,DSDU16BE,DSDU16LE,DSDU8},layout=(string)interleaved,reversed-bytes=(boolean)false,channels=(int)2"

typedef struct {
    GstBaseTransform parent;
    gboolean dop;
    gboolean converting;
    gboolean little_endian;
    guint width;
    guint marker;
} GmDsd;
typedef struct { GstBaseTransformClass parent; } GmDsdClass;

G_DEFINE_TYPE(GmDsd, gm_dsd, GST_TYPE_BASE_TRANSFORM)

enum { PROP_0, PROP_DOP };

static GstStaticPadTemplate input_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(RAW ";" DSD_IN));
static GstStaticPadTemplate output_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(RAW ";" DSD_OUT));

static gboolean intersects(GstCaps *a, GstCaps *b) {
    return gst_caps_can_intersect(a, b);
}

static GstCaps *dsd_caps(gint rate, const char *format) {
    return gst_caps_new_simple("audio/x-dsd", "format", G_TYPE_STRING, format,
        "layout", G_TYPE_STRING, "interleaved", "reversed-bytes", G_TYPE_BOOLEAN, FALSE,
        "channels", G_TYPE_INT, 2, "rate", G_TYPE_INT, rate, NULL);
}
static GstCaps *dop_caps(gint rate, const char *format) {
    return gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, format,
        "layout", G_TYPE_STRING, "interleaved", "channels", G_TYPE_INT, 2,
        "rate", G_TYPE_INT, rate / 2, NULL);
}

static GstCaps *convert_caps(GstBaseTransform *base, GstPadDirection direction,
                             GstCaps *caps, GstCaps *filter) {
    GmDsd *self = (GmDsd *)base;
    GstCaps *raw = gst_caps_from_string(RAW);
    GstCaps *result = gst_caps_intersect_full(caps, raw, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(raw);
    /* Standard 44.1 and 48 kHz family DSD64..512. Rates are DSD bytes/sec/channel. */
    const gint rates[] = {352800,705600,1411200,2822400,384000,768000,1536000,3072000};
    const char *native_formats[] = {"DSDU32BE","DSDU32LE","DSDU16BE","DSDU16LE","DSDU8",NULL};
    const char *dop_formats[] = {"S32LE","S24LE","S24_32LE",NULL};
    const char **formats = self->dop ? dop_formats : native_formats;
    for (guint i = 0; i < G_N_ELEMENTS(rates); ++i) {
        GstCaps *in = dsd_caps(rates[i], "DSDU8");
        for (guint j = 0; formats[j]; ++j) {
            GstCaps *out = self->dop ? dop_caps(rates[i], formats[j]) : dsd_caps(rates[i], formats[j]);
            if (direction == GST_PAD_SINK && intersects(caps, in))
                gst_caps_append(result, gst_caps_ref(out));
            else if (direction == GST_PAD_SRC && intersects(caps, out))
                gst_caps_append(result, gst_caps_ref(in));
            gst_caps_unref(out);
        }
        gst_caps_unref(in);
    }
    result = gst_caps_simplify(result);
    if (filter) {
        GstCaps *filtered = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(result);
        result = filtered;
    }
    return result;
}

static gboolean configure_caps(GstBaseTransform *base, GstCaps *in, GstCaps *out) {
    GmDsd *self = (GmDsd *)base;
    const GstStructure *a = gst_caps_get_structure(in, 0), *b = gst_caps_get_structure(out, 0);
    self->converting = gst_structure_has_name(a, "audio/x-dsd");
    self->marker = 0x05;
    self->little_endian = FALSE;
    self->width = 0;
    if (!self->converting) return gst_caps_is_equal(in, out);
    int ir, orate, ic, oc;
    const char *infmt = gst_structure_get_string(a, "format");
    const char *outfmt = gst_structure_get_string(b, "format");
    if (!infmt || strcmp(infmt, "DSDU8") || !outfmt ||
        !gst_structure_get_int(a,"rate",&ir) || !gst_structure_get_int(b,"rate",&orate) ||
        !gst_structure_get_int(a,"channels",&ic) || !gst_structure_get_int(b,"channels",&oc) || ic != 2 || oc != 2)
        return FALSE;
    if (self->dop) {
        if (!gst_structure_has_name(b, "audio/x-raw") || ir != orate * 2) return FALSE;
        if (!strcmp(outfmt,"S24LE")) self->width = 3;
        else if (!strcmp(outfmt,"S32LE") || !strcmp(outfmt,"S24_32LE")) self->width = 4;
        else return FALSE;
        /* S32LE has an extra low byte; S24_32LE has a high sign extension. */
        self->little_endian = !strcmp(outfmt,"S32LE");
    } else {
        if (!gst_structure_has_name(b, "audio/x-dsd") || ir != orate) return FALSE;
        if (!strcmp(outfmt,"DSDU8")) self->width = 1;
        else if (!strcmp(outfmt,"DSDU16BE") || !strcmp(outfmt,"DSDU16LE")) self->width = 2;
        else if (!strcmp(outfmt,"DSDU32BE") || !strcmp(outfmt,"DSDU32LE")) self->width = 4;
        else return FALSE;
        self->little_endian = g_str_has_suffix(outfmt,"LE");
    }
    GST_INFO_OBJECT(self, "DSD %s -> %s at %d bytes/sec/channel", self->dop ? "DoP" : "native", outfmt, ir);
    return TRUE;
}
static gboolean convert_size(GstBaseTransform *base, GstPadDirection direction,
                             GstCaps *caps, gsize size, GstCaps *othercaps, gsize *othersize) {
    (void)base;
    gboolean dsd = gst_structure_has_name(gst_caps_get_structure(caps,0),"audio/x-dsd");
    gboolean other_dsd = gst_structure_has_name(gst_caps_get_structure(othercaps,0),"audio/x-dsd");
    if (dsd == other_dsd) { *othersize = size; return TRUE; }
    const char *format = gst_structure_get_string(gst_caps_get_structure(dsd ? othercaps : caps,0),"format");
    if (!format) return FALSE;
    guint width = !strcmp(format,"S24LE") ? 3 : 4;
    if ((direction == GST_PAD_SINK && !dsd) || (direction == GST_PAD_SRC && dsd)) return FALSE;
    if (dsd) {
        if (size % 4 || size / 4 > G_MAXSIZE / (2 * width)) return FALSE;
        *othersize = size / 4 * 2 * width;
    } else {
        if (size % (2 * width)) return FALSE;
        *othersize = size / (2 * width) * 4;
    }
    return TRUE;
}

static GstFlowReturn convert_buffer(GstBaseTransform *base, GstBuffer *in, GstBuffer *out) {
    GmDsd *self = (GmDsd *)base;
    GstMapInfo a, b;
    if (!gst_buffer_map(in, &a, GST_MAP_READ)) return GST_FLOW_ERROR;
    if (!gst_buffer_map(out, &b, GST_MAP_WRITE)) { gst_buffer_unmap(in,&a); return GST_FLOW_ERROR; }
    GstFlowReturn rc = GST_FLOW_OK;
    guint group = self->dop ? 2 : self->width;
    guint stride = group * 2, output_stride = self->width * 2;
    if (!self->converting || !stride || a.size % stride || b.size != a.size / stride * output_stride) {
        /* Never discard or reinterpret incomplete DSD words as PCM. */
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Unaligned DSD buffer"), ("input size %" G_GSIZE_FORMAT, a.size));
        rc = GST_FLOW_ERROR;
        goto finish;
    }
    if (GST_BUFFER_FLAG_IS_SET(in,GST_BUFFER_FLAG_DISCONT)) self->marker = 0x05;
    for (gsize i = 0, o = 0; i < a.size; i += stride, o += output_stride) {
        for (guint ch = 0; ch < 2; ++ch) {
            guint8 *dst = b.data + o + ch * self->width;
            if (self->dop) {
                const guint8 first = a.data[i+ch], second = a.data[i+2+ch];
                if (self->width == 4 && self->little_endian) {
                    dst[0] = 0; dst[1] = second; dst[2] = first; dst[3] = self->marker;
                } else {
                    dst[0] = second; dst[1] = first; dst[2] = self->marker;
                    if (self->width == 4) dst[3] = self->marker == 0xfa ? 0xff : 0;
                }
            } else {
                for (guint j = 0; j < group; ++j)
                    dst[j] = a.data[i+2*(self->little_endian ? group-1-j : j)+ch];
            }
        }
        if (self->dop) self->marker = self->marker == 0x05 ? 0xfa : 0x05;
    }
finish:
    gst_buffer_unmap(out,&b);
    gst_buffer_unmap(in,&a);
    return rc;
}
static void set_property(GObject *obj, guint id, const GValue *value, GParamSpec *pspec) {
    if (id == PROP_DOP) ((GmDsd *)obj)->dop = g_value_get_boolean(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(obj,id,pspec);
}
static void get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec) {
    if (id == PROP_DOP) g_value_set_boolean(value,((GmDsd *)obj)->dop);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(obj,id,pspec);
}

static void gm_dsd_class_init(GmDsdClass *klass) {
    GObjectClass *obj = G_OBJECT_CLASS(klass);
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *base = GST_BASE_TRANSFORM_CLASS(klass);
    obj->set_property = set_property;
    obj->get_property = get_property;
    g_object_class_install_property(obj,PROP_DOP,g_param_spec_boolean("dop","DoP","Pack DSD in PCM frames",FALSE,G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
    gst_element_class_set_static_metadata(element,"gmrender DSD output","Filter/Converter/Audio","Lossless native DSD grouping or DoP framing","gmrender-audirvana");
    gst_element_class_add_static_pad_template(element,&input_template);
    gst_element_class_add_static_pad_template(element,&output_template);
    base->transform_caps = convert_caps;
    base->set_caps = configure_caps;
    base->transform_size = convert_size;
    base->transform = convert_buffer;
    base->passthrough_on_same_caps = TRUE;
}
static void gm_dsd_init(GmDsd *self) { self->marker = 0x05; }
gboolean gm_dsd_register(void) {
    return gst_element_register(NULL,"gmrenderdsd",GST_RANK_NONE,gm_dsd_get_type());
}
GstElement *gm_dsd_sink_new(GstElement *sink, gboolean dop) {
    GstElement *bin = gst_bin_new("direct-dsd-output");
    GstElement *converter = g_object_new(gm_dsd_get_type(),"dop",dop,NULL);
    gst_bin_add_many(GST_BIN(bin),converter,sink,NULL);
    if (!gst_element_link(converter,sink)) { gst_object_unref(bin); return NULL; }
    GstPad *pad = gst_element_get_static_pad(converter,"sink");
    GstPad *ghost = gst_ghost_pad_new("sink",pad);
    gst_object_unref(pad);
    if (!ghost || !gst_element_add_pad(bin,ghost)) {
        if (ghost) gst_object_unref(ghost);
        gst_object_unref(bin);
        return NULL;
    }
    return bin;
}
