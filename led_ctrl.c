#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/rpmsg.h>

#define RPMSG_DEV   "/dev/rpmsg1"
#define CTRL_DEV    "/dev/rpmsg_ctrl1"
#define EPT_NAME    "rpmsg-client-sample"
#define EPT_DST     0x400

/* Endpoint yoksa acar. Varsa ve dogru tipteyse dokunmaz. */
static int ensure_endpoint(void)
{
	struct rpmsg_endpoint_info ept = {0};
	struct stat st;
	int fd, ret;

	if (stat(RPMSG_DEV, &st) == 0) {
		if (S_ISCHR(st.st_mode))
			return 0;       /* zaten var ve karakter cihazi */

		/* yanlislikla olusmus duz dosya (ornegin 'tee' yuzunden) */
		fprintf(stderr, "uyari: %s karakter cihazi degil, siliniyor\n", RPMSG_DEV);
		if (unlink(RPMSG_DEV) < 0) {
			perror("unlink " RPMSG_DEV);
			return -1;
		}
	}

	fd = open(CTRL_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " CTRL_DEV);
		return -1;
	}

	strncpy(ept.name, EPT_NAME, sizeof(ept.name) - 1);
	ept.src = 0xFFFFFFFF;
	ept.dst = EPT_DST;

	ret = ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept);
	close(fd);
	if (ret < 0) {
		perror("ioctl CREATE_EPT");
		return -1;
	}

	usleep(100000);             /* cihazin belirmesini bekle */
	return 0;
}

int main(int argc, char *argv[])
{
	char buf[64];
	int fd, n;

	if (argc < 2) {
		fprintf(stderr, "kullanim: %s <0|1>\n", argv[0]);
		return 1;
	}

	if (ensure_endpoint() < 0)
		return 1;

	fd = open(RPMSG_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " RPMSG_DEV);
		return 1;
	}

	/* komutu gonder */
	if (write(fd, argv[1], 1) != 1) {
		perror("write");
		close(fd);
		return 1;
	}
	printf("gonderildi: %s\n", argv[1]);

	/* echo cevabini oku (R5 mesaji geri yolluyor) */
	n = read(fd, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		printf("cevap: %s (%d byte)\n", buf, n);
	} else if (n < 0) {
		perror("read");
	}

	close(fd);
	return 0;
}
