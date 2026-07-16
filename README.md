<div align="center">

# T3-Gemstone-O1 Deterministik Motor Kontrolü

**Linux ↔ Zephyr AMP köprüsü — TI AM67A (J722S) üzerinde gerçek zamanlı donanım kontrolü**

Linux'tan komut ver, Cortex-R5F'de çalışan Zephyr donanımı deterministik zamanlamayla sürsün.

[![Platform](https://img.shields.io/badge/SoC-TI%20AM67A%20(J722S)-red)]()
[![Board](https://img.shields.io/badge/board-T3%20Gemstone%20O1-blue)]()
[![RTOS](https://img.shields.io/badge/RTOS-Zephyr%204.4-purple)]()
[![IPC](https://img.shields.io/badge/IPC-OpenAMP%20%2F%20RPMsg-green)]()

</div>

---

## Ne yapar

```bash
$ sudo ./led_ctrl servo 90
-> type=0x20 id=0 value=90
<- type=0x20 status=OK value=90

$ sudo ./led_ctrl step 4096      # 1 tam tur, komut hemen döner
-> type=0x10 id=0 value=4096
<- type=0x10 status=OK value=451  # motor hâlâ önceki komuttan dönüyor
```

Linux (A53) komutu verir ve **işine döner**. R5F (Zephyr) motoru kendi ritminde, kesintisiz sürer. Linux takılsa, meşgul olsa, hatta çökse bile motor döngüsü aksamaz.

İşte AMP'nin bütün mesele bu.

---

## İçindekiler

- [Neden AMP?](#neden-amp)
- [Mimari](#mimari)
- [Donanım](#donanım)
- [Hızlı başlangıç](#hızlı-başlangıç)
- [Komut protokolü](#komut-protokolü)
- [Yeni komut ekleme](#yeni-komut-ekleme)
- [Kritik Kconfig ayarları](#kritik-kconfig-ayarları)
- [Tuzaklar](#tuzaklar--öğrenilen-dersler)
- [Dosya yapısı](#dosya-yapısı)

---

## Neden AMP?

> *"Linux tarafım yavaşken bir çekirdeğimi realtime koşturmak ne işime yarayacak? Bir insanın bir bacağı 100 km/h giderken diğeri 60 km/h gidebiliyorsa, stabil hareket için 60'a inmek zorunda kalır."*

Bu sezgi, iki çekirdeğin **aynı işi** yaptığı ve birbirini beklediği varsayımına dayanır. AMP'nin tüm olayı bu varsayımı kırmaktır.

Doğru benzetme: **orkestra şefi ve metronom.**

| | A53 · Linux | R5F · Zephyr |
|---|---|---|
| **İyi olduğu şey** | Throughput — bol veriyi hızlı öğütmek | Latency & determinizm — az veriyi tam zamanında |
| **Görevi** | Görüntü işleme, ağ, kütüphaneler, karar | Motor döngüsü, pulse zamanlaması, refleks |
| **Ölçüsü** | Ortalama hız | **En kötü durum gecikmesi** |
| **Bir tur kaçırırsa** | Bir frame düşer, kimse fark etmez | Motor sarsılır, adım kaçar, sargı yanar |

Motor kontrolünde önemli olan ortalama değil, **her seferinde** randevuyu tutturmak. Linux (PREEMPT_RT ile bile) çoğu zaman tutturur — ama bir ağ kesmesi, bir sürücü kilidi, bir bellek işlemi araya girdiğinde milisaniyeler geç kalabilir. Ortalaması harika, **en kötü durumu öngörülemez**.

R5F'de araya girecek katman yok. Söz verdiği anda orada.

**Kritik nokta:** R5 "daha hızlı" değil — A53 ham güçte kat kat üstün. R5 **güvenilir**. Ve ikisi birbirini beklemediği için sistemin toplam kalitesi ikisinin *çarpımı*, en yavaşına inen ortalaması değil.

---

## Mimari

### Katmanlar

Haberleşme tek bir API değil, üst üste oturan üç katman:

```
┌─────────────────────┐                    ┌─────────────────────┐
│    A53 · Linux      │                    │    R5F · Zephyr     │
│  echo/led_ctrl      │                    │  rpmsg callback     │
└──────────┬──────────┘                    └──────────▲──────────┘
           │                                          │
    ┌──────▼──────────────────────────────────────────┴──────┐
    │  RPMsg — adresli kanallar (endpoint), mesajlaşma       │
    │  rpmsg-client-sample @ 0x400                           │
    ├────────────────────────────────────────────────────────┤
    │  virtio / vring — halka tamponlar, tampon sözleşmesi   │
    │  virtio1 (remoteproc3 altında)                         │
    ├────────────────────────────────────────────────────────┤
    │  Paylaşılan bellek — DDR 0xa2000000 (ddr0, 1 MB)       │
    └────────────────────────────────────────────────────────┘
                              ▲
                   ┌──────────┴──────────┐
                   │  Mailbox (donanım)  │  ← kesme, polling yok
                   │  mbox3 (MAIN)       │
                   └─────────────────────┘
```

**Akış:** `write(/dev/rpmsg1, &cmd)` → rpmsg paketi paylaşılan belleğe koyar → mailbox R5'te kesme tetikler → Zephyr callback uyanır → `dispatch()` → `gpio_pin_set_dt()`

Mailbox kritik: R5 boşuna bellek yoklamaz, sadece iş geldiğinde uyanır.

### Çekirdek haritası

| remoteproc | Adres | Tür | Durum |
|---|---|---|---|
| remoteproc0/1 | `7e000000/7e200000.dsp` | C7x DSP | ilgisiz |
| remoteproc2 | `79000000.r5f` | MCU-domain R5F | **dokunma** — TI ping-pong fw |
| **remoteproc3** | `78400000.r5f` | **MAIN-domain R5F** | ✅ **Zephyr burada** |

> **Uyarı:** `dmesg`'de `virtio0: rpmsg host is online` görüp sevinme — o remoteproc2'ye (MCU-R5) ait ve boot'tan beri orada. Seninki **virtio1**.

### TI MCU+ SDK ile ilişkisi

TI'ın MCU+ SDK dokümantasyonu kendi IPC yığınını anlatır (FreeRTOS/NORTOS üzerinde). Bizim R5'te **Zephyr** koştuğu için o API'ler birebir kullanılmaz. Köprü, iki tarafın da konuştuğu ortak protokol olan **RPMsg/virtio** üzerinden kurulur.

Kavramlar birebir örtüşür:

| TI MCU+ SDK | Buradaki karşılığı |
|---|---|
| IPC Notify | Mailbox (`mbox3`) |
| IPC RPMessage | RPMsg endpoint |
| Shared memory | `ddr0` @ `0xa2000000` |

---

## Donanım

| | |
|---|---|
| Kart | T3 Gemstone O1 |
| SoC | TI AM67A (= J722S, Jacinto 7) |
| Linux | 4× Cortex-A53, kernel `6.12.24-ti`, PREEMPT_RT |
| Zephyr | MAIN-domain Cortex-R5F, remoteproc3 |
| Zephyr board | `beagley_ai/j722s/main_r5f0_0` |
| Host | Ubuntu 24.04 (aarch64) |
| Seri konsol | UART1 `0x2810000` — pin 8 (TX) / 10 (RX) / 6 (GND), **115200 8N1** |

### Pin haritası

```
                    T3 Gemstone O1 — 40 pin header
        ┌───────────────────────────────────────────────┐
   3v3  │  1   2  │ 5V ──────────── ULN2003 VCC         │
        │  3   4  │ 5V ──────────── Servo kırmızı       │
        │  5   6  │ GND ─────────── ULN2003 GND         │
        │  7   8  │ ← UART1 TX (Zephyr konsolu)         │
        │  9  10  │ ← UART1 RX                          │
IN1 ────│ 11  12  │                                     │
Servo ──│ 13  14  │ GND                                 │
        │ 15  16  │                                     │
        │ ...     │                                     │
        │ 29  30  │ ← IN2                               │
LED ────│ 31  32  │ (PWM-ECAP0 — kullanma)              │
        │ 33  34  │ (PWM-1B — kullanma)                 │
IN4 ────│ 35  36  │ ← IN3                               │
        │ 37  38  │                                     │
        │ 39  40  │                                     │
        └───────────────────────────────────────────────┘
```

### Pin kimlik tablosu

Her pinin **beş** farklı ismi var. Tek güvenilir bağ: pinmux offset.

| İşlev | Header | Gemstone | Linux | TI pad | Zephyr node + offset | pinctrl |
|---|---|---|---|---|---|---|
| **LED** | 31 | GPIO-6 | `gpiochip3` 17 | `SPI0_CLK.GPIO1_17` | `main_gpio1_0` 17 | `hat_31_gpio` (`0x1bc`) |
| **IN1** | 11 | GPIO-17 | `gpiochip3` 8 | `MCASP0_AXR2.GPIO1_8` | `main_gpio1_0` 8 | `hat_11_gpio` |
| **IN2** | 29 | GPIO-5 | `gpiochip3` 15 | `SPI0_CS0.GPIO1_15` | `main_gpio1_0` 15 | `hat_29_gpio` |
| **IN3** | 36 | GPIO-16 | `gpiochip3` 7 | `MCASP0_AXR3.GPIO1_7` | `main_gpio1_0` 7 | `hat_36_gpio` |
| **IN4** | 35 | GPIO-19 | `gpiochip3` 12 | `MCASP0_AFSX.GPIO1_12` | `main_gpio1_0` 12 | `hat_35_gpio` |
| **Servo** | 13 | GPIO-27 | `gpiochip2` 33 | `GPMC0_OEn_REn.GPIO0_33` | `main_gpio0_1` **1** | `hat_13_gpio` (`0x088`) |

**Pinmux offset → PADCONFIG:** `offset / 4`. Örnek: `0x1bc / 4 = 111` → PADCONFIG111.

**Bank offset dikkat:** J722S GPIO blokları Zephyr'de bölünmüş. `main_gpio0_0` = line 0–31, `main_gpio0_1` = line 32–63 **ama kendi içinde 0'dan sayar**. Linux `gpiochip2` line 33 → Zephyr `main_gpio0_1` offset **33−32 = 1**.

### Bağlantılar

| Bileşen | Bağlantı |
|---|---|
| **LED** | anot → 330Ω → pin 31, katot → GND |
| **28BYJ-48 + ULN2003** | IN1→11, IN2→29, IN3→36, IN4→35, VCC→2, GND→6 |
| **SG90 servo** | sinyal→13, 5V→4, GND→9 |

> ⚠️ **Besleme:** SG90 hareket anında 200–700 mA, 28BYJ-48 ~200 mA çeker. İkisi birden kartın 5V pininden beslenirse gerilim düşer → kart resetlenir → SD kart bozulabilir. Ciddi kullanımda harici besleme (GND ortak) kullan.

---

## Hızlı başlangıç

### 1. Bir kez kurulum

```bash
# SSH anahtarı (host → kart)
ssh-copy-id gemstone@192.168.7.2

# Şifresiz deploy (kartta) — SSH üzerinden sudo şifre soramaz
echo "gemstone ALL=(ALL) NOPASSWD: /bin/cp /tmp/zephyr.elf /lib/firmware/zephyr.elf, /sbin/reboot" \
  | sudo tee /etc/sudoers.d/rpmsg-deploy
sudo chmod 440 /etc/sudoers.d/rpmsg-deploy
```

### 2. Deploy

```bash
make deploy      # sync + derle + firmware at + client at + reboot
```

Tek komut. `~30sn` bekle.

| Hedef | Ne yapar |
|---|---|
| `make sync` | repo → `~/zephyrproject/led_ipc` (build alanı) |
| `make build` | `west build -p always` |
| `make fw` | firmware'i karta at |
| `make client` | sadece `led_ctrl` (reboot gerekmez) |
| `make deploy` | hepsi + reboot |
| `make clean` | build klasörünü sil |

> **Kaynak repo'da tutulur.** `~/zephyrproject/led_ipc` sadece build alanıdır — orada dosya düzenleme, `make sync` üzerine yazar. Tersini yaparsan repo eskir.

### 3. Kullan

```bash
sudo ~/led_ctrl ping           # bağlantı testi → 0xABCD
sudo ~/led_ctrl led 1          # LED yak
sudo ~/led_ctrl ledget         # LED durumu oku

sudo ~/led_ctrl step 4096      # 1 tam tur ileri (fire-and-forget)
sudo ~/led_ctrl step -512      # geri
sudo ~/led_ctrl speed 1500     # adım arası µs (min 1000)
sudo ~/led_ctrl mget           # pozisyon oku
sudo ~/led_ctrl mstop          # durdur

sudo ~/led_ctrl servo 90       # 0–180°
sudo ~/led_ctrl sget
sudo ~/led_ctrl soff           # pulse'ı kes (akım çekmesin, ısınmasın)

sudo ~/led_ctrl raw 0x99 0 0   # bilinmeyen komut → RESP_ERR_CMD
```

### 4. Firmware'i başlatma

```bash
sudo reboot
```

> **`echo stop > /sys/class/remoteproc/remoteproc3/state` ÇALIŞMAZ.**
> ```
> k3_r5_rproc_stop: timeout waiting for rproc completion event
> remoteproc remoteproc3: can't stop rproc: -16
> ```
> Takılan çekirdek "durdum" onayı gönderemez. Sistem açılışta symlink'in gösterdiği firmware'i **otomatik başlatır** — dosyayı değiştir + reboot yeterli. `stop` ile vakit kaybetme.

---

## Komut protokolü

Tek byte yerine yapılandırılmış mesaj. `ipc_proto.h` **iki tarafta da aynı** olmalı.

```c
struct ipc_cmd {          /* Linux → R5 */
    uint8_t  version;     /* IPC_PROTO_VERSION */
    uint8_t  type;        /* enum ipc_cmd_type */
    uint8_t  id;          /* cihaz no */
    uint8_t  _pad;
    int32_t  value;
} __attribute__((packed));

struct ipc_resp {         /* R5 → Linux */
    uint8_t  version;
    uint8_t  type;        /* hangi komuta cevap */
    uint8_t  status;      /* enum ipc_resp_status */
    uint8_t  _pad;
    int32_t  value;
} __attribute__((packed));
```

**`packed` neden şart:** İki taraf farklı derleyici kullanır (host `gcc` vs `arm-zephyr-eabi-gcc`). Packed olmadan biri araya dolgu byte'ı koyar, boyutlar uyuşmaz, veri bozulur.

**`version` neden var:** Protokol değişirse eski firmware sessizce yanlış davranmak yerine `RESP_ERR_VER` döner.

### Komut tablosu

| Kod | Komut | `value` | Açıklama |
|---|---|---|---|
| `0x00` | `CMD_PING` | — | → `0xABCD` |
| `0x01` | `CMD_LED_SET` | 0/1 | LED |
| `0x02` | `CMD_LED_GET` | — | ← durum |
| `0x10` | `CMD_MOTOR_STEP` | ±adım | 4096 = 1 tur, **bloklamaz** |
| `0x11` | `CMD_MOTOR_STOP` | — | |
| `0x12` | `CMD_MOTOR_GET` | — | ← pozisyon |
| `0x13` | `CMD_MOTOR_SPD` | µs | adım arası, 1000–100000 |
| `0x20` | `CMD_SERVO_SET` | 0–180 | derece |
| `0x21` | `CMD_SERVO_GET` | — | ← açı (−1 = kapalı) |
| `0x22` | `CMD_SERVO_OFF` | — | pulse'ı kes |
| `0x30` | `CMD_STATUS_GET` | — | (ayrılmış) |

### Cevap kodları

| Kod | Anlam |
|---|---|
| `RESP_OK` | tamam |
| `RESP_ERR_CMD` | bilinmeyen komut |
| `RESP_ERR_ID` | geçersiz id |
| `RESP_ERR_VALUE` | geçersiz değer |
| `RESP_ERR_VER` | protokol versiyonu uyuşmuyor |

### Linux tarafı: endpoint açma

`rpmsg_tty` modülü bu kernel'de **yok** (`modprobe: FATAL: Module rpmsg_tty not found`), ama `rpmsg_char` + `rpmsg_ctrl` var. Kanallar duyurulur ama `/dev/rpmsgX` otomatik oluşmaz — `/dev/rpmsg_ctrl1` üzerinden ioctl ile açılır:

```c
struct rpmsg_endpoint_info ept = {0};
strncpy(ept.name, "rpmsg-client-sample", sizeof(ept.name) - 1);
ept.src = 0xFFFFFFFF;
ept.dst = 0x400;
ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept);   /* → /dev/rpmsg1 belirir */
```

`led_ctrl` bunu kendi yapar — reboot sonrası tek komut yeter, manuel adım yok.

---

## Yeni komut ekleme

Üç satır. Komut tablosu (dispatch) sayesinde `if/else` çorbası yok.

**1.** `ipc_proto.h` — enum'a ekle:
```c
CMD_MOTOR_HOME = 0x14,
```

**2.** `main_remote.c` — handler yaz:
```c
static int handle_motor_home(const struct ipc_cmd *cmd, struct ipc_resp *resp)
{
    motor_target = -motor_position;
    k_sem_give(&motor_sem);
    resp->value = 0;
    return RESP_OK;
}
```

**3.** `main_remote.c` — tabloya ekle:
```c
static const struct cmd_entry cmd_table[] = {
    ...
    { CMD_MOTOR_HOME, handle_motor_home },
};
```

> Handler'lar `cmd_table`'dan **önce** tanımlanmalı.

---

## Kritik Kconfig ayarları

| Ayar | Neden şart |
|---|---|
| `CONFIG_PINCTRL=y` | Yoksa overlay'deki `pinctrl-0` **sessizce yok sayılır** → konsol "LED ON" der ama LED yanmaz |
| `CONFIG_LOG_BACKEND_UART=y` | `CONFIG_UART_CONSOLE=y` sadece `printk`'i yönlendirir; `LOG_INF` ayrı backend ister |
| `CONFIG_SHELL=n` | Orijinal örnek konsolu RPMsg'e bağlar → RPMsg açılamazsa firmware **tamamen sessiz** kalır |
| `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` | Servo pulse periyodu için. Bkz. [tick tuzağı](#servo-tık-tık-eder--tick-granülaritesi) |
| `CONFIG_GPIO=y` | |

**Kullanma:** `CONFIG_LOG_MODE_MINIMAL` — backend'i devre dışı bırakır, çıktı kaybolur.

**Kullanma:** `CONFIG_OPENAMP_WITH_DCACHE=y` — bu SoC'de **etkisiz**, aşağıya bak.

---

## Tuzaklar / Öğrenilen dersler

### Sinsi tuzak: Linux'un pinmux'unu miras almak

**Belirti:** Zephyr çalışır, konsol `LED ON` der, LED yanmaz. Sonra Linux'tan bir kez `sudo gpioset gpiochip3 17=1` çalıştırırsın — **o andan itibaren Zephyr'in kontrolü çalışmaya başlar**. Reboot → yine çalışmaz.

**Sebep:** `CONFIG_PINCTRL=y` yok. Overlay'deki `pinctrl-0` sessizce yok sayılır — derleme hatası bile vermez. Zephyr pad'i hiç ayarlamaz. `gpioset` pad'i MUX_MODE_7'ye alır ve ayar kalıcı olduğu için Zephyr "sanki çalışıyormuş gibi" görünür.

**Ders:** *Bir şey ancak "başka bir şey önce yapılırsa" çalışıyorsa, o şey senin kodun değildir.* Reboot sonrası **hiçbir Linux GPIO komutu çalıştırmadan** test et.

### `CONFIG_OPENAMP_WITH_DCACHE` bu SoC'de çalışmaz

Diğer tüm board overlay'lerinde (imx8mp, imx93, stm32mp...) var, o yüzden bizde de eksik sanıp ekledik. **Etkisiz:**

```
warning: OPENAMP_WITH_DCACHE was assigned the value 'y' but got the value 'n'.
Check these unsatisfied dependencies: CACHE_MANAGEMENT (=n)
```

Zincir: `OPENAMP_WITH_DCACHE` → `CACHE_MANAGEMENT` → `depends on DCACHE || ICACHE`.
`.config`'de `DCACHE=y`, `ICACHE=y`, `CPU_HAS_DCACHE=y` **var** ama `CACHE_MANAGEMENT` yine `n` — TI J722S R5F portu cache management backend'ini implemente etmemiş. `CONFIG_CACHE=y` / `CONFIG_CACHE_MANAGEMENT=y` eklemek de işe yaramıyor.

**Ders:** Build çıktısını `tail -25` ile izlersen bu uyarıyı **göremezsin** — Kconfig uyarıları en başta çıkar. Bir ayarın gerçekten etkin olduğunu doğrula:
```bash
grep -E "^CONFIG_PINCTRL" ~/zephyrproject/zephyr/build/zephyr/.config
```

### İlk takılmayı ne çözdü? (dürüst cevap: bilmiyoruz)

İlk denemede firmware boot etti, `remote processor is now up` dedi, ama `rpmsg host is online` **hiç gelmedi**. Sonraki turda **aynı anda birden fazla şey** değiştirdik ve çalıştı:

- Konsolu UART'a aldık
- RPMsg shell'i kapattık (`CONFIG_SHELL=n`)
- Stack/heap oynadık
- `CONFIG_OPENAMP_WITH_DCACHE=y` ekledik *(ki etkisiz olduğu sonradan anlaşıldı)*

Hangisinin çözdüğü **kesin bilinmiyor**. En olası şüphe: shell'in RPMsg'e bağlanması kanal kurulumunda çakışma yaratıyordu.

**Ders:** Aynı anda birden fazla değişken değiştirme. Yoksa "ne çözdü" sorusunun cevabı kaybolur.

### Firmware'i önce KONUŞTUR

Orijinal örnekte `CONFIG_PRINTK=n` + konsol RPMsg'de. RPMsg açılamayınca firmware **tamamen sessiz** — nerede takıldığını görmek imkânsız. İlk iş: konsolu UART'a al, sonra hata ayıkla.

### Servo tık tık eder — tick granülaritesi

**Belirti:** Servo boşta dururken sürekli mikro-düzeltme yapar, tık tık ses çıkarır.

**Sebep:** Zephyr'in varsayılan tick'i 100 Hz = **10 ms granülarite**. `k_usleep(18500)` dediğinde çekirdek sadece tick sınırlarında uyanabilir → periyot 20 ms yerine 11.5–21.5 ms arası zıplar. Pulse genişliği doğru (`k_busy_wait` tick'e bağlı değil) ama pulse'lar arası mesafe oynak → servo tereddüt eder.

**Çözüm:** `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` → granülarite 100 µs.

**Bedeli:** Saniyede 10.000 tick kesmesi ≈ CPU'nun %1–2'si. R5 için ihmal edilebilir.

**Kazancı:** Tüm `k_usleep`/`k_sleep` çağrıları 100 µs hassasiyetinde — step motorun zamanlaması da düzeldi.

### SG90 pulse aralığı 500–2500 µs

Yaygın bilgi `1000–2000 µs` der, ama çoğu SG90 klonu `500–2500 µs` ister. `1000–2000` kullanırsan servo mekanik aralığın ancak **yarısını** kullanır — "açılar ikiye bölünmüş gibi" hissi.

> Bazı klonlar 500 µs'de mekanik sınıra dayanır ve **inler**. Ses duyarsan `soff` yap, aralığı daralt (600–2400).

### `tee` ile /dev/rpmsg1'e yazma

```bash
echo -n "1" | sudo tee /dev/rpmsg1     # cihaz YOKSA düz dosya oluşturur!
```

`/dev/rpmsg1` reboot sonrası kaybolur. `tee` cihaz yoksa **normal dosya** yaratır, yazma "başarılı" görünür ama mesaj hiçbir yere gitmez. O dosya diskte kalır, sonraki denemeleri de bozar.

```bash
$ ls -la /dev/rpmsg*
crw------- ... /dev/rpmsg0     # 'c' = karakter cihazı ✓
-rw-r--r-- ... /dev/rpmsg1     # '-' = düz dosya ✗
```

`led_ctrl`'deki `S_ISCHR()` kontrolü bunu otomatik yakalar.

### virtio0 vs virtio1

`dmesg`'de `virtio0: rpmsg host is online` görüp sevinme — o **MCU-R5**'e (remoteproc2, TI ping-pong fw) ait, boot'tan beri orada. Seninki **virtio1**:

```bash
ls -la /sys/bus/rpmsg/devices/
# .../remoteproc3/rproc-virtio.7.auto/virtio1/virtio1.rpmsg-client-sample.-1.1024
```

### Overlay'de `#include` unutma

```
devicetree error: ...overlay:5 (column 30): parse error: expected number or parenthesized expression
```

`GPIO_ACTIVE_HIGH` bir dt-binding sabiti. En üste: `#include <zephyr/dt-bindings/gpio/gpio.h>`

### GPIO node'ları varsayılan kapalı

J722S'de `dts/arm/ti/j722s_main.dtsi` içinde tüm GPIO node'ları `status = "disabled"`. Overlay'de açman şart:
```
&main_gpio1_0 { status = "okay"; };
```

### Adresler zaten hizalı — boş yere arama

Zephyr board tanımı `ddr0 @ 0xa2000000` der, Linux `main-r5fss-dma-memory-region@a2000000` atar. **Birebir uyuşuyor** — resmi board tanımı bunu halletmiş.

### Kaynak tek yerde olsun

Kaynak repo'da, `make sync` build alanına kopyalar. Tersini yaparsan (build alanında düzenle → repo'ya kopyala) er geç repo eskir. **Bir kez başımıza geldi:** `CONFIG_PINCTRL` repo'ya geçmemişti, saatler kaybettirdi.

### Echo testi yanıltıcı

`timeout 2 cat /dev/rpmsg1` boş dönebilir ama mesaj yine de ulaşmış olabilir. Kesin kanıt **seri konsoldur**.

---

## Dosya yapısı

```
.
├── Makefile                      # make deploy / build / client / clean
├── ipc_proto.h                   # ORTAK protokol — iki tarafta da aynı
├── led_ctrl.c                    # Linux istemcisi (endpoint aç + gönder + oku)
└── zephyr_app/                   # openamp_rsc_table tabanlı
    ├── CMakeLists.txt            # target_include_directories'e 'src' ekli
    ├── prj.conf
    ├── boards/
    │   └── beagley_ai_j722s_main_r5f0_0.overlay
    └── src/
        ├── main_remote.c         # RPMsg + dispatch + LED/motor/servo
        └── ipc_proto.h           # make sync ile kopyalanır
```

### Thread yapısı ve öncelikler

| Thread | Öncelik | Görev |
|---|---|---|
| `servo_thread` | `K_PRIO_COOP(4)` | 50 Hz pulse — en kritik zamanlama |
| `motor_thread` | `K_PRIO_COOP(5)` | half-step faz dizisi |
| `app_rpmsg_client_sample` | `K_PRIO_COOP(7)` | endpoint kurulumu |
| `app_rpmsg_tty` | `K_PRIO_COOP(7)` | (kullanılmıyor) |
| `rpmsg_mng_task` | `K_PRIO_COOP(8)` | virtio/mailbox yönetimi |

Donanım zamanlaması RPMsg trafiğinden etkilenmesin diye motor/servo daha yüksek öncelikte.

### Step motor (28BYJ-48, half-step)

```c
static const uint8_t half_step[8][4] = {
    {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
    {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1},
};
```

Dişli oranı 1:64, motor 64 half-step/tur → **4096 half-step = 1 tam tur**.
Varsayılan hız 2000 µs/adım (~8 s/tur). Hareket bitince fazlar bırakılır — boşta akım çekmez, ısınmaz.

### Servo (SG90, yazılım PWM)

```c
#define SERVO_PERIOD_US    20000    /* 50 Hz */
#define SERVO_MIN_US       500      /* 0° */
#define SERVO_MAX_US       2500     /* 180° */
```

Pulse `k_busy_wait()` ile üretilir — `k_usleep` kısa sürelerde tick granülaritesine takılır. Busy-wait CPU yakar ama sadece 0.5–2.5 ms, ve zamanlaması kesin.

**Neden donanım PWM değil:** Zephyr'de `j722s_main.dtsi` içinde **hiç PWM node'u yok** (sürücüler var: `pwm_ti_am3352_ehrpwm.c`, `pwm_ti_am3352_ecap.c`). Node'u elle tanımlamak + Linux'un tuttuğu PWM bloğunu koparmak gerekirdi. Yazılım PWM ile hemen çalışıyor.

---

## Sorun giderme

```bash
# Kanal açıldı mı? (virtio1 aranıyor, virtio0 değil)
sudo dmesg | grep -iE "virtio1|remoteproc3" | tail

# Cihaz doğru tipte mi?
ls -la /dev/rpmsg*            # 'c' ile başlamalı

# Donanım testi (Zephyr'siz)
sudo gpioset gpiochip3 17=1   # LED
sudo gpioset gpiochip2 33=1   # servo pini

# Bir Kconfig ayarı gerçekten etkin mi?
grep -E "^CONFIG_PINCTRL" ~/zephyrproject/zephyr/build/zephyr/.config

# Seri konsol
sudo picocom -b 115200 /dev/ttyUSB0
```

Beklenen boot çıktısı:
```
*** Booting Zephyr OS build v4.4.0-... ***
<inf> openamp_rsc_table: Starting application threads!
<inf> openamp_rsc_table: LED (GPIO6) hazir
<inf> openamp_rsc_table: motor pinleri hazir
<inf> openamp_rsc_table: servo pini hazir
<inf> openamp_rsc_table: OpenAMP[remote] Linux responder demo started
<dbg> platform_ipm_callback: msg received from mb 1
```

---

## Yol haritası

- [ ] **Ramp** — step motor hızlanma/yavaşlama eğrisi (adım kaçırmadan daha hızlı)
- [ ] **Jitter ölçümü** — yük altında zamanlama kararlılığı (A53 meşgulken R5 etkileniyor mu?)
- [ ] **Shared memory / zero-copy** — görüntü işleme için. Büyük buffer'lar RPMsg'den geçmez; `ddr0` bölgesini doğrudan kullanıp RPMsg'i sadece *"frame N hazır, şu adreste"* sinyali için kullanmak
- [ ] **Donanım PWM** — J722S PWM node'unu Zephyr'de tanımla (yüksek mikro-adım frekansları için)
- [ ] **Çoklu motor** — `id` alanı zaten protokolde var

---

## Referanslar

| | |
|---|---|
| Zephyr BeagleY-AI board | [docs.zephyrproject.org](https://docs.zephyrproject.org/latest/boards/beagle/beagley_ai/) |
| Zephyr J722S R5 desteği | [PR #80344](https://github.com/zephyrproject-rtos/zephyr/pull/80344) |
| TI AM67A | [ti.com/product/AM67A](https://www.ti.com/product/AM67A) |
| T3 Gemstone | [docs.t3gemstone.org](https://docs.t3gemstone.org) |
| Temel alınan örnek | `zephyr/samples/subsys/ipc/openamp_rsc_table` |
| TI MCU+ SDK (kavramsal) | [J722S API guide](https://software-dl.ti.com/jacinto7/esd/processor-sdk-rtos-j722s/) |

> Bu proje, [Zephyr'i R5 çekirdeğinde çalıştırma](https://github.com/MehmetEmreee/zephyr-t3gemstone-o1-r5f) çalışmasının devamıdır. Orada Zephyr remoteproc3 üzerinde bağımsız koşuyordu (hello_world, jitter demosu); burada Linux ile **çift yönlü haberleşme** kuruldu.
