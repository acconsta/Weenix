/*
 *  FILE: open.c
 *  AUTH: mcc | jal
 *  DESC:
 *  DATE: Mon Apr  6 19:27:49 1998
 */

#include "globals.h"
#include "errno.h"
#include "fs/fcntl.h"
#include "util/string.h"
#include "util/printf.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/file.h"
#include "fs/vfs_syscall.h"
#include "fs/open.h"
#include "fs/stat.h"
#include "util/debug.h"

/* find empty index in p->p_files[] */
int get_empty_fd(proc_t *p) {
  int fd;

  for (fd = 0; fd < NFILES; fd++) {
    if (!p->p_files[fd])
      return fd;
  }

  dbg(DBG_ERROR | DBG_VFS, "ERROR: get_empty_fd: out of file descriptors "
                           "for pid %d\n",
      curproc->p_pid);
  return -EMFILE;
}

/*
 * There a number of steps to opening a file:
 *      1. Get the next empty file descriptor.
 *      2. Call fget to get a fresh file_t.
 *      3. Save the file_t in curproc's file descriptor table.
 *      4. Set file_t->f_mode to OR of FMODE_(READ|WRITE|APPEND) based on
 *         oflags, which can be O_RDONLY, O_WRONLY or O_RDWR, possibly OR'd with
 *         O_APPEND.
 *      5. Use open_namev() to get the vnode for the file_t.
 *      6. Fill in the fields of the file_t.
 *      7. Return new fd.
 *
 * If anything goes wrong at any point (specifically if the call to open_namev
 * fails), be sure to remove the fd from curproc, fput the file_t and return an
 * error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        oflags is not valid.
 *      o EMFILE
 *        The process already has the maximum number of files open.
 *      o ENOMEM
 *        Insufficient kernel memory was available.
 *      o ENAMETOOLONG
 *        A component of filename was too long.
 *      o ENOENT
 *        O_CREAT is not set and the named file does not exist.  Or, a
 *        directory component in pathname does not exist.
 *      o EISDIR
 *        pathname refers to a directory and the access requested involved
 *        writing (that is, O_WRONLY or O_RDWR is set).
 *      o ENXIO
 *        pathname refers to a device special file and no corresponding device
 *        exists.
 */

int do_open(const char *filename, int oflags) {
  dbg(DBG_VFS, "opening %s flags: 0x%x\n", filename, oflags);
  // Get new fd and empty f struct
  int new_fd = get_empty_fd(curproc);
  if (new_fd == -EMFILE)
    return -EMFILE;
  file_t *f = fget(-1);
  if (!f) return -ENOMEM;
  curproc->p_files[new_fd] = f;

  // Set file mode
  if ((oflags & 3) == O_RDONLY) {
    f->f_mode = FMODE_READ; 
  } else if ((oflags & 3) == O_WRONLY) {
    f->f_mode = FMODE_WRITE;
  } else if ((oflags & 3) == O_RDWR) {
    f->f_mode = FMODE_WRITE | FMODE_READ;
  } else {
    dbg(DBG_VFS, "invalid flags\n");
    do_close(new_fd);
    return -EINVAL;
  }
  if (oflags & O_APPEND)
    f->f_mode |= FMODE_APPEND;

  vnode_t *result;
  int status = open_namev(filename, oflags, &result, NULL);
  if (status) {
    dbg(DBG_VFS, "couldn't open path\n");
    do_close(new_fd);
    curproc->p_files[new_fd] = NULL;
    return status;
  }
  KASSERT(result);
  if (S_ISDIR(result->vn_mode) && (f->f_mode & FMODE_WRITE)) {
    dbg(DBG_VFS, "error: tried to open directory\n");
    do_close(new_fd);
    vput(result);
    return -EISDIR;
  }
  f->f_vnode = result;
  return new_fd;
}

