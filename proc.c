#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define STRIDE_LARGE_NUMBER 10000

struct proc proc;

struct
{
  struct spinlock lock;
  struct list_head queue_head;
  /* stride scheduling */
  int large_number;         // a large number required for stride scheduling
  long long min_pass_value; // system-wide lowest pass value
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

/* stride scheduling */

/* Remove and return a process that is RUNNABLE and has the lowest pass value from the queue.
   If there is no such a process, the function returns NULL.
   This function is called from scheduler().
*/
struct proc *remove_min(struct list_head *head)
{
  struct proc *targetProcess = NULL;
  struct proc *minProcess = NULL;
  struct list_head *item = NULL;

  // iterate all list
  list_for_each(item, head)
  {
    // get process
    targetProcess = list_entry(item, struct proc, queue_elem);

    // when not runnable skip
    if (targetProcess->state != RUNNABLE)
      continue;
    
    // find min
    if(minProcess == NULL) // just store at first
    {
      minProcess = targetProcess;
    }
    else
    { // if minProcess is larger store smaller one
      if(minProcess->stride_info.pass_value > targetProcess->stride_info.pass_value)
      {
        minProcess = targetProcess;
      }
    }
  }
  
  // if no process found
  if(minProcess == NULL)
  {
    return NULL;
  }
  else
  {
    // if has process then remove and return minProcess
    // list_del(minProcess)// reinitialize??
    list_del_init(&minProcess->queue_elem);
    return minProcess;
  }
}

/* Update the process' pass value after a run by the scheduler.
   This function is called from scheduler().
*/
void update_pass_value(struct proc *proc)
{
  proc->stride_info.pass_value += proc->stride_info.stride;
}

/* Update the global variable, ptable.min_pass_value, after a process' run by the scheduler.
   The function assigns the lowest pass value among RUNNABLE processes to ptable.min_pass_value.
   This function is called from scheduler().
*/
void update_min_pass_value()
{
  struct proc *targetProcess = NULL;
  int isFirst = 1; // to determine if finding minimun value is just started
  long long minPassValue = 0; // minimun pass value is at least 0
  long long itemPassValue = 0;
  struct list_head *item = NULL;
  struct list_head *head = &ptable.queue_head;

  // iterate all list
  list_for_each(item, head)
  {
    targetProcess = list_entry(item, struct proc, queue_elem);

    // when not runnable skip
    if (targetProcess->state != RUNNABLE)
      continue;
    
    itemPassValue = targetProcess->stride_info.pass_value;
    // find minimun pass value
    if(isFirst)
    { // when first just store anything 
      minPassValue = itemPassValue;
      isFirst = 0;
    }
    else
    { // if smaller one is found, store that as minPassValue
      if(minPassValue > itemPassValue)
      {
        minPassValue = itemPassValue;
      }
    }
  }

  // update min value to global variable
  ptable.min_pass_value = minPassValue;
}

/* Insert the current process into the queue after a run by the scheduler.
   This function is called from scheduler().
*/
void insert(struct list_head *head, struct proc *current)
{
  INIT_LIST_HEAD(&current->queue_elem);
  list_add_tail(&current->queue_elem, head);
}

/* Assign the lowest pass value in the system to a new process or wake-up process.
   This function is called from fork() and wakeup1().
*/
void assign_min_pass_value(struct proc *proc)
{
  proc->stride_info.pass_value = ptable.min_pass_value; // assign min_pass_value to given proc
}

/* Assign Tickets to current (cpu running) process by system call
*/
void assign_tickets(int tickets)
{
  // assign new ticket to running process (mycpu()->proc)
  myproc()->stride_info.tickets = tickets;
  // stride = a large number / number of ticket
  myproc()->stride_info.stride = (STRIDE_LARGE_NUMBER)/(tickets);
}

/* Initialize the process's stride_info member variables.
   The initial tickets value will be 100.
   This function is called from allocproc().
*/
void initialize_stride_info(struct proc *proc)
{
  proc->stride_info.tickets = 100;
  proc->stride_info.pass_value = 0; // should start from zero
  // stride = a large number / number of ticket
  proc->stride_info.stride = (STRIDE_LARGE_NUMBER) / (proc->stride_info.tickets);
}

void pinit(void)
{
  initlock(&ptable.lock, "ptable");

  /* stride scheduling */
  ptable.large_number = STRIDE_LARGE_NUMBER;
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  p = (struct proc *)k_malloc(sizeof(struct proc));

  if (p != NULL)
  {
    memset(p, 0, sizeof(struct proc));
  }
  else
  {
    release(&ptable.lock);
    return 0;
  }

  INIT_LIST_HEAD(&p->queue_elem);
  list_add_tail(&p->queue_elem, &ptable.queue_head);

  /* stride scheduling */
  initialize_stride_info(p);

  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  INIT_LIST_HEAD(&ptable.queue_head);

  p = allocproc();

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  /* stride scheduling */
  assign_min_pass_value(np);

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.

  struct list_head *head = &ptable.queue_head;
  struct list_head *iter;

  list_for_each(iter, head)
  {
    p = list_entry(iter, struct proc, queue_elem);
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  struct list_head *head = &ptable.queue_head;
  struct list_head *iter, *n;

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    list_for_each_safe(iter, n, head)
    {
      p = list_entry(iter, struct proc, queue_elem);

      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;

        list_del_init(&p->queue_elem);
        k_free(p);

        release(&ptable.lock);

        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  struct list_head *head = &ptable.queue_head;

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    // 1. pick client with min pass
    p = remove_min(head);

    // if runnable process is found, run it
    if(p != NULL)
    {
      // 2. run p for quantum
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      // 3. update pass using stride
      update_pass_value(p);
      // 4. return current process to queue       
      insert(head,p);
      // after process run, update ptable min value
      update_min_pass_value();
    }

    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        //DOC: sleeplock0
    acquire(&ptable.lock); //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void wakeup1(void *chan)
{
  struct proc *p;
  struct list_head *head = &ptable.queue_head;
  struct list_head *iter;

  list_for_each(iter, head)
  {
    p = list_entry(iter, struct proc, queue_elem);
    if (p->state == SLEEPING && p->chan == chan)
    {
      p->state = RUNNABLE;

      /* stride scheduling */
      assign_min_pass_value(p);
    }
  }
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;
  struct list_head *head = &ptable.queue_head;
  struct list_head *iter;

  acquire(&ptable.lock);
  list_for_each(iter, head)
  {
    p = list_entry(iter, struct proc, queue_elem);

    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  struct list_head *head = &ptable.queue_head;
  struct list_head *iter;

  list_for_each(iter, head)
  {
    p = list_entry(iter, struct proc, queue_elem);
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
