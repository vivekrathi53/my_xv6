#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
enum procstates { UNUSEDS, EMBRYOS, SLEEPINGS, RUNNABLES, RUNNINGS, ZOMBIES };// enum name and its values changed to differentiate from earlier existing enum
struct myproc {
  enum procstates state;        // Process state
  int pid;                     // Process ID
  char name[16];               // Process name (debugging)
  int priority; 		// denotes process priority higher number denotes lower priority
  int tickets;
};
struct procList
{
	struct myproc p[10];
};


int main(int argc, char *argv[])
{
	struct procList pl;	
	cps(&pl);
	for(int i=0;i<10;i++)// implement logic here for options passed with ps to display various details of process
	{
		if(pl.p[i].pid!=0)
			printf(1,"%d\n",pl.p[i].pid);
	}
	exit();
	return 0;
}
