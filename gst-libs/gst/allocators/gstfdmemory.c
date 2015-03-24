/* GStreamer fd backed memory
 * Copyright (C) 2013 Linaro SA
 * Author: Benjamin Gaignard <benjamin.gaignard@linaro.org> for Linaro.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for mordetails.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstfdmemory.h"

#ifdef HAVE_MMAP
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct
{
  GstMemory mem;

  GstFdMemoryFlags flags;
  gint fd;
  gpointer data;
  gint mmapping_flags;
  gint mmap_count;
  GMutex lock;
} GstFdMemory;

static void
gst_fd_mem_free (GstAllocator * allocator, GstMemory * gmem)
{
#ifdef HAVE_MMAP
  GstFdMemory *mem = (GstFdMemory *) gmem;

  if (mem->data) {
    if (!(mem->flags & GST_FD_MEMORY_FLAG_KEEP_MAPPED))
      g_warning (G_STRLOC ":%s: Freeing memory %p still mapped", G_STRFUNC,
          mem);

    munmap ((void *) mem->data, gmem->maxsize);
  }
  if (mem->fd >= 0 && gmem->parent == NULL)
    close (mem->fd);
  g_mutex_clear (&mem->lock);
  g_slice_free (GstFdMemory, mem);
  GST_DEBUG ("%p: freed", mem);
#endif
}

static gpointer
gst_fd_mem_map (GstMemory * gmem, gsize maxsize, GstMapFlags flags)
{
#ifdef HAVE_MMAP
  GstFdMemory *mem = (GstFdMemory *) gmem;
  gint prot;
  gpointer ret = NULL;

  if (gmem->parent)
    return gst_fd_mem_map (gmem->parent, maxsize, flags);

  prot = flags & GST_MAP_READ ? PROT_READ : 0;
  prot |= flags & GST_MAP_WRITE ? PROT_WRITE : 0;

  g_mutex_lock (&mem->lock);
  /* do not mmap twice the buffer */
  if (mem->data) {
    /* only return address if mapping flags are a subset
     * of the previous flags */
    if ((mem->mmapping_flags & prot) == prot) {
      ret = mem->data;
      mem->mmap_count++;
    }

    goto out;
  }

  if (mem->fd != -1) {
    gint flags;

    flags =
        (mem->flags & GST_FD_MEMORY_FLAG_MAP_PRIVATE) ? MAP_PRIVATE :
        MAP_SHARED;

    mem->data = mmap (0, gmem->maxsize, prot, flags, mem->fd, 0);
    if (mem->data == MAP_FAILED) {
      mem->data = NULL;
      GST_ERROR ("%p: fd %d: mmap failed: %s", mem, mem->fd,
          g_strerror (errno));
      goto out;
    }
  }

  GST_DEBUG ("%p: fd %d: mapped %p", mem, mem->fd, mem->data);

  if (mem->data) {
    mem->mmapping_flags = prot;
    mem->mmap_count++;
    ret = mem->data;
  }

out:
  g_mutex_unlock (&mem->lock);
  return ret;
#else /* !HAVE_MMAP */
  return FALSE;
#endif
}

static void
gst_fd_mem_unmap (GstMemory * gmem)
{
#ifdef HAVE_MMAP
  GstFdMemory *mem = (GstFdMemory *) gmem;

  if (gmem->parent)
    return gst_fd_mem_unmap (gmem->parent);

  if (mem->flags & GST_FD_MEMORY_FLAG_KEEP_MAPPED)
    return;

  g_mutex_lock (&mem->lock);
  if (mem->data && !(--mem->mmap_count)) {
    munmap ((void *) mem->data, gmem->maxsize);
    mem->data = NULL;
    mem->mmapping_flags = 0;
    GST_DEBUG ("%p: fd %d unmapped", mem, mem->fd);
  }
  g_mutex_unlock (&mem->lock);
#endif
}

static GstMemory *
gst_fd_mem_share (GstMemory * gmem, gssize offset, gssize size)
{
#ifdef HAVE_MMAP
  GstFdMemory *mem = (GstFdMemory *) gmem;
  GstFdMemory *sub;
  GstMemory *parent;

  GST_DEBUG ("%p: share %" G_GSSIZE_FORMAT " %" G_GSIZE_FORMAT, mem, offset,
      size);

  /* find the real parent */
  if ((parent = mem->mem.parent) == NULL)
    parent = (GstMemory *) mem;

  if (size == -1)
    size = gmem->maxsize - offset;

  sub = g_slice_new0 (GstFdMemory);
  /* the shared memory is always readonly */
  gst_memory_init (GST_MEMORY_CAST (sub), GST_MINI_OBJECT_FLAGS (parent) |
      GST_MINI_OBJECT_FLAG_LOCK_READONLY, mem->mem.allocator, parent,
      mem->mem.maxsize, mem->mem.align, mem->mem.offset + offset, size);

  sub->fd = mem->fd;
  g_mutex_init (&sub->lock);

  return GST_MEMORY_CAST (sub);
#else /* !HAVE_MMAP */
  return NULL;
#endif
}

static GstMemory *
gst_fd_allocator_alloc (GstFdAllocator * allocator, gint fd, gsize size,
    GstFdMemoryFlags flags)
{
#ifdef HAVE_MMAP
  GstFdMemory *mem;

  mem = g_slice_new0 (GstFdMemory);
  gst_memory_init (GST_MEMORY_CAST (mem), 0, GST_ALLOCATOR_CAST (allocator),
      NULL, size, 0, 0, size);

  mem->flags = flags;
  mem->fd = fd;
  g_mutex_init (&mem->lock);

  GST_DEBUG ("%p: fd: %d size %" G_GSIZE_FORMAT, mem, mem->fd,
      mem->mem.maxsize);

  return (GstMemory *) mem;
#else /* !HAVE_MMAP */
  return NULL;
#endif
}

G_DEFINE_ABSTRACT_TYPE (GstFdAllocator, gst_fd_allocator, GST_TYPE_ALLOCATOR);

static void
gst_fd_allocator_class_init (GstFdAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class;

  allocator_class = (GstAllocatorClass *) klass;

  allocator_class->alloc = NULL;
  allocator_class->free = gst_fd_mem_free;

  klass->alloc = gst_fd_allocator_alloc;
}

static void
gst_fd_allocator_init (GstFdAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_map = gst_fd_mem_map;
  alloc->mem_unmap = gst_fd_mem_unmap;
  alloc->mem_share = gst_fd_mem_share;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

/**
 * gst_is_fd_memory:
 * @mem: #GstMemory
 *
 * Check if @mem is memory backed by an fd
 *
 * Returns: %TRUE when @mem has an fd that can be retrieved with
 * gst_fd_memory_get_fd().
 *
 * Since: 1.6
 */
gboolean
gst_is_fd_memory (GstMemory * mem)
{
  g_return_val_if_fail (mem != NULL, FALSE);

  return GST_IS_FD_ALLOCATOR (mem->allocator);
}

/**
 * gst_fd_memory_get_fd:
 * @mem: #GstMemory
 *
 * Get the fd from @mem. Call gst_is_fd_memory() to check if @mem has
 * an fd.
 *
 * Returns: the fd of @mem or -1 when there is no fd on @mem
 *
 * Since: 1.6
 */
gint
gst_fd_memory_get_fd (GstMemory * mem)
{
  g_return_val_if_fail (mem != NULL, -1);
  g_return_val_if_fail (GST_IS_FD_ALLOCATOR (mem->allocator), -1);

  return ((GstFdMemory *) mem)->fd;
}
