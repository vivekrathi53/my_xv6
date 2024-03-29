#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;
static struct proc *initproc;
int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->tid = 1;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  p->priority=10;
  p->tickets=10;
  return p;
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
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
  p->tf->eip = 0;  // beginning of initcode.S

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
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
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

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
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
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
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
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
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
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
int random_variable=0;
int rand()
{
	random_variable+=37*random_variable+2;
	return random_variable%1007;
}
#define NULL 0
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  struct proc *p1;
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();
    //struct proc *highP = NULL;
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    		if(p->state != RUNNABLE)
        continue;
	/* ----------CODE-FOR-HIGHEST-PRIORITY-FIRST-SCHEDULING--------
	highP=p;
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
	// choose the one with highest priority
	for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++)
	{
		if(p1->state != RUNNABLE)
			continue;
		if ( highP->priority > p1->priority )
		// larger value, lower priority
		highP = p1;
	}
	p=highP;
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
	--------------CODE-DONE---------------------------------------	
	*/
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
	// choose the one with highest priority
	int total=0;
	for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++)
	{
		if(p1->state != RUNNABLE)
			continue;
		total+=p1->tickets;
	}	
	int num=rand()%total+1;
	int cur=0;
	for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++)
	{
		if(p1->state != RUNNABLE)
			continue;
		if(num<cur)
		{
			p=p1;
			break;
		}
		cur+=p1->tickets;
	}
	//p=highP;
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();
	
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
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
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
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
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
int
cps(struct procList *pl)
{
	struct proc *p;// this is sructure which holds infrmation about a process
	sti(); // used to eable interupts on this processor
	acquire(&ptable.lock); // get all details from process table 
	cprintf("name\tpid\tstate\tpriority\ttickets\n");
	p=ptable.proc;
	for(int i=0,j=0;i<NPROC;i++)
	{
		if(p[i].state == SLEEPING)
		{
			cprintf("%s\t%d\tSLEEPING\t%d\t%d\n",p[i].name,p[i].pid,p[i].priority,p[i].tickets);
			//(*sco)++;
			pl->p[j].state=p[i].state;
			pl->p[j].pid=p[i].pid;
			//pl->p[j].name=p[i].name;
			
			pl->p[j].priority=p[i].priority;
			pl->p[j].tickets=p[i].tickets;
			j++;
		}
		else if(p[i].state == RUNNING)
		{
			cprintf("%s\t%d\tRUNNING \t%d\t%d\n",p[i].name,p[i].pid,p[i].priority,p[i].tickets);
			//(*rco)++;
			pl->p[j].state=p[i].state;
			pl->p[j].pid=p[i].pid;
			//pl->p[j].name=p[i].name;
			pl->p[j].priority=p[i].priority;
			pl->p[j].tickets=p[i].tickets;
			j++;
			
		}
		else if(p[i].state == RUNNABLE)
		{
			cprintf("%s\t%d\tRUNNABLE\t%d\t%d\n",p[i].name,p[i].pid,p[i].priority,p[i].tickets);
			pl->p[j].state=p[i].state;
			pl->p[j].pid=p[i].pid;
			//pl->p[j].name=p[i].name;
			pl->p[j].priority=p[i].priority;
			pl->p[j].tickets=p[i].tickets;
			j++;
		}
		
	}	
	release(&ptable.lock);
	return 22;
}
//change priority
int chpr( int pid, int priority )
{
	struct proc *p;
	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->pid == pid ) {
			p->priority = priority;
			break;
		}
	}
	release(&ptable.lock);
	return pid;
}
// lottery scheduling problem
int
setTickets(int pid,int tickets)
{
	struct proc *p;// this is sructure which holds infrmation about a process
	sti(); // used to eable interupts on this processor
	acquire(&ptable.lock); // get all details from process table 
	p=ptable.proc;
	for(int i=0;i<NPROC;i++)
	{
		if(p[i].pid==pid)
		{
			p[i].tickets=tickets;
			break;
		}
	}	
	release(&ptable.lock);
	return 24;
}
// thread creation function
int
thread_create(void (*function)(void*),void* arg,void* stack)
{
	int next_tid=1;
	int number_of_threads=0;
	struct proc *curproc=myproc();// current process proc structure or pcb
	struct proc *np;// for new thread pcb
	struct proc *p;// for iterating over ptable
	acquire(&ptable.lock);
	p=ptable.proc;
	for(int i=0;i<NPROC;i++)
	{
		if(p[i].pid==curproc->pid)
		{
			if(p[i].tid>next_tid)
			{
				next_tid=p[i].tid;
			}
			if(p[i].state!=UNUSED)
			{
				number_of_threads++;
			}
		}
	}
	next_tid++;
	release(&ptable.lock);
	if(number_of_threads>=8)
		return -1;
	if((uint)stack%PGSIZE!=0)
		return -1;
	if((np=allocproc())==0)
		return -1;
	
	np->tid=next_tid;
	np->pgdir=curproc->pgdir;
	np->sz=curproc->sz;
	np->parent=curproc;
	*(np->tf)=(*curproc->tf);
	np->tf->esp=(uint)stack+4096-8;
	np->tf->eax=0;
	np->tf->eip=(uint)function;
	np->tf->ebp=(uint)stack+4096-8;
	int *sp=stack+4096-8;
	*(sp+1)=(uint)arg;
	(*sp)=0xffffffff;
	for(int i = 0; i < NOFILE; i++)
	    if(curproc->ofile[i])
	      np->ofile[i] = filedup(curproc->ofile[i]);
	np->cwd = idup(curproc->cwd);
	safestrcpy(np->name,curproc->name,sizeof(curproc->name));
	np->pid=curproc->pid;
	acquire(&ptable.lock);
	np->state=RUNNABLE;
	release(&ptable.lock);  
	return next_tid;
}
void thread_exit(void *retval)
{
	struct proc* curproc=myproc();//my current process to exit
	struct proc* p;// p variable to iterate over process table wherever necessary 
	if(curproc==initproc)
	{
		panic("init exiting");
	}
	if(curproc->tid==1)// main thread
	{
		exit();
		return; 
	}
	for(int i=0;i<NOFILE;i++)
	{
		if(curproc->ofile[i])
		{
			fileclose(curproc->ofile[i]);
			curproc->ofile[i]=0;
		}
	}
	
	begin_op();
	iput(curproc->cwd);
	end_op();
	curproc->cwd=0;
	
	acquire(&ptable.lock);
	p=ptable.proc;
	wakeup1(curproc->parent);
	for(int i=0;i<NPROC;i++)
	{
		if(p[i].pid==curproc->pid)
		{
			p[i].parent=initproc;
			if(p[i].state==ZOMBIE)
			{
				wakeup1(p[i].parent);
			}
		}
	}
	for(int i=0;i<NPROC;i++)
	{
		if(p[i].pid==curproc->pid&&p[i].tid!=curproc->tid)
		{
			wakeup1(&p[i]);
		}
	}
	release(&ptable.lock);
	curproc->state=ZOMBIE;
	sched();
	panic("zombie Exit");
}
int thread_join(int tid,void** retval)
{
	struct proc* p;
	struct proc* curproc=myproc();
	acquire(&ptable.lock);
	p=ptable.proc;
	int flag=0;
	for(;;)
	{
		for(int i=0;i<NPROC;i++)
		{
			if(p[i].pid!=curproc->pid)
				continue;
			if(p[i].tid==tid)
			{
				flag=1;
				if(p[i].state==ZOMBIE)
				{
					kfree(p[i].kstack);
					p[i].state=UNUSED;
					p[i].tid=0;
					p[i].pid=0;
					p[i].parent=0;
					p[i].killed=0;
					*retval=(void*)p[i].retval;
					p[i].retval=0;
					release(&ptable.lock);
					return 0;
				}
				if(p[i].state==UNUSED)
				{
					release(&ptable.lock);
					return 0;
				}
			}
		}
		release(&ptable.lock);	
		if(curproc->killed||flag==0)
		{
			release(&ptable.lock);
			return -1;
		}
		sleep(curproc,&ptable.lock);
	}
	return 0;
}
int gettid(void)
{
	return myproc()->tid;
}

