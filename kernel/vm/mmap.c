#include "globals.h"
#include "errno.h"
#include "types.h"

#include "mm/mm.h"
#include "mm/tlb.h"
#include "mm/mman.h"
#include "mm/page.h"

#include "proc/proc.h"

#include "util/string.h"
#include "util/debug.h"

#include "fs/vnode.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/stat.h"

#include "vm/vmmap.h"
#include "vm/mmap.h"

/*
 * This function implements the mmap(2) syscall, but only
 * supports the MAP_SHARED, MAP_PRIVATE, MAP_FIXED, and
 * MAP_ANON flags.
 *
 * Add a mapping to the current process's address space.
 * You need to do some error checking; see the ERRORS section
 * of the manpage for the problems you should anticipate.
 * After error checking most of the work of this function is
 * done by vmmap_map(), but remember to clear the TLB.
 */
int do_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off,
            void **ret) {
  dbg(DBG_VM, "addr: %p\n", addr);
  if (!PAGE_ALIGNED(addr) || !PAGE_ALIGNED(len) || !PAGE_ALIGNED(off) || !len) {
    dbg(DBG_VM, "error\n");
    return -EINVAL;
  }
  if (addr && (addr < (void *)USER_MEM_LOW || addr >= (void *)USER_MEM_HIGH)) {
    dbg(DBG_VM, "error\n");
    return -EINVAL;
  }
  if (((flags & MAP_PRIVATE) && (flags & MAP_SHARED)) || 
      (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED))) {
    dbg(DBG_VM, "error\n");
    return -EINVAL;
  }
  // Set up file
  file_t *file;
  if (flags & MAP_ANON) {
    file = NULL;
  } else {
    file = fget(fd);
    if (!file) {
      dbg(DBG_VM, "error\n");
      return -EBADF;
    }
    //if (!S_ISREG(file->f_vnode->vn_mode)) {
    //  dbg(DBG_VM, "error\n");
    //  fput(file);
    //  return -EACCES;
    //}
    if (!(file->f_mode & FMODE_READ) ||
        ((flags & MAP_SHARED) && (prot & PROT_WRITE) && 
         !(file->f_mode & FMODE_WRITE)) ||
        ((prot & PROT_WRITE) && (file->f_mode == FMODE_APPEND))) {
      dbg(DBG_VM, "error\n");
      fput(file);
      return -EACCES;
    }
  }

  vmarea_t *new_area;
  int status = vmmap_map(curproc->p_vmmap, file ? file->f_vnode : NULL, 
      ADDR_TO_PN(addr), len / PAGE_SIZE, prot, 
      flags, off, VMMAP_DIR_HILO, &new_area);
  if (new_area)
    tlb_flush_range(new_area->vma_start, len / PAGE_SIZE);
  if (!status) *ret = (void *)new_area->vma_start;
  return status;
}

/*
 * This function implements the munmap(2) syscall.
 *
 * As with do_mmap() it should perform the required error checking,
 * before calling upon vmmap_remove() to do most of the work.
 * Remember to clear the TLB.
 */
int do_munmap(void *addr, size_t len) {
  if (!PAGE_ALIGNED(addr) || !PAGE_ALIGNED(len) || !len)
      return -EINVAL;
  if (addr < (void *)USER_MEM_LOW || addr >= (void *)USER_MEM_HIGH)
    return -EINVAL;
  int status = vmmap_remove(curproc->p_vmmap, (uint32_t)addr, len / PAGE_SIZE);
  tlb_flush_range((uint32_t)addr, len / PAGE_SIZE);
  return status;
}
