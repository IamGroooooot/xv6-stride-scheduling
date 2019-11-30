// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void freerange(void *vstart, void *vend)
{
  char *p;
  p = (char *)PGROUNDUP((uint)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(char *v)
{
  struct run *r;

  if ((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run *)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if (kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char *
kalloc(void)
{
  struct run *r;

  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  if (kmem.use_lock)
    release(&kmem.lock);
  return (char *)r;
}

/* cheolho */

typedef long Align;

union header {
  struct
  {
    union header *ptr;
    uint size;
  } s;
  Align x;
};

typedef union header Header;

Header *base_p;
char *sbrk_addr;
static Header *freep;

void k_free(void *ap)
{
  Header *bp, *p;

  bp = (Header *)ap - 1;
  for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if (p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if (bp + bp->s.size == p->s.ptr)
  {
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  }
  else
    bp->s.ptr = p->s.ptr;
  if (p + p->s.size == bp)
  {
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  }
  else
    p->s.ptr = bp;
  freep = p;
}

static Header *
kmorecore(uint nu)
{
  char *p;
  Header *hp;
  static int first = 1;

  if (first)
  {
    first = 0;
    if (nu < 4096)
      nu = 4096;
    p = sbrk_addr;
    if (p == (char *)-1)
      return 0;
    hp = (Header *)p;
    hp->s.size = nu;
    k_free((void *)(hp + 1));
    return freep;
  }
  else
  {
    return 0;
  }
}

void *
k_malloc(uint nbytes)
{
  Header *p, *prevp;
  uint nunits;

  nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
  if ((prevp = freep) == 0)
  {
    int i;
    for (i = 0; i < 9; i++)
    {
      char *temp_p = kalloc();
      if (i == 7)
        sbrk_addr = temp_p;
      if (i == 8)
        base_p = (Header *)temp_p;
    }
    base_p->s.ptr = freep = prevp = base_p;
    base_p->s.size = 0;
  }
  for (p = prevp->s.ptr;; prevp = p, p = p->s.ptr)
  {
    if (p->s.size >= nunits)
    {
      if (p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else
      {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      return (void *)(p + 1);
    }
    if (p == freep)
      if ((p = kmorecore(nunits)) == 0)
        return 0;
  }
}
