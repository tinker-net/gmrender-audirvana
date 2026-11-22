/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/dsd_output.h"
#include <gst/app/gstappsink.h>
#include <string.h>

static void test_dff(const char *uri, gboolean dop) {
    GstElement *player = gst_element_factory_make("playbin",NULL);
    GstElement *sink = gst_element_factory_make("appsink",NULL);
    g_assert_nonnull(player); g_assert_nonnull(sink);
    GstCaps *caps = gst_caps_from_string(dop ?
        "audio/x-raw,format=S32LE,layout=interleaved,rate=176400,channels=2" :
        "audio/x-dsd,format=DSDU32BE,layout=interleaved,reversed-bytes=false,rate=352800,channels=2");
    gst_app_sink_set_caps(GST_APP_SINK(sink),caps);
    gst_caps_unref(caps);
    g_object_set(sink,"sync",FALSE,NULL);
    GstElement *bin = gm_dsd_sink_new(sink,dop);
    g_assert_nonnull(bin);
    gst_object_ref_sink(bin);
    g_object_set(player,"flags",0x02 | 0x20,"audio-sink",bin,"uri",uri,NULL);
    gst_object_unref(bin);
    g_assert_cmpint(gst_element_set_state(player,GST_STATE_PLAYING),!=,GST_STATE_CHANGE_FAILURE);
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),10*GST_SECOND);
    if (!sample) {
        GstBus *bus = gst_element_get_bus(player);
        GstMessage *msg = gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR);
        if (msg) {
            GError *error = NULL; gchar *debug = NULL;
            gst_message_parse_error(msg,&error,&debug);
            g_printerr("DFF pipeline: %s (%s)\n",error->message,debug ? debug : "");
            g_error_free(error); g_free(debug); gst_message_unref(msg);
        }
        gst_object_unref(bus);
        g_error("No DFF output sample");
    }
    const guint8 be[] = {0x10,0x11,0x12,0x13,0x20,0x21,0x22,0x23};
    const guint8 dp[] = {0,0x11,0x10,0x05,0,0x21,0x20,0x05,0,0x13,0x12,0xfa,0,0x23,0x22,0xfa};
    const guint8 *expected = dop ? dp : be;
    gsize count = dop ? sizeof(dp) : sizeof(be);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(gst_sample_get_buffer(sample),&map,GST_MAP_READ));
    g_assert_cmpuint(map.size,>=,count);
    g_assert_cmpmem(map.data,count,expected,count);
    gst_buffer_unmap(gst_sample_get_buffer(sample),&map);
    gst_sample_unref(sample);
    gst_element_set_state(player,GST_STATE_NULL);
    gst_object_unref(player);
}
int main(int argc,char **argv) {
    gst_init(&argc,&argv);
    g_assert_cmpint(argc,==,2);
    GstElementFactory *parser = gst_element_factory_find("dffparse");
    g_assert_nonnull(parser);
    gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(parser),GST_RANK_PRIMARY+100);
    gst_object_unref(parser);
    test_dff(argv[1],FALSE);
    test_dff(argv[1],TRUE);
    g_print("PASS: DFF via playbin into native DSD and DoP simulated sinks\n");
    return 0;
}
