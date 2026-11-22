/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GM_DSD_OUTPUT_H
#define GM_DSD_OUTPUT_H
#include <gst/gst.h>

/* Takes ownership of the sink; output is a bin with a ghost sink pad. */
GstElement *gm_dsd_sink_new(GstElement *sink, gboolean dop);
/* Register the private converter (also used by tests). */
gboolean gm_dsd_register(void);
#endif
