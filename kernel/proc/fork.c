#include "types.h"
#include "globals.h"
#include "errno.h"

#include "util/debug.h"
#include "util/string.h"

#include "proc/proc.h"
#include "proc/kthread.h"

#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/page.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "mm/pagetable.h"
#include "mm/tlb.h"

#include "fs/file.h"
#include "fs/vnode.h"

#include "vm/shadow.h"
#include "vm/vmmap.h"

#include "api/exec.h"

#include "main/interrupt.h"

/* Pushes the appropriate things onto the kernel stack of a newly forked thread
 * so that it can begin execution in userland_entry.
 * regs: registers the new thread should have on execution
 * kstack: location of the new thread's kernel stack
 * Returns the new stack pointer on success. */
static uint32_t fork_setup_stack(const regs_t *regs, void *kstack) {
  /* Pointer argument and dummy return address, and userland dummy return
   * address */
  uint32_t esp =
      ((uint32_t)kstack) + DEFAULT_STACK_SIZE - (sizeof(regs_t) + 12);
  *(void **)(esp + 4) = (void *)(esp + 8); /* Set the argument to point to
                                              location of struct on stack */
  memcpy((void *)(esp + 8), regs, sizeof(regs_t)); /* Copy over struct */
  return esp;
}

/*
 * The implementation of fork(2). Once this works,
 * you're practically home free. This is what the
 * entirety of Weenix has been leading up to.
 * Go forth and conquer.
 */
int do_fork(struct regs *regs) {
  // Set up new proc
  proc_t *new_proc = proc_create("");
  KASSERT(new_proc);
  new_proc->p_vmmap = vmmap_clone(curproc->p_vmmap);
  memcpy(&new_proc->p_comm, curproc->p_comm, PROC_NAME_LEN);
  new_proc->p_status = curproc->p_status;
  new_proc->p_state = curproc->p_state;
  new_proc->p_pagedir = pt_create_pagedir();
  KASSERT(new_proc->p_pagedir);
  new_proc->p_brk = curproc->p_brk;
  new_proc->p_start_brk = curproc->p_start_brk;
  new_proc->p_cwd = curproc->p_cwd;

  // Increment refcounts
  memcpy(&new_proc->p_files, &curproc->p_files, sizeof(file_t)*NFILES);
  for (int i = 0; i < NFILES; ++i) fref(new_proc->p_files[i]);
  vref(curproc->p_cwd);

  // Set up shadow objects for private objects
  vmarea_t *vma;
  vmarea_t *vma2;
  list_link_t *link;
  list_link_t *link2;
  for (link = curproc->p_vmmap->vmm_list.l_next, 
      link2 = new_proc->p_vmmap->vmm_list.l_prev; 
      link != curproc->p_vmmap->vmm_list.l_next &&
      link2 != new_proc->p_vmmap->vmm_list.l_next; 
      link = link->l_next, link2 = link2->l_next) {
    vma = list_item(link, vmarea_t, vma_plink);
    vma2 = list_item(link2, vmarea_t, vma_plink);
    if (vma->vma_flags & MAP_PRIVATE) {
      mmobj_t *shadow = shadow_create();
      mmobj_t *shadow2 = shadow_create();
      shadow->mmo_shadowed = vma->vma_obj;
      shadow2->mmo_shadowed = vma->vma_obj;
      vma->vma_obj = shadow;
      vma2->vma_obj = shadow2;
    }
  }
  
  // Unmap pages and flush caches
  pt_unmap_range(curproc->p_pagedir, USER_MEM_LOW, USER_MEM_HIGH);
  tlb_flush_all();

  // Set up new thread context
  kthread_t *new_thr = kthread_clone(curthr); 
  KASSERT(new_thr);
  list_insert_tail(&new_proc->p_threads, &new_thr->kt_plink);
  new_thr->kt_proc = new_proc;

  new_thr->kt_ctx.c_eip = (uint32_t)&userland_entry;
  new_thr->kt_ctx.c_esp = fork_setup_stack(regs, new_thr->kt_kstack);
  new_thr->kt_ctx.c_pdptr = new_proc->p_pagedir;

  new_thr->kt_ctx.c_kstack = (uintptr_t)new_thr->kt_kstack + DEFAULT_STACK_SIZE;
  new_thr->kt_ctx.c_kstacksz = DEFAULT_STACK_SIZE;

  sched_make_runnable(new_thr);

  return 0;
}
