/* Copyright (C) 2004 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "mysys_priv.h"

#ifdef HAVE_LARGE_PAGES

#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif

#ifdef HAVE_SYS_SHM_H
#include <sys/shm.h>
#endif

static uint my_get_large_page_size_int(void);
static gptr my_large_malloc_int(uint size, myf my_flags);
static my_bool my_large_free_int(gptr ptr, myf my_flags);

/* Gets the size of large pages from the OS */

uint my_get_large_page_size(void)
{
  uint size;
  DBUG_ENTER("my_get_large_page_size");

  if (!(size = my_get_large_page_size_int()))
    fprintf(stderr, "Warning: Failed to determine large page size\n");

  DBUG_RETURN(size);
}

/*
  General large pages allocator.
  Tries to allocate memory from large pages pool and falls back to
  my_malloc_lock() in case of failure
*/

gptr my_large_malloc(uint size, myf my_flags)
{
  gptr ptr;
  DBUG_ENTER("my_large_malloc");

  if (my_use_large_pages && my_large_page_size)
  {
    if ((ptr = my_large_malloc_int(size, my_flags)) != NULL)
      DBUG_RETURN(ptr);
    if (my_flags & MY_WME)
      fprintf(stderr, "Warning: Using conventional memory pool\n");
  }

  DBUG_RETURN(my_malloc_lock(size, my_flags));
}

/*
  General large pages deallocator.
  Tries to deallocate memory as if it was from large pages pool and falls back
  to my_free_lock() in case of failure
 */

void my_large_free(gptr ptr, myf my_flags __attribute__((unused)))
{
  DBUG_ENTER("my_large_free");

  /*
    my_large_free_int() can only fail if ptr was not allocated with
    my_large_malloc_int(), i.e. my_malloc_lock() was used so we should free it
    with my_free_lock()
  */
  if (!my_use_large_pages || !my_large_page_size || !my_large_free_int(ptr, my_flags))
    my_free_lock(ptr, my_flags);

  DBUG_VOID_RETURN;
}

#ifdef HUGETLB_USE_PROC_MEMINFO
/* Linux-specific function to determine the size of large pages */

uint my_get_large_page_size_int(void)
{
  FILE *f;
  uint size = 0;
  char buf[256];
  DBUG_ENTER("my_get_large_page_size_int");

  if (!(f = my_fopen("/proc/meminfo", O_RDONLY, MYF(MY_WME))))
    goto finish;

  while (fgets(buf, sizeof(buf), f))
    if (sscanf(buf, "Hugepagesize: %u kB", &size))
      break;

  my_fclose(f, MYF(MY_WME));

finish:
  DBUG_RETURN(size * 1024);
}
#endif /* HUGETLB_USE_PROC_MEMINFO */

#if HAVE_DECL_SHM_HUGETLB
/* Linux-specific large pages allocator  */

gptr my_large_malloc_int(uint size, myf my_flags)
{
  int shmid;
  gptr ptr;
  struct shmid_ds buf;
  DBUG_ENTER("my_large_malloc_int");

  /* Align block size to my_large_page_size */
  size = ((size - 1) & ~(my_large_page_size - 1)) + my_large_page_size;

  shmid = shmget(IPC_PRIVATE, (size_t)size, SHM_HUGETLB | SHM_R | SHM_W);
  if (shmid < 0)
  {
    if (my_flags & MY_WME)
      fprintf(stderr,
              "Warning: Failed to allocate %d bytes from HugeTLB memory."
              " errno %d\n",
              size, errno);

    DBUG_RETURN(NULL);
  }

  ptr = shmat(shmid, NULL, 0);
  if (ptr == (void *)-1)
  {
    if (my_flags & MY_WME)
      fprintf(stderr,
              "Warning: Failed to attach shared memory segment,"
              " errno %d\n",
              errno);
    shmctl(shmid, IPC_RMID, &buf);

    DBUG_RETURN(NULL);
  }

  /*
    Remove the shared memory segment so that it will be automatically freed
    after memory is detached or process exits
  */
  shmctl(shmid, IPC_RMID, &buf);

  DBUG_RETURN(ptr);
}

/* Linux-specific large pages deallocator */

my_bool my_large_free_int(byte *ptr, myf my_flags __attribute__((unused)))
{
  DBUG_ENTER("my_large_free_int");
  DBUG_RETURN(shmdt(ptr) == 0);
}
#endif /* HAVE_DECL_SHM_HUGETLB */

#endif /* HAVE_LARGE_PAGES */
