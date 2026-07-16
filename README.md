<div align="center">

# T3-Gemstone-O1 Deterministik Motor Kontrolü

**Linux ↔ Zephyr AMP köprüsü — TI AM67A (J722S) üzerinde gerçek zamanlı donanım kontrolü**

Linux'tan komut ver, Cortex-R5F'te çalışan Zephyr donanımı deterministik zamanlamayla sürsün.

[![Platform](https://img.shields.io/badge/SoC-TI%20AM67A%20(J722S)-red)]()
[![Board](https://img.shields.io/badge/board-T3%20Gemstone%20O1-blue)]()
[![RTOS](https://img.shields.io/badge/RTOS-Zephyr%204.4-purple)]()
[![IPC](https://img.shields.io/badge/IPC-OpenAMP%20%2F%20RPMsg-green)]()

</div>

---

## Ne Yapar?

```bash
$ sudo ./led_ctrl servo 90
-> type=0x20 id=0 value=90
<- type=0x20 status=OK value=90

$ sudo ./led_ctrl step 4096      # 1 tam tur, komut hemen döner
-> type=0x10 id=0 value=4096
<- type=0x10 status=OK value=451  # motor hâlâ önceki komutu işliyor
```

Linux (A53) komutu verir ve işine döner. R5F (Zephyr) motoru kendi ritminde, kesintisiz sürer. Linux takılsa, yoğunlaşsa, hatta çökse bile motor döngüsü aksamaz.

AMP'nin bütün meselesi budur.

---

## İçindekiler

- [Neden AMP?](#neden-amp)
- [Mimari](#mimari)
- [Donanım](#donanım)
- [Hızlı Başlangıç](#hızlı-başlangıç)
- [Komut Protokolü](#komut-protokolü)
- [Yeni Komut Ekleme](#yeni-komut-ekleme)
- [Kritik Kconfig Ayarları](#kritik-kconfig-ayarları)
- [Tuzaklar ve Öğrenilen Dersler](#tuzaklar-ve-öğrenilen-dersler)
- [Dosya Yapısı](#dosya-yapısı)
- [Sorun Giderme](#sorun-giderme)
- [Yol Haritası](#yol-haritası)
- [Referanslar](#referanslar)

---

## Neden AMP?

> *"Linux tarafım yavaşken bir çekirdeğimi gerçek zamanlı çalıştırmak ne işe yarar? Bir insanın bir bacağı 100 km/s hızla giderken diğeri 60 km/s hızla gidiyorsa, dengeli hareket için 60'a inmek zorunda kalır."*

Bu sezgi, iki çekirdeğin aynı işi yaptığı ve birbirini beklediği varsayımına dayanır. AMP'nin tüm olayı bu varsayımı kırmaktır.

Doğru benzetme: **orkestra şefi ve metronom.**

| | A53 · Linux | R5F · Zephyr |
|---|---|---|
| **İyi olduğu şey** | Throughput — bol veriyi hızlı işlemek | Latency & determinizm — az veriyi tam zamanında işlemek |
| **Görevi** | Görüntü işleme, ağ, kütüphaneler, karar verme | Motor döngüsü, pals zamanlaması, refleks |
| **Ölçüsü** | Ortalama hız | En kötü durum gecikmesi |
| **Bir tur kaçırırsa** | Bir kare düşer, kimse fark etmez | Motor sarsılır, adım kaçar, sargı yanar |

Motor kontrolünde önemli olan ortalama değil, her seferinde randevuya uymaktır. Linux (PREEMPT_RT ile bile) çoğu zaman tutturur ama bir ağ kesmesi, bir sürücü kilidi, bir bellek işlemi araya girdiğinde milisaniyelerle gecikebilir. Ortalaması harikadır, en kötü durumu öngörülemez.

R5F'te araya girecek katman yoktur. Söz verdiği anda oradadır.

**Kritik nokta:** R5F "daha hızlı" değildir — A53 ham güçte kat kat üstündür. R5F güvenilirdir. İkisi birbirini beklemediği için sistemin toplam kalitesi ikisinin çarpımıdır, en yavaşına inen ortalaması değil.

---

## Mimari

### Katmanlar

Haberleşme tek bir API değil, üst üste oturan üç katmandır:

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
    │  Paylaşımlı bellek — DDR 0xa2000000 (ddr0, 1 MB)       │
    └────────────────────────────────────────────────────────┘
                              ▲
                   ┌──────────┴──────────┐
                   │  Mailbox (donanım)  │  ← kesme, polling yok
                   │  mbox3 (MAIN)       │
                   └─────────────────────┘
```

**Akış:** `write(/dev/rpmsg1, &cmd)` → rpmsg paketi paylaşımlı belleğe koyulur → mailbox R5'te kesme tetikler → Zephyr callback uyanır → `dispatch()` → `gpio_pin_set_dt()`

Mailbox kritiktir: R5 boşuna bellek yoklamaz, sadece iş geldiğinde uyanır.

### Çekirdek Haritası

| remoteproc | Adres | Tür | Durum |
|---|---|---|---|
| remoteproc0/1 | `7e000000/7e200000.dsp` | C7x DSP | ilgisiz |
| remoteproc2 | `79000000.r5f` | MCU-domain R5F | **dokunma** — TI ping-pong fw |
| **remoteproc3** | `78400000.r5f` | **MAIN-domain R5F** | ✅ **Zephyr burada** |

> **Uyarı:** `dmesg`'te `virtio0: rpmsg host is online` görüp sevinme — o remoteproc2'ye (MCU-R5) aittir ve açılıştan beri oradadır. Seninki virtio1'dir.

### TI MCU+ SDK ile İlişkisi

TI'ın MCU+ SDK dokümantasyonu kendi IPC yığınını anlatır (FreeRTOS/NORTOS üzerinde). Bizim R5'te Zephyr çalıştığı için o API'ler birebir kullanılmaz. Köprü, iki tarafın da konuştuğu ortak protokol olan RPMsg/virtio üzerinden kurulur.

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

### Pin Haritası

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

### Pin Kimlik Tablosu

Her pinin beş farklı ismi vardır. Tek güvenilir bağ: pinmux offset.

| İşlev | Header | Gemstone | Linux | TI pad | Zephyr node + offset | pinctrl |
|---|---|---|---|---|---|---|
| **LED** | 31 | GPIO-6 | `gpiochip3` 17 | `SPI0_CLK.GPIO1_17` | `main_gpio1_0` 17 | `hat_31_gpio` (`0x1bc`) |
| **IN1** | 11 | GPIO-17 | `gpiochip3` 8 | `MCASP0_AXR2.GPIO1_8` | `main_gpio1_0` 8 | `hat_11_gpio` |
| **IN2** | 29 | GPIO-5 | `gpiochip3` 15 | `SPI0_CS0.GPIO1_15` | `main_gpio1_0` 15 | `hat_29_gpio` |
| **IN3** | 36 | GPIO-16 | `gpiochip3` 7 | `MCASP0_AXR3.GPIO1_7` | `main_gpio1_0` 7 | `hat_36_gpio` |
| **IN4** | 35 | GPIO-19 | `gpiochip3` 12 | `MCASP0_AFSX.GPIO1_12` | `main_gpio1_0` 12 | `hat_35_gpio` |
| **Servo** | 13 | GPIO-27 | `gpiochip2` 33 | `GPMC0_OEn_REn.GPIO0_33` | `main_gpio0_1` **1** | `hat_13_gpio` (`0x088`) |

**Pinmux offset → PADCONFIG:** `offset / 4`. Örnek: `0x1bc / 4 = 111` → PADCONFIG111.

**Bank offset uyarısı:** J722S GPIO blokları Zephyr'de bölünmüştür. `main_gpio0_0` = line 0–31, `main_gpio0_1` = line 32–63 ama kendi içinde 0'dan sayar. Linux `gpiochip2` line 33 → Zephyr `main_gpio0_1` offset 33−32 = 1.

### Bağlantılar

| Bileşen | Bağlantı |
|---|---|
| **LED** | anot → 330Ω → pin 31, katot → GND |
| **28BYJ-48 + ULN2003** | IN1→11, IN2→29, IN3→36, IN4→35, VCC→2, GND→6 |
| **SG90 servo** | sinyal→13, 5V→4, GND→9 |

> ⚠️ **Besleme:** SG90 hareket anında 200–700 mA, 28BYJ-48 yaklaşık 200 mA çeker. İkisi birden kartın 5V pininden beslenirse gerilim düşer → kart sıfırlanır → SD kart bozulabilir. Ciddi kullanımda harici besleme (GND ortak) kullanın.

---

## Hızlı Başlangıç

### 1. Bir Kerelik Kurulum

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

Tek komut, yaklaşık 30 saniye bekleyin.

| Hedef | Ne Yapar? |
|---|---|
| `make sync` | repo → `~/zephyrproject/led_ipc` (build alanı) |
| `make build` | `west build -p always` |
| `make fw` | firmware'i karta atar |
| `make client` | sadece `led_ctrl` (reboot gerekmez) |
| `make deploy` | hepsi + reboot |
| `make clean` | build klasörünü siler |

> **Kaynak repo'da tutulur.** `~/zephyrproject/led_ipc` sadece build alanıdır — orada dosya düzenlemeyin, `make sync` üzerine yazar. Tersini yaparsanız repo eskir.

### 3. Kullanım

```bash
sudo ~/led_ctrl ping           # bağlantı testi → 0xABCD
sudo ~/led_ctrl led 1          # LED yak
sudo ~/led_ctrl ledget         # LED durumunu oku

sudo ~/led_ctrl step 4096      # 1 tam tur ileri (fire-and-forget)
sudo ~/led_ctrl step -512      # geri
sudo ~/led_ctrl speed 1500     # adım arası µs (min 1000)
sudo ~/led_ctrl mget           # pozisyon oku
sudo ~/led_ctrl mstop          # durdur

sudo ~/led_ctrl servo 90       # 0–180°
sudo ~/led_ctrl sget
sudo ~/led_ctrl soff           # palsı kes (akım çekmesin, ısınmasın)

sudo ~/led_ctrl raw 0x99 0 0   # bilinmeyen komut → RESP_ERR_CMD
```

### 4. Firmware'i Başlatma

```bash
sudo reboot
```

> **`echo stop > /sys/class/remoteproc/remoteproc3/state` çalışmaz.**
> ```
> k3_r5_rproc_stop: timeout waiting for rproc completion event
> remoteproc remoteproc3: can't stop rproc: -16
> ```
> Takılan çekirdek "durdum" onayı gönderemez. Sistem açılışta symlink'in gösterdiği firmware'i otomatik başlatır — dosyayı değiştirip reboot etmek yeterlidir. `stop` ile vakit kaybetmeyin.

---

## Komut Protokolü

Tek byte yerine yapılandırılmış mesaj. `ipc_proto.h` iki tarafta da aynı olmalıdır.

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

### Komut Tablosu

| Kod | Komut | `value` | Açıklama |
|---|---|---|---|
| `0x00` | `CMD_PING` | — | → `0xABCD` |
| `0x01` | `CMD_LED_SET` | 0/1 | LED |
| `0x02` | `CMD_LED_GET` | — | ← durum |
| `0x10` | `CMD_MOTOR_STEP` | ±adım | 4096 = 1 tur, bloklamaz |
| `0x11` | `CMD_MOTOR_STOP` | — | |
| `0x12` | `CMD_MOTOR_GET` | — | ← pozisyon |
| `0x13` | `CMD_MOTOR_SPD` | µs | adım arası, 1000–100000 |
| `0x20` | `CMD_SERVO_SET` | 0–180 | derece |
| `0x21` | `CMD_SERVO_GET` | — | ← açı (−1 = kapalı) |
| `0x22` | `CMD_SERVO_OFF` | — | palsı kes |
| `0x30` | `CMD_STATUS_GET` | — | (ayrılmış) |

### Cevap Kodları

| Kod | Anlam |
|---|---|
| `RESP_OK` | tamam |
| `RESP_ERR_CMD` | bilinmeyen komut |
| `RESP_ERR_ID` | geçersiz id |
| `RESP_ERR_VALUE` | geçersiz değer |
| `RESP_ERR_VER` | protokol sürümü uyuşmuyor |

### Linux Tarafı: Endpoint Açma

`rpmsg_tty` modülü bu kernel'de yok (`modprobe: FATAL: Module rpmsg_tty not found`), ama `rpmsg_char` + `rpmsg_ctrl` var. Kanallar duyurulur ama `/dev/rpmsgX` otomatik oluşmaz — `/dev/rpmsg_ctrl1` üzerinden ioctl ile açılır:

```c
struct rpmsg_endpoint_info ept = {0};
strncpy(ept.name, "rpmsg-client-sample", sizeof(ept.name) - 1);
ept.src = 0xFFFFFFFF;
ept.dst = 0x400;
ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept);   /* → /dev/rpmsg1 belirir */
```

`led_ctrl` bunu kendi yapar — reboot sonrası tek komut yeterlidir, manuel adım gerekmez.

---

## Yeni Komut Ekleme

Üç satır. Komut tablosu (dispatch) sayesinde `if/else` çorbası yok.

**1.** `ipc_proto.h` — enum'a ekleyin:
```c
CMD_MOTOR_HOME = 0x14,
```

**2.** `main_remote.c` — handler yazın:
```c
static int handle_motor_home(const struct ipc_cmd *cmd, struct ipc_resp *resp)
{
    motor_target = -motor_position;
    k_sem_give(&motor_sem);
    resp->value = 0;
    return RESP_OK;
}
```

**3.** `main_remote.c` — tabloya ekleyin:
```c
static const struct cmd_entry cmd_table[] = {
    ...
    { CMD_MOTOR_HOME, handle_motor_home },
};
```

> Handler'lar `cmd_table`'dan önce tanımlanmalıdır.

---

## Kritik Kconfig Ayarları

| Ayar | Neden Şart? |
|---|---|
| `CONFIG_PINCTRL=y` | Yoksa overlay'deki `pinctrl-0` sessizce yok sayılır → konsol "LED ON" der ama LED yanmaz |
| `CONFIG_LOG_BACKEND_UART=y` | `CONFIG_UART_CONSOLE=y` sadece `printk`'i yönlendirir; `LOG_INF` ayrı backend ister |
| `CONFIG_SHELL=n` | Orijinal örnek konsolu RPMsg'e bağlar → RPMsg açılamazsa firmware tamamen sessiz kalır |
| `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` | Servo pals periyodu için. Bkz. tick tuzağı |
| `CONFIG_GPIO=y` | |

**Kullanmayın:** `CONFIG_LOG_MODE_MINIMAL` — backend'i devre dışı bırakır, çıktı kaybolur.

**Kullanmayın:** `CONFIG_OPENAMP_WITH_DCACHE=y` — bu SoC'te etkisiz, aşağıya bakın.

---

## Tuzaklar ve Öğrenilen Dersler

### Sinsi Tuzak: Linux'un Pinmux'unu Miras Almak

**Belirti:** Zephyr çalışır, konsol `LED ON` der, LED yanmaz. Sonra Linux'tan bir kez `sudo gpioset gpiochip3 17=1` çalıştırırsınız — o andan itibaren Zephyr'in kontrolü çalışmaya başlar. Reboot → yine çalışmaz.

**Sebep:** `CONFIG_PINCTRL=y` yok. Overlay'deki `pinctrl-0` sessizce yok sayılır — derleme hatası bile vermez. Zephyr padi hiç ayarlamaz. `gpioset` padi MUX_MODE_7'ye alır ve ayar kalıcı olduğu için Zephyr "sanki çalışıyormuş gibi" görünür.

**Ders:** *Bir şey ancak "başka bir şey önce yapılırsa" çalışıyorsa, o şey sizin kodunuz değildir.* Reboot sonrası hiçbir Linux GPIO komutu çalıştırmadan test edin.

### `CONFIG_OPENAMP_WITH_DCACHE` Bu SoC'te Çalışmaz

Diğer tüm board overlay'lerinde (imx8mp, imx93, stm32mp...) var, o yüzden bizde de eksik sanıp ekledik. Etkisiz:

```
warning: OPENAMP_WITH_DCACHE was assigned the value 'y' but got the value 'n'.
Check these unsatisfied dependencies: CACHE_MANAGEMENT (=n)
```

Zincir: `OPENAMP_WITH_DCACHE` → `CACHE_MANAGEMENT` → `depends on DCACHE || ICACHE`.
`.config`'te `DCACHE=y`, `ICACHE=y`, `CPU_HAS_DCACHE=y` var ama `CACHE_MANAGEMENT` yine `n` — TI J722S R5F portu cache management backend'ini implemente etmemiş. `CONFIG_CACHE=y` / `CONFIG_CACHE_MANAGEMENT=y` eklemek de işe yaramıyor.

**Ders:** Build çıktısını `tail -25` ile izlerseniz bu uyarıyı göremezsiniz — Kconfig uyarıları en başta çıkar. Bir ayarın gerçekten etkin olduğunu doğrulayın:
```bash
grep -E "^CONFIG_PINCTRL" ~/zephyrproject/zephyr/build/zephyr/.config
```

### İlk Takılmayı Ne Çözdü? (Dürüst Cevap: Bilmiyoruz)

İlk denemede firmware açıldı, `remote processor is now up` dedi, ama `rpmsg host is online` hiç gelmedi. Sonraki turda aynı anda birden fazla şey değiştirdik ve çalıştı:

- Konsolu UART'a aldık
- RPMsg shell'i kapattık (`CONFIG_SHELL=n`)
- Stack/heap ile oynadık
- `CONFIG_OPENAMP_WITH_DCACHE=y` ekledik (sonradan etkisiz olduğu anlaşıldı)

Hangisinin çözdüğü kesin bilinmiyor. En olası şüpheli: shell'in RPMsg'e bağlanması kanal kurulumunda çakışma yaratıyordu.

**Ders:** Aynı anda birden fazla değişkeni değiştirmeyin. Yoksa "ne çözdü" sorusunun cevabı kaybolur.

### Firmware'i Önce Konuşturun

Orijinal örnekte `CONFIG_PRINTK=n` + konsol RPMsg'de. RPMsg açılamayınca firmware tamamen sessiz — nerede takıldığını görmek imkânsız. İlk iş: konsolu UART'a alın, sonra hata ayıklayın.

### Servo Tık Tık Eder — Tick Granülaritesi

**Belirti:** Servo boşta dururken sürekli mikro düzeltme yapar, tık tık ses çıkarır.

**Sebep:** Zephyr'in varsayılan tick'i 100 Hz = 10 ms granülarite. `k_usleep(18500)` dediğinizde çekirdek sadece tick sınırlarında uyanabilir → periyot 20 ms yerine 11.5–21.5 ms arası zıplar. Pals genişliği doğru (`k_busy_wait` tick'e bağlı değil) ama palslar arası mesafe oynak → servo tereddüt eder.

**Çözüm:** `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` → granülarite 100 µs.

**Bedeli:** Saniyede 10.000 tick kesmesi ≈ CPU'nun %1–2'si. R5 için ihmal edilebilir.

**Kazancı:** Tüm `k_usleep`/`k_sleep` çağrıları 100 µs hassasiyetinde — step motorun zamanlaması da düzeldi.

### SG90 Pals Aralığı 500–2500 µs

Yaygın bilgi 1000–2000 µs der, ama çoğu SG90 klonu 500–2500 µs ister. 1000–2000 kullanırsanız servo mekanik aralığın ancak yarısını kullanır — "açılar ikiye bölünmüş gibi" hissi oluşur.

> Bazı klonlar 500 µs'de mekanik sınıra dayanır ve inler. Ses duyarsanız `soff` yapın, aralığı daraltın (600–2400).

### `tee` ile /dev/rpmsg1'e Yazma

```bash
echo -n "1" | sudo tee /dev/rpmsg1     # cihaz YOKSA düz dosya oluşturur!
```

`/dev/rpmsg1` reboot sonrası kaybolur. `tee` cihaz yoksa normal dosya yaratır, yazma "başarılı" görünür ama mesaj hiçbir yere gitmez. O dosya diskte kalır, sonraki denemeleri de bozar.

```bash
$ ls -la /dev/rpmsg*
crw------- ... /dev/rpmsg0     # 'c' = karakter cihazı ✓
-rw-r--r-- ... /dev/rpmsg1     # '-' = düz dosya ✗
```

`led_ctrl`'deki `S_ISCHR()` kontrolü bunu otomatik yakalar.

### virtio0 vs virtio1

`dmesg`'te `virtio0: rpmsg host is online` görüp sevinmeyin — o MCU-R5'e (remoteproc2, TI ping-pong fw) aittir, açılıştan beri oradadır. Sizinki virtio1'dir:

```bash
ls -la /sys/bus/rpmsg/devices/
# .../remoteproc3/rproc-virtio.7.auto/virtio1/virtio1.rpmsg-client-sample.-1.1024
```

### Overlay'de `#include` Unutmayın

```
devicetree error: ...overlay:5 (column 30): parse error: expected number or parenthesized expression
```

`GPIO_ACTIVE_HIGH` bir dt-binding sabitidir. En üste: `#include <zephyr/dt-bindings/gpio/gpio.h>`

### GPIO Node'ları Varsayılan Kapalıdır

J722S'te `dts/arm/ti/j722s_main.dtsi` içinde tüm GPIO node'ları `status = "disabled"` durumundadır. Overlay'de açmanız şart:
```
&main_gpio1_0 { status = "okay"; };
```

### Adresler Zaten Hizalı — Boş Yere Aramayın

Zephyr board tanımı `ddr0 @ 0xa2000000` der, Linux `main-r5fss-dma-memory-region@a2000000` atar. Birebir uyuşur — resmi board tanımı bunu halletmiştir.

### Kaynak Tek Yerde Olsun

Kaynak repo'da, `make sync` build alanına kopyalar. Tersini yaparsanız (build alanında düzenleme → repo'ya kopyalama) er geç repo eskir. Bir kez başımıza geldi: `CONFIG_PINCTRL` repo'ya geçmemişti, saatler kaybettirdi.

### Echo Testi Yanıltıcıdır

`timeout 2 cat /dev/rpmsg1` boş dönebilir ama mesaj yine de ulaşmış olabilir. Kesin kanıt seri konsoldur.

---

## Dosya Yapısı

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

### Thread Yapısı ve Öncelikler

| Thread | Öncelik | Görev |
|---|---|---|
| `servo_thread` | `K_PRIO_COOP(4)` | 50 Hz pals — en kritik zamanlama |
| `motor_thread` | `K_PRIO_COOP(5)` | half-step faz dizisi |
| `app_rpmsg_client_sample` | `K_PRIO_COOP(7)` | endpoint kurulumu |
| `app_rpmsg_tty` | `K_PRIO_COOP(7)` | (kullanılmıyor) |
| `rpmsg_mng_task` | `K_PRIO_COOP(8)` | virtio/mailbox yönetimi |

Donanım zamanlaması RPMsg trafiğinden etkilenmesin diye motor/servo daha yüksek önceliktedir.

### Step Motor (28BYJ-48, half-step)

```c
static const uint8_t half_step[8][4] = {
    {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
    {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1},
};
```

Dişli oranı 1:64, motor 64 half-step/tur → 4096 half-step = 1 tam tur.
Varsayılan hız 2000 µs/adım (~8 s/tur). Hareket bitince fazlar bırakılır — boşta akım çekmez, ısınmaz.

### Servo (SG90, yazılım PWM)

```c
#define SERVO_PERIOD_US    20000    /* 50 Hz */
#define SERVO_MIN_US       500      /* 0° */
#define SERVO_MAX_US       2500     /* 180° */
```

Pals `k_busy_wait()` ile üretilir — `k_usleep` kısa sürelerde tick granülaritesine takılır. Busy-wait CPU yakar ama sadece 0.5–2.5 ms, ve zamanlaması kesindir.

**Neden donanım PWM değil:** Zephyr'de `j722s_main.dtsi` içinde hiç PWM node'u yok (sürücüler var: `pwm_ti_am3352_ehrpwm.c`, `pwm_ti_am3352_ecap.c`). Node'u elle tanımlamak + Linux'un tuttuğu PWM bloğunu koparmak gerekirdi. Yazılım PWM ile hemen çalışıyor.

---

## Sorun Giderme

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

Beklenen açılış çıktısı:
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

## Yol Haritası

- [ ] **Ramp** — step motor hızlanma/yavaşlama eğrisi (adım kaçırmadan daha hızlı)
- [ ] **Jitter ölçümü** — yük altında zamanlama kararlılığı (A53 meşgulken R5 etkileniyor mu?)
- [ ] **Shared memory / zero-copy** — görüntü işleme için. Büyük buffer'lar RPMsg'den geçmez; `ddr0` bölgesini doğrudan kullanıp RPMsg'i sadece "frame N hazır, şu adreste" sinyali için kullanmak
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

> Bu proje, [Zephyr'i R5 çekirdeğinde çalıştırma](https://github.com/MehmetEmreee/zephyr-t3gemstone-o1-r5f) çalışmasının devamıdır. Orada Zephyr remoteproc3 üzerinde bağımsız çalışıyordu (hello_world, jitter demosu); burada Linux ile çift yönlü haberleşme kuruldu.

---

# T3-Gemstone-O1 Deterministic Motor Control

**Linux ↔ Zephyr AMP bridge — Real-time hardware control on TI AM67A (J722S)**

Command from Linux, drive hardware with deterministic timing on Cortex-R5F running Zephyr.

[![Platform](https://img.shields.io/badge/SoC-TI%20AM67A%20(J722S)-red)]()
[![Board](https://img.shields.io/badge/board-T3%20Gemstone%20O1-blue)]()
[![RTOS](https://img.shields.io/badge/RTOS-Zephyr%204.4-purple)]()
[![IPC](https://img.shields.io/badge/IPC-OpenAMP%20%2F%20RPMsg-green)]()

</div>

---

## What It Does

```bash
$ sudo ./led_ctrl servo 90
-> type=0x20 id=0 value=90
<- type=0x20 status=OK value=90

$ sudo ./led_ctrl step 4096      # 1 full rotation, returns immediately
-> type=0x10 id=0 value=4096
<- type=0x10 status=OK value=451  # motor still processing previous command
```

Linux (A53) issues the command and moves on. R5F (Zephyr) drives the motor at its own pace, uninterrupted. Even if Linux hangs, gets busy, or crashes, the motor loop stays solid.

That's the whole point of AMP.

---

## Table of Contents

- [Why AMP?](#why-amp)
- [Architecture](#architecture)
- [Hardware](#hardware)
- [Quick Start](#quick-start)
- [Command Protocol](#command-protocol)
- [Adding New Commands](#adding-new-commands)
- [Critical Kconfig Settings](#critical-kconfig-settings)
- [Pitfalls & Lessons Learned](#pitfalls--lessons-learned)
- [File Structure](#file-structure)
- [Troubleshooting](#troubleshooting)
- [Roadmap](#roadmap)
- [References](#references)

---

## Why AMP?

> *"If my Linux side is slow, what good is running one core real-time? If a person has one leg going 100 km/h and the other 60 km/h, they must drop to 60 for stable movement."*

This intuition assumes both cores do the same work and wait for each other. AMP's entire purpose is to break this assumption.

The right analogy: **conductor and metronome.**

| | A53 · Linux | R5F · Zephyr |
|---|---|---|
| **Best at** | Throughput — processing lots of data fast | Latency & determinism — processing little data on time |
| **Job** | Vision, networking, libraries, decisions | Motor loop, pulse timing, reflexes |
| **Measured by** | Average speed | Worst-case latency |
| **Misses a cycle** | A frame drops, nobody notices | Motor jerks, step lost, winding burns |

In motor control, what matters isn't the average — it's meeting the deadline every single time. Linux (even with PREEMPT_RT) usually does, but a network interrupt, a driver lock, a memory operation can delay it by milliseconds. Average is great, worst case is unpredictable.

On R5F there are no layers to get in the way. It's there exactly when promised.

**Key point:** R5F isn't "faster" — A53 is far superior in raw power. R5F is reliable. And because the two don't wait for each other, total system quality is their product, not an average dragged down by the slowest.

---

## Architecture

### Layers

Communication isn't a single API but three stacked layers:

```
┌─────────────────────┐                    ┌─────────────────────┐
│    A53 · Linux      │                    │    R5F · Zephyr     │
│  echo/led_ctrl      │                    │  rpmsg callback     │
└──────────┬──────────┘                    └──────────▲──────────┘
           │                                          │
    ┌──────▼──────────────────────────────────────────┴──────┐
    │  RPMsg — addressed channels (endpoints), messaging     │
    │  rpmsg-client-sample @ 0x400                           │
    ├────────────────────────────────────────────────────────┤
    │  virtio / vring — ring buffers, buffer contract        │
    │  virtio1 (under remoteproc3)                           │
    ├────────────────────────────────────────────────────────┤
    │  Shared memory — DDR 0xa2000000 (ddr0, 1 MB)           │
    └────────────────────────────────────────────────────────┘
                              ▲
                   ┌──────────┴──────────┐
                   │  Mailbox (hardware) │  ← interrupt, no polling
                   │  mbox3 (MAIN)       │
                   └─────────────────────┘
```

**Flow:** `write(/dev/rpmsg1, &cmd)` → rpmsg packet placed in shared memory → mailbox triggers interrupt on R5 → Zephyr callback wakes → `dispatch()` → `gpio_pin_set_dt()`

Mailbox is critical: R5 doesn't waste time polling memory, it wakes only when work arrives.

### Core Map

| remoteproc | Address | Type | Status |
|---|---|---|---|
| remoteproc0/1 | `7e000000/7e200000.dsp` | C7x DSP | irrelevant |
| remoteproc2 | `79000000.r5f` | MCU-domain R5F | **don't touch** — TI ping-pong fw |
| **remoteproc3** | `78400000.r5f` | **MAIN-domain R5F** | ✅ **Zephyr runs here** |

> **Warning:** Don't get excited seeing `virtio0: rpmsg host is online` in `dmesg` — that's remoteproc2 (MCU-R5), there since boot. Yours is virtio1.

### Relationship with TI MCU+ SDK

TI's MCU+ SDK documentation describes its own IPC stack (on FreeRTOS/NORTOS). Since we run Zephyr on R5, those APIs aren't used directly. The bridge is built on the common protocol both sides speak: RPMsg/virtio.

Concepts map one-to-one:

| TI MCU+ SDK | Our equivalent |
|---|---|
| IPC Notify | Mailbox (`mbox3`) |
| IPC RPMessage | RPMsg endpoint |
| Shared memory | `ddr0` @ `0xa2000000` |

---

## Hardware

| | |
|---|---|
| Board | T3 Gemstone O1 |
| SoC | TI AM67A (= J722S, Jacinto 7) |
| Linux | 4× Cortex-A53, kernel `6.12.24-ti`, PREEMPT_RT |
| Zephyr | MAIN-domain Cortex-R5F, remoteproc3 |
| Zephyr board | `beagley_ai/j722s/main_r5f0_0` |
| Host | Ubuntu 24.04 (aarch64) |
| Serial console | UART1 `0x2810000` — pin 8 (TX) / 10 (RX) / 6 (GND), **115200 8N1** |

### Pin Map

```
                    T3 Gemstone O1 — 40 pin header
        ┌───────────────────────────────────────────────┐
   3v3  │  1   2  │ 5V ──────────── ULN2003 VCC         │
        │  3   4  │ 5V ──────────── Servo red           │
        │  5   6  │ GND ─────────── ULN2003 GND         │
        │  7   8  │ ← UART1 TX (Zephyr console)         │
        │  9  10  │ ← UART1 RX                          │
IN1 ────│ 11  12  │                                     │
Servo ──│ 13  14  │ GND                                 │
        │ 15  16  │                                     │
        │ ...     │                                     │
        │ 29  30  │ ← IN2                               │
LED ────│ 31  32  │ (PWM-ECAP0 — don't use)             │
        │ 33  34  │ (PWM-1B — don't use)                │
IN4 ────│ 35  36  │ ← IN3                               │
        │ 37  38  │                                     │
        │ 39  40  │                                     │
        └───────────────────────────────────────────────┘
```

### Pin Identity Table

Every pin has five different names. The only reliable link: pinmux offset.

| Function | Header | Gemstone | Linux | TI pad | Zephyr node + offset | pinctrl |
|---|---|---|---|---|---|---|
| **LED** | 31 | GPIO-6 | `gpiochip3` 17 | `SPI0_CLK.GPIO1_17` | `main_gpio1_0` 17 | `hat_31_gpio` (`0x1bc`) |
| **IN1** | 11 | GPIO-17 | `gpiochip3` 8 | `MCASP0_AXR2.GPIO1_8` | `main_gpio1_0` 8 | `hat_11_gpio` |
| **IN2** | 29 | GPIO-5 | `gpiochip3` 15 | `SPI0_CS0.GPIO1_15` | `main_gpio1_0` 15 | `hat_29_gpio` |
| **IN3** | 36 | GPIO-16 | `gpiochip3` 7 | `MCASP0_AXR3.GPIO1_7` | `main_gpio1_0` 7 | `hat_36_gpio` |
| **IN4** | 35 | GPIO-19 | `gpiochip3` 12 | `MCASP0_AFSX.GPIO1_12` | `main_gpio1_0` 12 | `hat_35_gpio` |
| **Servo** | 13 | GPIO-27 | `gpiochip2` 33 | `GPMC0_OEn_REn.GPIO0_33` | `main_gpio0_1` **1** | `hat_13_gpio` (`0x088`) |

**Pinmux offset → PADCONFIG:** `offset / 4`. Example: `0x1bc / 4 = 111` → PADCONFIG111.

**Bank offset warning:** J722S GPIO blocks are split in Zephyr. `main_gpio0_0` = line 0–31, `main_gpio0_1` = line 32–63 but counts from 0 internally. Linux `gpiochip2` line 33 → Zephyr `main_gpio0_1` offset 33−32 = 1.

### Connections

| Component | Connection |
|---|---|
| **LED** | anode → 330Ω → pin 31, cathode → GND |
| **28BYJ-48 + ULN2003** | IN1→11, IN2→29, IN3→36, IN4→35, VCC→2, GND→6 |
| **SG90 servo** | signal→13, 5V→4, GND→9 |

> ⚠️ **Power:** SG90 draws 200–700 mA during movement, 28BYJ-48 draws ~200 mA. If both are powered from the board's 5V pin, voltage drops → board resets → SD card may corrupt. For serious use, use external power (common GND).

---

## Quick Start

### 1. One-Time Setup

```bash
# SSH key (host → board)
ssh-copy-id gemstone@192.168.7.2

# Passwordless deploy (on board) — sudo can't ask for password over SSH
echo "gemstone ALL=(ALL) NOPASSWD: /bin/cp /tmp/zephyr.elf /lib/firmware/zephyr.elf, /sbin/reboot" \
  | sudo tee /etc/sudoers.d/rpmsg-deploy
sudo chmod 440 /etc/sudoers.d/rpmsg-deploy
```

### 2. Deploy

```bash
make deploy      # sync + build + push firmware + push client + reboot
```

Single command, wait ~30 seconds.

| Target | What it does |
|---|---|
| `make sync` | repo → `~/zephyrproject/led_ipc` (build area) |
| `make build` | `west build -p always` |
| `make fw` | push firmware to board |
| `make client` | only `led_ctrl` (no reboot needed) |
| `make deploy` | all + reboot |
| `make clean` | delete build directory |

> **Source lives in the repo.** `~/zephyrproject/led_ipc` is just the build area — don't edit files there, `make sync` will overwrite them. Do it the other way and the repo goes stale.

### 3. Usage

```bash
sudo ~/led_ctrl ping           # connectivity test → 0xABCD
sudo ~/led_ctrl led 1          # LED on
sudo ~/led_ctrl ledget         # read LED state

sudo ~/led_ctrl step 4096      # 1 full rotation forward (fire-and-forget)
sudo ~/led_ctrl step -512      # reverse
sudo ~/led_ctrl speed 1500     # step interval µs (min 1000)
sudo ~/led_ctrl mget           # read position
sudo ~/led_ctrl mstop          # stop

sudo ~/led_ctrl servo 90       # 0–180°
sudo ~/led_ctrl sget
sudo ~/led_ctrl soff           # cut pulse (stop drawing current/heat)

sudo ~/led_ctrl raw 0x99 0 0   # unknown command → RESP_ERR_CMD
```

### 4. Starting Firmware

```bash
sudo reboot
```

> **`echo stop > /sys/class/remoteproc/remoteproc3/state` DOES NOT WORK.**
> ```
> k3_r5_rproc_stop: timeout waiting for rproc completion event
> remoteproc remoteproc3: can't stop rproc: -16
> ```
> A hung core can't send "I stopped" acknowledgment. The system auto-starts the firmware the symlink points to at boot — change the file + reboot is enough. Don't waste time with `stop`.

---

## Command Protocol

Structured messages instead of single bytes. `ipc_proto.h` must be identical on both sides.

```c
struct ipc_cmd {          /* Linux → R5 */
    uint8_t  version;     /* IPC_PROTO_VERSION */
    uint8_t  type;        /* enum ipc_cmd_type */
    uint8_t  id;          /* device number */
    uint8_t  _pad;
    int32_t  value;
} __attribute__((packed));

struct ipc_resp {         /* R5 → Linux */
    uint8_t  version;
    uint8_t  type;        /* which command this responds to */
    uint8_t  status;      /* enum ipc_resp_status */
    uint8_t  _pad;
    int32_t  value;
} __attribute__((packed));
```

**Why `packed` is essential:** Both sides use different compilers (host `gcc` vs `arm-zephyr-eabi-gcc`). Without packed, one may insert padding bytes, sizes won't match, data gets corrupted.

**Why `version` exists:** If the protocol changes, old firmware returns `RESP_ERR_VER` instead of silently misbehaving.

### Command Table

| Code | Command | `value` | Description |
|---|---|---|---|
| `0x00` | `CMD_PING` | — | → `0xABCD` |
| `0x01` | `CMD_LED_SET` | 0/1 | LED |
| `0x02` | `CMD_LED_GET` | — | ← state |
| `0x10` | `CMD_MOTOR_STEP` | ±steps | 4096 = 1 rotation, non-blocking |
| `0x11` | `CMD_MOTOR_STOP` | — | |
| `0x12` | `CMD_MOTOR_GET` | — | ← position |
| `0x13` | `CMD_MOTOR_SPD` | µs | step interval, 1000–100000 |
| `0x20` | `CMD_SERVO_SET` | 0–180 | degrees |
| `0x21` | `CMD_SERVO_GET` | — | ← angle (−1 = off) |
| `0x22` | `CMD_SERVO_OFF` | — | cut pulse |
| `0x30` | `CMD_STATUS_GET` | — | (reserved) |

### Response Codes

| Code | Meaning |
|---|---|
| `RESP_OK` | success |
| `RESP_ERR_CMD` | unknown command |
| `RESP_ERR_ID` | invalid id |
| `RESP_ERR_VALUE` | invalid value |
| `RESP_ERR_VER` | protocol version mismatch |

### Linux Side: Opening an Endpoint

`rpmsg_tty` module is absent in this kernel (`modprobe: FATAL: Module rpmsg_tty not found`), but `rpmsg_char` + `rpmsg_ctrl` exist. Channels are announced but `/dev/rpmsgX` is not auto-created — open via ioctl on `/dev/rpmsg_ctrl1`:

```c
struct rpmsg_endpoint_info ept = {0};
strncpy(ept.name, "rpmsg-client-sample", sizeof(ept.name) - 1);
ept.src = 0xFFFFFFFF;
ept.dst = 0x400;
ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept);   /* → /dev/rpmsg1 appears */
```

`led_ctrl` does this itself — a single command after reboot is all you need, no manual steps.

---

## Adding New Commands

Three lines. Thanks to the dispatch table, no `if/else` soup.

**1.** `ipc_proto.h` — add to enum:
```c
CMD_MOTOR_HOME = 0x14,
```

**2.** `main_remote.c` — write handler:
```c
static int handle_motor_home(const struct ipc_cmd *cmd, struct ipc_resp *resp)
{
    motor_target = -motor_position;
    k_sem_give(&motor_sem);
    resp->value = 0;
    return RESP_OK;
}
```

**3.** `main_remote.c` — add to table:
```c
static const struct cmd_entry cmd_table[] = {
    ...
    { CMD_MOTOR_HOME, handle_motor_home },
};
```

> Handlers must be defined before `cmd_table`.

---

## Critical Kconfig Settings

| Setting | Why it's essential |
|---|---|
| `CONFIG_PINCTRL=y` | Without it, `pinctrl-0` in overlay is silently ignored → console says "LED ON" but LED stays dark |
| `CONFIG_LOG_BACKEND_UART=y` | `CONFIG_UART_CONSOLE=y` only routes `printk`; `LOG_INF` needs separate backend |
| `CONFIG_SHELL=n` | Original sample binds console to RPMsg → if RPMsg doesn't open, firmware goes completely silent |
| `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` | For servo pulse period. See tick trap |
| `CONFIG_GPIO=y` | |

**Don't use:** `CONFIG_LOG_MODE_MINIMAL` — disables backend, output vanishes.

**Don't use:** `CONFIG_OPENAMP_WITH_DCACHE=y` — ineffective on this SoC, see below.

---

## Pitfalls & Lessons Learned

### Sneaky Trap: Inheriting Linux's Pinmux

**Symptom:** Zephyr runs, console says `LED ON`, LED stays off. Then you run `sudo gpioset gpiochip3 17=1` once from Linux — from that moment, Zephyr's control starts working. Reboot → broken again.

**Cause:** Missing `CONFIG_PINCTRL=y`. Overlay's `pinctrl-0` is silently ignored — doesn't even produce a build error. Zephyr never configures the pad. `gpioset` sets the pad to MUX_MODE_7, and since the setting is persistent, Zephyr "appears to work."

**Lesson:** *If something only works when "something else is done first," that thing isn't your code.* Test after reboot without running any Linux GPIO commands.

### `CONFIG_OPENAMP_WITH_DCACHE` Doesn't Work on This SoC

Every other board overlay has it (imx8mp, imx93, stm32mp...), so we thought it was missing and added it. Ineffective:

```
warning: OPENAMP_WITH_DCACHE was assigned the value 'y' but got the value 'n'.
Check these unsatisfied dependencies: CACHE_MANAGEMENT (=n)
```

Chain: `OPENAMP_WITH_DCACHE` → `CACHE_MANAGEMENT` → `depends on DCACHE || ICACHE`.
`.config` has `DCACHE=y`, `ICACHE=y`, `CPU_HAS_DCACHE=y` but `CACHE_MANAGEMENT` is still `n` — TI J722S R5F port hasn't implemented the cache management backend. Adding `CONFIG_CACHE=y` / `CONFIG_CACHE_MANAGEMENT=y` doesn't help either.

**Lesson:** Watching build output with `tail -25` won't show this warning — Kconfig warnings appear at the very beginning. Verify a setting is actually active:
```bash
grep -E "^CONFIG_PINCTRL" ~/zephyrproject/zephyr/build/zephyr/.config
```

### What Fixed the First Hang? (Honest Answer: We Don't Know)

First attempt: firmware booted, `remote processor is now up` appeared, but `rpmsg host is online` never came. Next round, we changed multiple things at once and it worked:

- Moved console to UART
- Disabled RPMsg shell (`CONFIG_SHELL=n`)
- Adjusted stack/heap
- Added `CONFIG_OPENAMP_WITH_DCACHE=y` (later proven ineffective)

Which one fixed it is not definitively known. Most likely suspect: shell binding to RPMsg was creating a conflict during channel setup.

**Lesson:** Don't change multiple variables at once. Otherwise the answer to "what fixed it" is lost.

### Make Firmware Talk First

Original sample has `CONFIG_PRINTK=n` + console on RPMsg. When RPMsg doesn't open, firmware is completely silent — impossible to see where it's stuck. First order of business: move console to UART, then debug.

### Servo Tick-Tick-Tick — Tick Granularity

**Symptom:** Servo constantly micro-corrects while idle, making ticking sounds.

**Cause:** Zephyr's default tick is 100 Hz = 10 ms granularity. When you call `k_usleep(18500)`, the kernel can only wake at tick boundaries → period jumps between 11.5–21.5 ms instead of steady 20 ms. Pulse width is correct (`k_busy_wait` isn't tick-dependent) but inter-pulse spacing is erratic → servo hesitates.

**Fix:** `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` → granularity 100 µs.

**Cost:** 10,000 tick interrupts per second ≈ 1–2% CPU. Negligible for R5.

**Gain:** All `k_usleep`/`k_sleep` calls at 100 µs precision — step motor timing also improved.

### SG90 Pulse Range 500–2500 µs

Common knowledge says 1000–2000 µs, but most SG90 clones need 500–2500 µs. Using 1000–2000 means the servo only uses half its mechanical range — angles feel "halved."

> Some clones hit the mechanical limit and whine at 500 µs. If you hear noise, `soff` and narrow the range (600–2400).

### Writing to /dev/rpmsg1 with `tee`

```bash
echo -n "1" | sudo tee /dev/rpmsg1     # creates regular file if device MISSING!
```

`/dev/rpmsg1` disappears after reboot. `tee` creates a normal file if the device doesn't exist, writes "succeed" but the message goes nowhere. That file stays on disk and breaks future attempts.

```bash
$ ls -la /dev/rpmsg*
crw------- ... /dev/rpmsg0     # 'c' = character device ✓
-rw-r--r-- ... /dev/rpmsg1     # '-' = regular file ✗
```

`led_ctrl`'s `S_ISCHR()` check catches this automatically.

### virtio0 vs virtio1

Don't get excited seeing `virtio0: rpmsg host is online` in `dmesg` — that's MCU-R5 (remoteproc2, TI ping-pong fw), there since boot. Yours is virtio1:

```bash
ls -la /sys/bus/rpmsg/devices/
# .../remoteproc3/rproc-virtio.7.auto/virtio1/virtio1.rpmsg-client-sample.-1.1024
```

### Don't Forget `#include` in Overlay

```
devicetree error: ...overlay:5 (column 30): parse error: expected number or parenthesized expression
```

`GPIO_ACTIVE_HIGH` is a dt-binding constant. At the top: `#include <zephyr/dt-bindings/gpio/gpio.h>`

### GPIO Nodes Are Disabled by Default

In J722S's `dts/arm/ti/j722s_main.dtsi`, all GPIO nodes are `status = "disabled"`. You must enable them in the overlay:
```
&main_gpio1_0 { status = "okay"; };
```

### Addresses Already Aligned — Don't Hunt Blindly

Zephyr board definition says `ddr0 @ 0xa2000000`, Linux assigns `main-r5fss-dma-memory-region@a2000000`. They match exactly — the official board definition has already handled this.

### Source in One Place

Source in the repo, `make sync` copies to build area. Do it the other way (edit in build area → copy to repo) and the repo eventually goes stale. Happened to us once: `CONFIG_PINCTRL` didn't make it to the repo, cost us hours.

### Echo Test is Deceiving

`timeout 2 cat /dev/rpmsg1` may return empty but the message still reached its destination. Definitive proof is the serial console.

---

## File Structure

```
.
├── Makefile                      # make deploy / build / client / clean
├── ipc_proto.h                   # SHARED protocol — identical on both sides
├── led_ctrl.c                    # Linux client (open endpoint + send + read)
└── zephyr_app/                   # based on openamp_rsc_table
    ├── CMakeLists.txt            # adds 'src' to target_include_directories
    ├── prj.conf
    ├── boards/
    │   └── beagley_ai_j722s_main_r5f0_0.overlay
    └── src/
        ├── main_remote.c         # RPMsg + dispatch + LED/motor/servo
        └── ipc_proto.h           # copied by make sync
```

### Thread Structure and Priorities

| Thread | Priority | Job |
|---|---|---|
| `servo_thread` | `K_PRIO_COOP(4)` | 50 Hz pulse — most timing-critical |
| `motor_thread` | `K_PRIO_COOP(5)` | half-step phase sequence |
| `app_rpmsg_client_sample` | `K_PRIO_COOP(7)` | endpoint setup |
| `app_rpmsg_tty` | `K_PRIO_COOP(7)` | (unused) |
| `rpmsg_mng_task` | `K_PRIO_COOP(8)` | virtio/mailbox management |

Motor/servo have higher priority so hardware timing isn't affected by RPMsg traffic.

### Stepper Motor (28BYJ-48, half-step)

```c
static const uint8_t half_step[8][4] = {
    {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
    {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1},
};
```

Gear ratio 1:64, motor 64 half-steps/rev → 4096 half-steps = 1 full rotation.
Default speed 2000 µs/step (~8 s/rev). Phases released when idle — draws no current, doesn't heat up.

### Servo (SG90, software PWM)

```c
#define SERVO_PERIOD_US    20000    /* 50 Hz */
#define SERVO_MIN_US       500      /* 0° */
#define SERVO_MAX_US       2500     /* 180° */
```

Pulse generated with `k_busy_wait()` — `k_usleep` hits tick granularity issues at short durations. Busy-wait burns CPU but only for 0.5–2.5 ms, and timing is precise.

**Why not hardware PWM:** Zephyr's `j722s_main.dtsi` has no PWM nodes (drivers exist: `pwm_ti_am3352_ehrpwm.c`, `pwm_ti_am3352_ecap.c`). Would need to manually define the node + steal the PWM block from Linux. Software PWM works immediately.

---

## Troubleshooting

```bash
# Is the channel open? (look for virtio1, not virtio0)
sudo dmesg | grep -iE "virtio1|remoteproc3" | tail

# Is the device the right type?
ls -la /dev/rpmsg*            # must start with 'c'

# Hardware test (without Zephyr)
sudo gpioset gpiochip3 17=1   # LED
sudo gpioset gpiochip2 33=1   # servo pin

# Is a Kconfig setting actually enabled?
grep -E "^CONFIG_PINCTRL" ~/zephyrproject/zephyr/build/zephyr/.config

# Serial console
sudo picocom -b 115200 /dev/ttyUSB0
```

Expected boot output:
```
*** Booting Zephyr OS build v4.4.0-... ***
<inf> openamp_rsc_table: Starting application threads!
<inf> openamp_rsc_table: LED (GPIO6) ready
<inf> openamp_rsc_table: motor pins ready
<inf> openamp_rsc_table: servo pin ready
<inf> openamp_rsc_table: OpenAMP[remote] Linux responder demo started
<dbg> platform_ipm_callback: msg received from mb 1
```

---

## Roadmap

- [ ] **Ramp** — stepper acceleration/deceleration curve (faster without missed steps)
- [ ] **Jitter measurement** — timing stability under load (does R5 stay solid when A53 is busy?)
- [ ] **Shared memory / zero-copy** — for vision processing. Large buffers don't fit through RPMsg; use `ddr0` region directly with RPMsg only for "frame N ready, at address X" signaling
- [ ] **Hardware PWM** — define J722S PWM node in Zephyr (for high micro-step frequencies)
- [ ] **Multiple motors** — `id` field already in protocol

---

## References

| | |
|---|---|
| Zephyr BeagleY-AI board | [docs.zephyrproject.org](https://docs.zephyrproject.org/latest/boards/beagle/beagley_ai/) |
| Zephyr J722S R5 support | [PR #80344](https://github.com/zephyrproject-rtos/zephyr/pull/80344) |
| TI AM67A | [ti.com/product/AM67A](https://www.ti.com/product/AM67A) |
| T3 Gemstone | [docs.t3gemstone.org](https://docs.t3gemstone.org) |
| Based on sample | `zephyr/samples/subsys/ipc/openamp_rsc_table` |
| TI MCU+ SDK (conceptual) | [J722S API guide](https://software-dl.ti.com/jacinto7/esd/processor-sdk-rtos-j722s/) |

> This project builds on [running Zephyr on the R5 core](https://github.com/MehmetEmreee/zephyr-t3gemstone-o1-r5f). There, Zephyr ran independently on remoteproc3 (hello_world, jitter demo); here, bidirectional communication with Linux is established.
