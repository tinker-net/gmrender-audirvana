/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/dsd_output.h"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <string.h>

static void check_sample(GstElement *sink, const guint8 *expected, gsize length) {
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5*GST_SECOND);
    g_assert_nonnull(sample);
    GstMapInfo map;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    g_assert_true(gst_buffer_map(buf, &map, GST_MAP_READ));
    g_assert_cmpuint(map.size, ==, length);
    g_assert_cmpmem(map.data, map.size, expected, length);
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
}

static void push(GstElement *source, const guint8 *data, gsize length, GstClockTime pts) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, length, NULL);
    gst_buffer_fill(buffer,0,data,length);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = GST_MSECOND;
    g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source),buffer),==,GST_FLOW_OK);
}

static void run(const char *input_caps, const char *output_caps, gboolean dop,
                const guint8 *data, gsize length, const guint8 *expected,
                const guint8 *second, gsize output_length) {
    GstElement *pipe = gst_pipeline_new(NULL);
    GstElement *source = gst_element_factory_make("appsrc",NULL);
    GstElement *sink = gst_element_factory_make("appsink",NULL);
    g_assert_nonnull(pipe); g_assert_nonnull(source); g_assert_nonnull(sink);
    GstCaps *in = gst_caps_from_string(input_caps);
    GstCaps *out = gst_caps_from_string(output_caps);
    gst_app_src_set_caps(GST_APP_SRC(source),in);
    gst_app_sink_set_caps(GST_APP_SINK(sink),out);
    g_object_set(source,"format",GST_FORMAT_TIME,NULL);
    g_object_set(sink,"sync",FALSE,NULL);
    gst_caps_unref(in); gst_caps_unref(out);
    GstElement *bin = gm_dsd_sink_new(sink,dop);
    g_assert_nonnull(bin);
    gst_bin_add_many(GST_BIN(pipe),source,bin,NULL);
    g_assert_true(gst_element_link(source,bin));
    g_assert_cmpint(gst_element_set_state(pipe,GST_STATE_PLAYING),!=,GST_STATE_CHANGE_FAILURE);
    push(source,data,length,0);
    if (second) push(source,data,length,GST_MSECOND);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    check_sample(sink,expected,output_length);
    if (second) check_sample(sink,second,output_length);
    gst_element_set_state(pipe,GST_STATE_NULL);
    gst_object_unref(pipe);
}
#define DSD "audio/x-dsd,layout=interleaved,reversed-bytes=false,rate=352800,channels=2,format="
#define PCM "audio/x-raw,layout=interleaved,rate=176400,channels=2,format="
int main(int argc, char **argv) {
    gst_init(&argc,&argv);
    const guint8 input[] = {0x10,0x20,0x11,0x21,0x12,0x22,0x13,0x23};
    const guint8 be[] = {0x10,0x11,0x12,0x13,0x20,0x21,0x22,0x23};
    const guint8 le[] = {0x13,0x12,0x11,0x10,0x23,0x22,0x21,0x20};
    const guint8 be16[] = {0x10,0x11,0x20,0x21,0x12,0x13,0x22,0x23};
    const guint8 dop24a[] = {0x11,0x10,0x05,0x21,0x20,0x05};
    const guint8 dop24b[] = {0x11,0x10,0xfa,0x21,0x20,0xfa};
    const guint8 dop32a[] = {0,0x11,0x10,0x05,0,0x21,0x20,0x05};
    const guint8 dop32b[] = {0,0x11,0x10,0xfa,0,0x21,0x20,0xfa};
    const guint8 packed24a[] = {0x11,0x10,0x05,0,0x21,0x20,0x05,0};
    const guint8 packed24b[] = {0x11,0x10,0xfa,0xff,0x21,0x20,0xfa,0xff};
    run(DSD "DSDU8", DSD "DSDU32BE", FALSE,input,8,be,NULL,8);
    run(DSD "DSDU8", DSD "DSDU32LE", FALSE,input,8,le,NULL,8);
    run(DSD "DSDU8", DSD "DSDU16BE", FALSE,input,8,be16,NULL,8);
    run(DSD "DSDU8", PCM "S24LE", TRUE,input,4,dop24a,dop24b,6);
    run(DSD "DSDU8", PCM "S32LE", TRUE,input,4,dop32a,dop32b,8);
    run(DSD "DSDU8", PCM "S24_32LE", TRUE,input,4,packed24a,packed24b,8);
    run(PCM "S16LE", PCM "S16LE", FALSE,input,8,input,NULL,8);
    run(PCM "S16LE", PCM "S16LE", TRUE,input,8,input,NULL,8);
    g_print("PASS: native DSD grouping, DoP packing and PCM passthrough byte vectors\n");
    return 0;
}
