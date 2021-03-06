/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendtest.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobwrite.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobqueryinfowrite.h"

G_DEFINE_TYPE (GVfsBackendTest, g_vfs_backend_test, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_test_finalize (GObject *object)
{
  if (G_OBJECT_CLASS (g_vfs_backend_test_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_test_parent_class)->finalize) (object);
}

static void
g_vfs_backend_test_init (GVfsBackendTest *test_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (test_backend);
  GMountSpec *mount_spec;

  g_vfs_backend_set_display_name (backend, "test");

  mount_spec = g_mount_spec_new ("test");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);
}


static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source,
	   gboolean is_automount)
{
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static gboolean 
open_idle_cb (gpointer data)
{
  GVfsJobOpenForRead *job = data;
  int fd;

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_CANCELLED,
			_("Operation was cancelled"));
      return FALSE;
    }
  
  fd = g_open (job->filename, O_RDONLY);
  if (fd == -1)
    {
      int errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			g_io_error_from_errno (errsv),
			"Error opening file %s: %s",
			job->filename, g_strerror (errsv));
    }
  else
    {
      g_vfs_job_open_for_read_set_can_seek (job, TRUE);
      g_vfs_job_open_for_read_set_handle (job, GINT_TO_POINTER (fd));
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  return FALSE;
}

static void
open_read_cancelled_cb (GVfsJob *job, gpointer data)
{
  guint tag = GPOINTER_TO_INT (data);

  g_print ("open_read_cancelled_cb\n");
  
  if (g_source_remove (tag))
    g_vfs_job_failed (job, G_IO_ERROR,
		      G_IO_ERROR_CANCELLED,
		      _("Operation was cancelled"));
}

static gboolean 
try_open_for_read (GVfsBackend *backend,
		   GVfsJobOpenForRead *job,
		   const char *filename)
{
  GError *error;

  g_print ("try_open_for_read (%s)\n", filename);
  
  if (strcmp (filename, "/fail") == 0)
    {
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "Test error");
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    }
  else
    {
      guint tag = g_timeout_add (0, open_idle_cb, job);
      g_signal_connect (job, "cancelled", (GCallback)open_read_cancelled_cb, GINT_TO_POINTER (tag));
    }
  
  return TRUE;
}

static gboolean 
read_idle_cb (gpointer data)
{
  GVfsJobRead *job = data;
  int fd;
  ssize_t res;

  fd = GPOINTER_TO_INT (job->handle);

  res = read (fd, job->buffer, job->bytes_requested);

  if (res == -1)
    {
      int errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			g_io_error_from_errno (errsv),
			"Error reading from file: %s",
			g_strerror (errsv));
    }
  else
    {
      g_vfs_job_read_set_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
  return FALSE;
}

static void
read_cancelled_cb (GVfsJob *job, gpointer data)
{
  guint tag = GPOINTER_TO_INT (job->backend_data);

  g_source_remove (tag);
  g_vfs_job_failed (job, G_IO_ERROR,
		    G_IO_ERROR_CANCELLED,
		    _("Operation was cancelled"));
}

static gboolean
try_read (GVfsBackend *backend,
	  GVfsJobRead *job,
	  GVfsBackendHandle handle,
	  char *buffer,
	  gsize bytes_requested)
{
  guint tag;

  g_print ("read (%"G_GSIZE_FORMAT")\n", bytes_requested);

  tag = g_timeout_add (0, read_idle_cb, job);
  G_VFS_JOB (job)->backend_data = GINT_TO_POINTER (tag);
  g_signal_connect (job, "cancelled", (GCallback)read_cancelled_cb, NULL);
  
  return TRUE;
}

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  int whence;
  int fd;
  off_t final_offset;

  g_print ("seek_on_read (%d, %u)\n", (int)offset, type);

  switch (type)
    {
    default:
    case G_SEEK_SET:
      whence = SEEK_SET;
      break;
    case G_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case G_SEEK_END:
      whence = SEEK_END;
      break;
    }
      
  
  fd = GPOINTER_TO_INT (handle);

  final_offset = lseek (fd, offset, whence);
  
  if (final_offset == (off_t)-1)
    {
      int errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			g_io_error_from_errno (errsv),
			"Error seeking in file: %s",
			g_strerror (errsv));
    }
  else
    {
      g_vfs_job_seek_read_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_query_info_on_read (GVfsBackend *backend,
		       GVfsJobQueryInfoRead *job,
		       GVfsBackendHandle handle,
		       GFileInfo *info,
		       GFileAttributeMatcher *attribute_matcher)
{
  int fd, res;
  struct stat statbuf;
    
  fd = GPOINTER_TO_INT (handle);

  res = fstat (fd, &statbuf);

  if (res == -1)
    {
      int errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			g_io_error_from_errno (errsv),
			"Error querying info in file: %s",
			g_strerror (errsv));
    }
  else
    {
      g_file_info_set_size (info, statbuf.st_size);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE,
					statbuf.st_dev);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, statbuf.st_mtime);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, statbuf.st_atime);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED, statbuf.st_ctime);

      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  int fd;

  g_print ("close ()\n");

  fd = GPOINTER_TO_INT (handle);
  close(fd);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GFile *file;
  GFileInfo *info2;
  GError *error;
  GVfs *local_vfs;

  g_print ("do_get_file_info (%s)\n", filename);
  
  local_vfs = g_vfs_get_local ();
  file = g_vfs_get_file_for_path (local_vfs, filename);

  error = NULL;
  info2 = g_file_query_info (file, job->attributes, flags,
			     NULL, &error);

  if (info2)
    {
      g_file_info_copy_into (info2, info);
      g_object_unref (info2);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);

  g_object_unref (file);
}

static void
do_create (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename,
	   GFileCreateFlags flags)
{
  GFile *file;
  GFileOutputStream *out;
  GError *error;
  
  file = g_vfs_get_file_for_path (g_vfs_get_local (),
				  filename);

  error = NULL;
  out = g_file_create (file, flags, G_VFS_JOB (job)->cancellable, &error);
  g_object_unref (file);
  if (out)
    {
      g_vfs_job_open_for_write_set_can_seek (job, FALSE);
      g_vfs_job_open_for_write_set_handle (job, out);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
do_append_to (GVfsBackend *backend,
	      GVfsJobOpenForWrite *job,
	      const char *filename,
	      GFileCreateFlags flags)
{
  GFile *file;
  GFileOutputStream *out;
  GError *error;
  
  file = g_vfs_get_file_for_path (g_vfs_get_local (),
				  filename);

  error = NULL;
  out = g_file_append_to (file, flags, G_VFS_JOB (job)->cancellable, &error);
  g_object_unref (file);
  if (out)
    {
      g_vfs_job_open_for_write_set_can_seek (job, FALSE);
      g_vfs_job_open_for_write_set_handle (job, out);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
do_replace (GVfsBackend *backend,
	    GVfsJobOpenForWrite *job,
	    const char *filename,
	    const char *etag,
	    gboolean make_backup,
	    GFileCreateFlags flags)
{
  GFile *file;
  GFileOutputStream *out;
  GError *error;
  
  file = g_vfs_get_file_for_path (g_vfs_get_local (),
				  filename);

  error = NULL;
  out = g_file_replace (file,
			etag, make_backup,
			flags, G_VFS_JOB (job)->cancellable,
			&error);
  g_object_unref (file);
  if (out)
    {
      g_vfs_job_open_for_write_set_can_seek (job, FALSE);
      g_vfs_job_open_for_write_set_handle (job, out);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
do_close_write (GVfsBackend *backend,
		GVfsJobCloseWrite *job,
		GVfsBackendHandle handle)
{
  GFileOutputStream *out;
  GError *error;
  char *etag;

  out = (GFileOutputStream *)handle;

  error = NULL;
  if (!g_output_stream_close (G_OUTPUT_STREAM (out),
			      G_VFS_JOB (job)->cancellable,
			      &error))
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      etag = g_file_output_stream_get_etag (out);

      if (etag)
	{
	  g_vfs_job_close_write_set_etag (job, etag);
	  g_free (etag);
	}
      
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
  g_object_unref (out);
}

static void
do_write (GVfsBackend *backend,
	  GVfsJobWrite *job,
	  GVfsBackendHandle handle,
	  char *buffer,
	  gsize buffer_size)
{
  GFileOutputStream *out;
  GError *error;
  gssize res;

  g_print ("do_write\n");
  
  out = (GFileOutputStream *)handle;

  error = NULL;
  res = g_output_stream_write (G_OUTPUT_STREAM (out),
			       buffer, buffer_size,
			       G_VFS_JOB (job)->cancellable,
			       &error);
  if (res < 0)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      g_vfs_job_write_set_written_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_query_info_on_write (GVfsBackend *backend,
			GVfsJobQueryInfoWrite *job,
			GVfsBackendHandle handle,
			GFileInfo *info,
			GFileAttributeMatcher *attribute_matcher)
{
  GFileOutputStream *out;
  GError *error;
  GFileInfo *info2;

  g_print ("do_query_info_on_write\n");
  
  out = (GFileOutputStream *)handle;
  
  error = NULL;
  info2 = g_file_output_stream_query_info (out, job->attributes,
					  G_VFS_JOB (job)->cancellable,
					  &error);
  if (info2 == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      g_file_info_copy_into (info2, info);
      g_object_unref (info2);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static gboolean
try_enumerate (GVfsBackend *backend,
	       GVfsJobEnumerate *job,
	       const char *filename,
	       GFileAttributeMatcher *matcher,
	       GFileQueryInfoFlags flags)
{
  GFileInfo *info1, *info2;
  GList *l;

  g_print ("try_enumerate (%s)\n", filename);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  info1 = g_file_info_new ();
  info2 = g_file_info_new ();
  g_file_info_set_name (info1, "file1");
  g_file_info_set_file_type (info1, G_FILE_TYPE_REGULAR);
  g_file_info_set_name (info2, "file2");
  g_file_info_set_file_type (info2, G_FILE_TYPE_REGULAR);
  
  l = NULL;
  l = g_list_append (l, info1);
  l = g_list_append (l, info2);

  g_vfs_job_enumerate_add_infos (job, l);

  g_list_free (l);
  g_object_unref (info1);
  g_object_unref (info2);

  g_vfs_job_enumerate_done (job);
  
  return TRUE;
}

static void
g_vfs_backend_test_class_init (GVfsBackendTestClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_test_finalize;

  backend_class->try_mount = try_mount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read = try_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->query_info_on_read = do_query_info_on_read;
  backend_class->close_read = do_close_read;

  backend_class->replace = do_replace;
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->write = do_write;
  backend_class->query_info_on_write = do_query_info_on_write;
  backend_class->close_write = do_close_write;
  
  backend_class->query_info = do_query_info;
  backend_class->try_enumerate = try_enumerate;
}
