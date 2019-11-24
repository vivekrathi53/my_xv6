#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
int sys_cps(void)
{	
	struct procList *pl;
	if(argptr(0,(void*)&pl,sizeof(pl))<0)
		return -1;
	return cps(pl);	
}
int
sys_chpr (void)
{
	int pid, pr;
	if(argint(0, &pid) < 0)
		return -1;
	if(argint(1, &pr) < 0)
		return -1;
	return chpr ( pid, pr );
}
int
sys_setTickets (void)
{
	int pid, tickets;
	if(argint(0, &pid) < 0)
		return -1;
	if(argint(1, &tickets) < 0)
		return -1;
	return  setTickets( pid, tickets );
}
int sys_thread_create(void)
{
	void (*fcn)(void*);
	void* arg;
	void* stack;
	if(argptr(0,(void*)&fcn,sizeof(fcn))<0)
	{
		return -1;
	}	
	if(argptr(1,(void*)&arg,sizeof(arg))<0)
		return -1;
	if(argptr(2,(void*)&stack,sizeof(stack))<0)
		return -1;
	return thread_create((void*)fcn,(void*)arg,(void*)stack);
}
int sys_thread_exit(void)
{
	void *retval;
	if(argptr(0,(void*)&retval,sizeof(retval))<0)
		return 0;	
	thread_exit((void*)retval);
	return 1;
}
int sys_thread_join(void)
{
	return 1;
}
int sys_gettid(void)
{
	return gettid();
}




