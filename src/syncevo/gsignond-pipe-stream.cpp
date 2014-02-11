/* vi: set et sw=4 ts=4 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of gsignond
 *
 * Copyright (C) 2013 Intel Corporation.
 *
 * Contact: Imran Zaman <imran.zaman@linux.intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include "gsignond-pipe-stream.h"

#define GSIGNOND_PIPE_STREAM_GET_PRIVATE(obj) \
                                          (G_TYPE_INSTANCE_GET_PRIVATE ((obj),\
                                           GSIGNOND_TYPE_PIPE_STREAM, \
                                           GSignondPipeStreamPrivate))

struct _GSignondPipeStreamPrivate
{
    GInputStream  *input_stream;
    GOutputStream *output_stream;
};

G_DEFINE_TYPE (GSignondPipeStream, gsignond_pipe_stream, G_TYPE_IO_STREAM);

static GInputStream *
_gsignond_pipe_stream_get_input_stream (GIOStream *io_stream)
{
    return GSIGNOND_PIPE_STREAM (io_stream)->priv->input_stream;
}

static GOutputStream *
_gsignond_pipe_stream_get_output_stream (GIOStream *io_stream)
{
    return GSIGNOND_PIPE_STREAM (io_stream)->priv->output_stream;
}

static void
_gsignond_pipe_stream_dispose (GObject *gobject)
{
    g_return_if_fail (GSIGNOND_IS_PIPE_STREAM (gobject));

    /* Chain up to the parent class */
    G_OBJECT_CLASS (gsignond_pipe_stream_parent_class)->dispose (gobject);

}

static void
_gsignond_pipe_stream_finalize (GObject *gobject)
{
    GSignondPipeStream *stream = GSIGNOND_PIPE_STREAM (gobject);

    /* g_io_stream needs streams to be valid in its dispose still
     */
    if (stream->priv->input_stream) {
        g_object_unref (stream->priv->input_stream);
        stream->priv->input_stream = NULL;
    }

    if (stream->priv->output_stream) {
        g_object_unref (stream->priv->output_stream);
        stream->priv->output_stream = NULL;
    }

    G_OBJECT_CLASS (gsignond_pipe_stream_parent_class)->finalize (gobject);
}

static void
gsignond_pipe_stream_class_init (GSignondPipeStreamClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GIOStreamClass *stream_class = G_IO_STREAM_CLASS (klass);

    gobject_class->finalize = _gsignond_pipe_stream_finalize;
    gobject_class->dispose = _gsignond_pipe_stream_dispose;

    /* virtual methods */
    stream_class->get_input_stream = _gsignond_pipe_stream_get_input_stream;
    stream_class->get_output_stream = _gsignond_pipe_stream_get_output_stream;

    g_type_class_add_private (klass, sizeof (GSignondPipeStreamPrivate));
}

static void
gsignond_pipe_stream_init (GSignondPipeStream *self)
{
    self->priv = GSIGNOND_PIPE_STREAM_GET_PRIVATE (self);
    self->priv->input_stream = NULL;
    self->priv->output_stream = NULL;
}

GSignondPipeStream *
gsignond_pipe_stream_new (
        gint in_fd,
        gint out_fd,
        gboolean close_fds)
{
    GSignondPipeStream *stream = GSIGNOND_PIPE_STREAM (g_object_new (
            GSIGNOND_TYPE_PIPE_STREAM, NULL));
    if (stream) {
        stream->priv->input_stream = G_INPUT_STREAM (
                g_unix_input_stream_new (in_fd, close_fds));
        stream->priv->output_stream = G_OUTPUT_STREAM (
                g_unix_output_stream_new (out_fd, close_fds));
    }
    return stream;
}


