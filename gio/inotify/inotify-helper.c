/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 2; tab-width: 8 -*- */

/* inotify-helper.c - GVFS Monitor based on inotify.

   Copyright (C) 2007 John McCutchan

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   see <http://www.gnu.org/licenses/>.

   Authors: 
		 John McCutchan <john@johnmccutchan.com>
*/

#include "config.h"
#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
/* Just include the local header to stop all the pain */
#include <sys/inotify.h>
#include <gio/glocalfilemonitor.h>
#include <gio/gfile.h>
#include "inotify-helper.h"
#include "inotify-missing.h"
#include "inotify-path.h"

static gboolean ih_debug_enabled = FALSE;
#define IH_W if (ih_debug_enabled) g_warning 

static void ih_event_callback (ik_event_t  *event,
			       inotify_sub *sub,
			       gboolean     file_event);
static void ih_not_missing_callback (inotify_sub *sub);

/* We share this lock with inotify-kernel.c and inotify-missing.c
 *
 * inotify-kernel.c takes the lock when it reads events from
 * the kernel and when it processes those events
 *
 * inotify-missing.c takes the lock when it is scanning the missing
 * list.
 *
 * We take the lock in all public functions
 */
G_LOCK_DEFINE (inotify_lock);

static GFileMonitorEvent ih_mask_to_EventFlags (guint32 mask);

/**
 * _ih_startup:
 *
 * Initializes the inotify backend.  This must be called before
 * any other functions in this module.
 *
 * Returns: #TRUE if initialization succeeded, #FALSE otherwise
 */
gboolean
_ih_startup (void)
{
  static gboolean initialized = FALSE;
  static gboolean result = FALSE;
  
  G_LOCK (inotify_lock);
  
  if (initialized == TRUE)
    {
      G_UNLOCK (inotify_lock);
      return result;
    }

  result = _ip_startup (ih_event_callback);
  if (!result)
    {
      G_UNLOCK (inotify_lock);
      return FALSE;
    }
  _im_startup (ih_not_missing_callback);

  IH_W ("started gvfs inotify backend\n");
  
  initialized = TRUE;
  
  G_UNLOCK (inotify_lock);
  
  return TRUE;
}

/*
 * Adds a subscription to be monitored.
 */
gboolean
_ih_sub_add (inotify_sub *sub)
{
  G_LOCK (inotify_lock);
	
  if (!_ip_start_watching (sub))
    _im_add (sub);
  
  G_UNLOCK (inotify_lock);

  return TRUE;
}

/*
 * Cancels a subscription which was being monitored.
 */
gboolean
_ih_sub_cancel (inotify_sub *sub)
{
  G_LOCK (inotify_lock);

  if (!sub->cancelled)
    {
      IH_W ("cancelling %s\n", sub->dirname);
      sub->cancelled = TRUE;
      _im_rm (sub);
      _ip_stop_watching (sub);
    }
  
  G_UNLOCK (inotify_lock);

  return TRUE;
}

static char *
_ih_fullpath_from_event (ik_event_t *event,
			 const char *dirname,
			 const char *filename)
{
  char *fullpath;

  if (filename)
    fullpath = g_strdup_printf ("%s/%s", dirname, filename);
  else if (event->name)
    fullpath = g_strdup_printf ("%s/%s", dirname, event->name);
  else
    fullpath = g_strdup_printf ("%s/", dirname);

   return fullpath;
}

static void
ih_event_callback (ik_event_t  *event,
                   inotify_sub *sub,
                   gboolean     file_event)
{
  g_assert (!file_event); /* XXX hardlink support */

  if (event->mask & IN_MOVE)
    {
      /* We either have a rename (in the same directory) or a move
       * (between different directories).
       */
      if (event->pair && event->pair->wd == event->wd)
        {
          /* this is a rename */
          g_file_monitor_source_handle_event (sub->user_data, G_FILE_MONITOR_EVENT_RENAMED,
                                              event->name, event->pair->name, NULL, event->timestamp);
        }
      else
        {
          GFile *other;

          if (event->pair)
            {
              const char *parent_dir;
              gchar *fullpath;

              parent_dir = _ip_get_path_for_wd (event->pair->wd);
              fullpath = _ih_fullpath_from_event (event->pair, parent_dir, NULL);
              other = g_file_new_for_path (fullpath);
              g_free (fullpath);
            }
          else
            other = NULL;

          /* this is either an incoming or outgoing move */
          g_file_monitor_source_handle_event (sub->user_data, ih_mask_to_EventFlags (event->mask),
                                              event->name, NULL, other, event->timestamp);

          if (other)
            g_object_unref (other);
        }
    }
  else
    /* unpaired event -- no 'other' field */
    g_file_monitor_source_handle_event (sub->user_data, ih_mask_to_EventFlags (event->mask),
                                        event->name, NULL, NULL, event->timestamp);
}

static void
ih_not_missing_callback (inotify_sub *sub)
{
  g_file_monitor_source_handle_event (sub->user_data, G_FILE_MONITOR_EVENT_CREATED,
                                      sub->filename, NULL, NULL, g_get_monotonic_time ());
}

/* Transforms a inotify event to a GVFS event. */
static GFileMonitorEvent
ih_mask_to_EventFlags (guint32 mask)
{
  mask &= ~IN_ISDIR;
  switch (mask)
    {
    case IN_MODIFY:
      return G_FILE_MONITOR_EVENT_CHANGED;
    case IN_CLOSE_WRITE:
      return G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT;
    case IN_ATTRIB:
      return G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED;
    case IN_MOVE_SELF:
    case IN_DELETE:
    case IN_DELETE_SELF:
      return G_FILE_MONITOR_EVENT_DELETED;
    case IN_CREATE:
      return G_FILE_MONITOR_EVENT_CREATED;
    case IN_MOVED_FROM:
      return G_FILE_MONITOR_EVENT_MOVED_OUT;
    case IN_MOVED_TO:
      return G_FILE_MONITOR_EVENT_MOVED_IN;
    case IN_UNMOUNT:
      return G_FILE_MONITOR_EVENT_UNMOUNTED;
    case IN_Q_OVERFLOW:
    case IN_OPEN:
    case IN_CLOSE_NOWRITE:
    case IN_ACCESS:
    case IN_IGNORED:
    default:
      return -1;
    }
}
