#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
int main(int argc, char *argv[])
{	
	int num=atoi(argv[1]);
	for(int i=0;i<num;i++)
	{
		if(fork()==0)
		{
			sleep(200);
			exit();
		}
	}
	//return 0;
	exit();
}
