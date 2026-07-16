#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/rpmsg.h>

#include "ipc_proto.h"

#define RPMSG_DEV   "/dev/rpmsg1"
#define CTRL_DEV    "/dev/rpmsg_ctrl1"
#define EPT_NAME    "rpmsg-client-sample"
#define EPT_DST     0x400

static const char *status_str(uint8_t s)
{
	switch (s) {
	case RESP_OK:        return "OK";
	case RESP_ERR_CMD:   return "bilinmeyen komut";
	case RESP_ERR_ID:    return "gecersiz id";
	case RESP_ERR_VALUE: return "gecersiz deger";
	case RESP_ERR_VER:   return "versiyon uyusmuyor";
	default:             return "?";
	}
}

static int ensure_endpoint(void)
{
	struct rpmsg_endpoint_info ept = {0};
	struct stat st;
	int fd, ret;

	if (stat(RPMSG_DEV, &st) == 0) {
		if (S_ISCHR(st.st_mode))
			return 0;

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

	usleep(100000);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"kullanim:\n"
		"  %s ping\n"
		"  %s led <0|1>\n"
		"  %s ledget\n"
		"  %s servo <0-180>\n"
		"  %s sget\n"
		"  %s soff\n"
		"  %s step <adim>      (+ileri, -geri; 4096 = 1 tur)\n"
		"  %s speed <us>       (adim arasi mikrosaniye, min 1000)\n"
		"  %s mstop\n"
		"  %s mget\n"
		"  %s raw <type> <id> <value>\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	struct ipc_cmd cmd = {0};
	struct ipc_resp resp;
	int fd, n;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	cmd.version = IPC_PROTO_VERSION;

	if (strcmp(argv[1], "ping") == 0) {
		cmd.type = CMD_PING;
	} else if (strcmp(argv[1], "led") == 0) {
		if (argc < 3) { usage(argv[0]); return 1; }
		cmd.type  = CMD_LED_SET;
		cmd.value = atoi(argv[2]);
	} else if (strcmp(argv[1], "ledget") == 0) {
		cmd.type = CMD_LED_GET;
	} else if (strcmp(argv[1], "servo") == 0) {
		if (argc < 3) { usage(argv[0]); return 1; }
		cmd.type  = CMD_SERVO_SET;
		cmd.value = atoi(argv[2]);
	} else if (strcmp(argv[1], "sget") == 0) {
		cmd.type = CMD_SERVO_GET;
	} else if (strcmp(argv[1], "soff") == 0) {
		cmd.type = CMD_SERVO_OFF;
	} else if (strcmp(argv[1], "step") == 0) {
		if (argc < 3) { usage(argv[0]); return 1; }
		cmd.type  = CMD_MOTOR_STEP;
		cmd.value = atoi(argv[2]);
	} else if (strcmp(argv[1], "mstop") == 0) {
		cmd.type = CMD_MOTOR_STOP;
	} else if (strcmp(argv[1], "mget") == 0) {
		cmd.type = CMD_MOTOR_GET;
	} else if (strcmp(argv[1], "speed") == 0) {
		if (argc < 3) { usage(argv[0]); return 1; }
		cmd.type  = CMD_MOTOR_SPD;
		cmd.value = atoi(argv[2]);
	} else if (strcmp(argv[1], "raw") == 0) {
		if (argc < 5) { usage(argv[0]); return 1; }
		cmd.type  = (uint8_t)strtol(argv[2], NULL, 0);
		cmd.id    = (uint8_t)strtol(argv[3], NULL, 0);
		cmd.value = (int32_t)strtol(argv[4], NULL, 0);
	} else {
		usage(argv[0]);
		return 1;
	}

	if (ensure_endpoint() < 0)
		return 1;

	fd = open(RPMSG_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " RPMSG_DEV);
		return 1;
	}

	if (write(fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
		perror("write");
		close(fd);
		return 1;
	}
	printf("-> type=0x%02x id=%u value=%d\n", cmd.type, cmd.id, cmd.value);

	n = read(fd, &resp, sizeof(resp));
	if (n < 0) {
		perror("read");
		close(fd);
		return 1;
	}
	if (n != sizeof(resp)) {
		fprintf(stderr, "kisa cevap: %d byte (beklenen %zu)\n", n, sizeof(resp));
		close(fd);
		return 1;
	}

	printf("<- type=0x%02x status=%s value=%d\n",
	       resp.type, status_str(resp.status), resp.value);

	close(fd);
	return resp.status == RESP_OK ? 0 : 1;
}