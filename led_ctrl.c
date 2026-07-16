#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/rpmsg.h>

int main(int argc, char *argv[])
{
	struct rpmsg_endpoint_info ept = {0};
	int fd, ret;

	if (argc < 2) {
		fprintf(stderr, "kullanim: %s <0|1>\n", argv[0]);
		return 1;
	}

	fd = open("/dev/rpmsg_ctrl1", O_RDWR);
	if (fd < 0) { perror("open ctrl1"); return 1; }

	strcpy(ept.name, "rpmsg-client-sample");
	ept.src = 0xFFFFFFFF;
	ept.dst = 0x400;

	ret = ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept);
	if (ret < 0) { perror("ioctl create ept"); close(fd); return 1; }
	close(fd);

	printf("endpoint olusturuldu, /dev/rpmsg* kontrol et\n");
	return 0;
}
