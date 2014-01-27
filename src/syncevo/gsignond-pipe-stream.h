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

#ifndef __GSIGNOND_PIPE_STREAM_H__
#define __GSIGNOND_PIPE_STREAM_H__

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * Type macros.
 */
#define GSIGNOND_TYPE_PIPE_STREAM   (gsignond_pipe_stream_get_type ())
#define GSIGNOND_PIPE_STREAM(obj)   (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                           GSIGNOND_TYPE_PIPE_STREAM, \
                                           GSignondPipeStream))
#define GSIGNOND_IS_PIPE_STREAM(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
                                           GSIGNOND_TYPE_PIPE_STREAM))
#define GSIGNOND_PIPE_STREAM_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), \
                                             GSIGNOND_TYPE_PIPE_STREAM, \
                                             GSignondPipeStreamClass))
#define GSIGNOND_IS_PIPE_STREAM_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),\
                                             GSIGNOND_TYPE_PIPE_STREAM))
#define GSIGNOND_PIPE_STREAM_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), \
                                             GSIGNOND_TYPE_PIPE_STREAM, \
                                             GSignondPipeStreamClass))

typedef struct _GSignondPipeStreamPrivate GSignondPipeStreamPrivate;

typedef struct {
    GIOStream parent_instance;

    /*< private >*/
    GSignondPipeStreamPrivate *priv;
} GSignondPipeStream;

typedef struct {
    GIOStreamClass parent_class;

} GSignondPipeStreamClass;

/* used by GSIGNOND_TYPE_PIPE_STREAM */
GType
gsignond_pipe_stream_get_type (void);

GSignondPipeStream *
gsignond_pipe_stream_new (
        gint in_fd,
        gint out_fd,
        gboolean close_fds);

G_END_DECLS

#endif /* __GSIGNOND_PIPE_STREAM_H__ */
