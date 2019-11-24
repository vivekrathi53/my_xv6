#include "types.h"
#include "user.h"
#include "stat.h"
#include "fcntl.h"

int main(int argc,char* argv[])
{
	if(argc<=2||argc>3)
		printf(1,"Illegal number Of Arguments");
	int pid=0,tickets=0;
	int val=1;
	for(int i=0;i<strlen(argv[1]);i++)
	{
		pid+=(argv[1][strlen(argv[1])-i-1]-'0')*val;
		val*=10;
	}	
	val=1;
	for(int i=0;i<strlen(argv[2]);i++)
	{
		tickets+=(argv[2][strlen(argv[2])-i-1]-'0')*val;
		val*=10;
	}
	setTickets(pid,tickets);
	exit();
}
