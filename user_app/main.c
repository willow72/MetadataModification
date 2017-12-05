#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>

long long GetTimeDiff(unsigned int nFlag)
{
	const long long NANOS = 1000000000LL;
	static struct timespec startTS, endTS;
	static long long retDiff = 0;
	if(nFlag == 0)
	{
		retDiff = 0;
		clock_gettime(CLOCK_MONOTONIC, &startTS);
	}else{
		clock_gettime(CLOCK_MONOTONIC, &endTS);
		retDiff = NANOS * (endTS.tv_sec - startTS.tv_sec) + (endTS.tv_nsec-startTS.tv_nsec);
	}

	return retDiff/1000;
}

int main(void)
{
	int fd = open("./testfile", O_RDWR);
	int offset = 0;
	int size = 0;
	int file_size = 0;
	long long diff = 0;

	file_size = lseek(fd, 0, SEEK_END);
	printf("file size : %d\n", file_size);
	offset = 0;
//	size = file_size*0.1;
	size = 4096 * 1024;
	GetTimeDiff(0);
	syscall(329, fd, offset, size);
	printf("time : %lld\n", GetTimeDiff(1));
	close(fd);
	return 0;
}
