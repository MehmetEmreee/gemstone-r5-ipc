/*
 * Linux (A53) <-> Zephyr (R5F) ortak IPC protokolu.
 * Bu dosya IKI tarafta da ayni olmali.
 */
#ifndef IPC_PROTO_H
#define IPC_PROTO_H

#include <stdint.h>

#define IPC_PROTO_VERSION  1


/* Komut tipleri */
enum ipc_cmd_type {
	CMD_PING       = 0x00,
	CMD_LED_SET    = 0x01,
	CMD_LED_GET    = 0x02,
	CMD_MOTOR_STEP = 0x10,   /* value: adim sayisi (+ileri, -geri) */
	CMD_MOTOR_STOP = 0x11,
	CMD_MOTOR_GET  = 0x12,   /* mevcut pozisyon */
	CMD_MOTOR_SPD  = 0x13,   /* value: adim arasi us (min ~1500) */
	CMD_STATUS_GET = 0x30,
	CMD_SERVO_SET  = 0x20,   /* value: aci 0-180 derece */
	CMD_SERVO_GET  = 0x21,
	CMD_SERVO_OFF  = 0x22,   /* pulse uretimini durdur */
};

/* Cevap kodlari */
enum ipc_resp_status {
	RESP_OK        = 0x00,
	RESP_ERR_CMD   = 0x01,   /* bilinmeyen komut */
	RESP_ERR_ID    = 0x02,   /* gecersiz id */
	RESP_ERR_VALUE = 0x03,   /* gecersiz deger */
	RESP_ERR_VER   = 0x04,   /* protokol versiyonu uyusmuyor */
};

/* Linux -> R5 */
struct ipc_cmd {
	uint8_t  version;
	uint8_t  type;
	uint8_t  id;
	uint8_t  _pad;
	int32_t  value;
} __attribute__((packed));

/* R5 -> Linux */
struct ipc_resp {
	uint8_t  version;
	uint8_t  type;
	uint8_t  status;
	uint8_t  _pad;
	int32_t  value;
} __attribute__((packed));

#endif /* IPC_PROTO_H */
