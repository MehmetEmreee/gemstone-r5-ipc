<div align="center">

# T3-Gemstone-O1 Deterministik Motor Kontrolü

**Linux ↔ Zephyr AMP köprüsü — TI AM67A (J722S) üzerinde gerçek zamanlı donanım kontrolü**

Komutlar Linux'tan gelir; donanımı, Cortex-R5F üzerinde çalışan Zephyr deterministik zamanlamayla sürer.

[![Platform](https://img.shields.io/badge/SoC-TI%20AM67A%20(J722S)-red)]()
[![Board](https://img.shields.io/badge/board-T3%20Gemstone%20O1-blue)]()
[![RTOS](https://img.shields.io/badge/RTOS-Zephyr%204.4-purple)]()
[![IPC](https://img.shields.io/badge/IPC-OpenAMP%20%2F%20RPMsg-green)]()

*[English version below](#t3-gemstone-o1-deterministic-motor-control)*

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

Linux (A53) komutu gönderdikten sonra kendi işine döner; motoru, R5F üzerindeki Zephyr kendi ritminde ve kesintisiz olarak sürer. Linux tarafı yoğunlaşsa, takılsa, hatta tamamen çökse bile motor döngüsü bundan etkilenmez. Zaten AMP kullanmamızın asıl sebebi de bu.

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

## Neden (Asymmetric Multi-Processing) AMP ?

Bu projeye başlarken aklımdaki soru şuydu:

> *"Linux tarafım yavaşken bir çekirdeğimi gerçek zamanlı çalıştırmak ne işe yarar? Bir insanın bir bacağı 100 km/s hızla giderken diğeri 60 km/s hızla gidiyorsa, dengeli hareket için 60'a inmek zorunda kalır. 100 km/s hızın ne anlamı kalır ki ?"*

Bu sezgi, iki çekirdeğin aynı işi yaptığı ve birbirini beklediği varsayımına dayanıyor. AMP'nin amacı ise tam olarak bu varsayımı ortadan kaldırmak. Daha doğru bir benzetme, orkestra şefi ile metronom olurdu:

| | A53 · Linux | R5F · Zephyr |
|---|---|---|
| **İyi olduğu şey** | Throughput — bol veriyi hızlı işlemek | Latency ve determinizm — az veriyi tam zamanında işlemek |
| **Görevi** | Görüntü işleme, ağ, kütüphaneler, karar verme | Motor döngüsü, pals zamanlaması, refleks |
| **Ölçüsü** | Ortalama hız | En kötü durum gecikmesi |
| **Bir tur kaçırırsa** | Bir kare düşer, kimse fark etmez | Motor sarsılır, adım kaçar, sargı yanar |

Motor kontrolünde önemli olan ortalama hız değil, her randevuya zamanında yetişebilmek. Linux bunu (PREEMPT_RT ile) çoğu zaman başarıyor; ama araya bir ağ kesmesi, bir sürücü kilidi ya da bir bellek işlemi girdiğinde milisaniyeler mertebesinde gecikebiliyor. Ortalama performansı harika olsa da en kötü durumu öngörülemiyor. R5F tarafında ise araya girecek bir katman yok; söz verdiği zamanda orada oluyor.

Şunu da vurgulamak gerekiyor: R5F "daha hızlı" bir çekirdek değil, ham işlem gücünde A53 kat kat üstün. R5F'in getirdiği şey güvenilirlik. İki çekirdek birbirini beklemediği için sistemin toplam kalitesi en yavaş halkaya inmek yerine, iki tarafın güçlü yanlarının birleşiminden oluşuyor.

---

## Mimari

### Katmanlar

Haberleşme tek bir API'den değil, üst üste oturan üç katmandan oluşuyor:

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

Bir komutun izlediği yol şöyle: `write(/dev/rpmsg1, &cmd)` çağrısıyla rpmsg paketi paylaşımlı belleğe yazılır, mailbox R5 tarafında bir kesme tetikler, Zephyr'in callback'i uyanır, `dispatch()` ilgili handler'ı çağırır ve sonunda `gpio_pin_set_dt()` pini sürer.

Mailbox burada önemli bir rol oynuyor: R5, belleği sürekli yoklamak yerine yalnızca iş geldiğinde uyanıyor.

### Çekirdek Haritası

| remoteproc | Adres | Tür | Durum |
|---|---|---|---|
| remoteproc0/1 | `7e000000/7e200000.dsp` | C7x DSP | ilgisiz |
| remoteproc2 | `79000000.r5f` | MCU-domain R5F | **dokunmayın** — TI'ın ping-pong firmware'i |
| **remoteproc3** | `78400000.r5f` | **MAIN-domain R5F** | ✅ **Zephyr burada çalışıyor** |

> **Uyarı:** `dmesg` çıktısında `virtio0: rpmsg host is online` satırını görmek yanıltıcı olabilir; o satır remoteproc2'ye (MCU-R5) ait ve sistem açıldığından beri orada. Bizim kanalımız virtio1.

### TI MCU + SDK ile İlişkisi

TI'ın MCU+ SDK dokümantasyonu, FreeRTOS/NORTOS üzerine kurulu kendi IPC yığınını anlatıyor. Bizim R5 tarafında Zephyr çalıştığı için o API'ler burada birebir kullanılmıyor; köprü, iki tarafın da ortak konuştuğu RPMsg/virtio protokolü üzerinden kuruluyor. Kavramlar yine de birebir örtüşüyor:

| TI MCU + SDK | Buradaki karşılığı |
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
LED ────│ 31  32  │ (PWM-ECAP0 — kullanmayın)           │
        │ 33  34  │ (PWM-1B — kullanmayın)              │
IN4 ────│ 35  36  │ ← IN3                               │
        │ 37  38  │                                     │
        │ 39  40  │                                     │
        └───────────────────────────────────────────────┘
```

### Pin Kimlik Tablosu

Her pinin beş farklı ismi var ve aralarındaki tek güvenilir bağ pinmux offset'i. Bu tabloyu, isimler arasında kaybolmamak için tutuyoruz:

| İşlev | Header | Gemstone | Linux | TI pad | Zephyr node + offset | pinctrl |
|---|---|---|---|---|---|---|
| **LED** | 31 | GPIO-6 | `gpiochip3` 17 | `SPI0_CLK.GPIO1_17` | `main_gpio1_0` 17 | `hat_31_gpio` (`0x1bc`) |
| **IN1** | 11 | GPIO-17 | `gpiochip3` 8 | `MCASP0_AXR2.GPIO1_8` | `main_gpio1_0` 8 | `hat_11_gpio` |
| **IN2** | 29 | GPIO-5 | `gpiochip3` 15 | `SPI0_CS0.GPIO1_15` | `main_gpio1_0` 15 | `hat_29_gpio` |
| **IN3** | 36 | GPIO-16 | `gpiochip3` 7 | `MCASP0_AXR3.GPIO1_7` | `main_gpio1_0` 7 | `hat_36_gpio` |
| **IN4** | 35 | GPIO-19 | `gpiochip3` 12 | `MCASP0_AFSX.GPIO1_12` | `main_gpio1_0` 12 | `hat_35_gpio` |
| **Servo** | 13 | GPIO-27 | `gpiochip2` 33 | `GPMC0_OEn_REn.GPIO0_33` | `main_gpio0_1` **1** | `hat_13_gpio` (`0x088`) |

Pinmux offset'ten PADCONFIG numarasına geçmek için offset'i 4'e bölmek yeterli: örneğin `0x1bc / 4 = 111`, yani PADCONFIG111.

**Bank offset uyarısı:** J722S'in GPIO blokları Zephyr'de ikiye bölünmüş durumda. `main_gpio0_0` 0–31 arası line'ları, `main_gpio0_1` ise 32–63 arası line'ları kapsıyor ama her blok kendi içinde 0'dan saymaya başlıyor. Yani Linux'ta `gpiochip2` line 33 olan pin, Zephyr'de `main_gpio0_1` üzerinde 33−32 = 1 numaralı offset'e denk geliyor.

### Bağlantılar

| Bileşen | Bağlantı |
|---|---|
| **LED** | anot → 330Ω → pin 31, katot → GND |
| **28BYJ-48 + ULN2003** | IN1→11, IN2→29, IN3→36, IN4→35, VCC→2, GND→6 |
| **SG90 servo** | sinyal→13, 5V→4, GND→9 |

> ⚠️ **Besleme:** SG90 hareket anında 200–700 mA, 28BYJ-48 ise yaklaşık 200 mA çekiyor. İkisi aynı anda kartın 5V pininden beslenirse gerilim düşüyor, kart sıfırlanıyor ve SD kart bozulabiliyor. Ciddi bir kullanımda ortak GND'li harici bir besleme kullanmanızı öneririz.

---

## Hızlı Başlangıç

### 1. Bir Kerelik Kurulum

```bash
# SSH anahtarı (host → kart)
ssh-copy-id gemstone@192.168.7.2

# Şifresiz deploy (kartta) — SSH üzerinden sudo şifre soramıyor
echo "gemstone ALL=(ALL) NOPASSWD: /bin/cp /tmp/zephyr.elf /lib/firmware/zephyr.elf, /sbin/reboot" \
  | sudo tee /etc/sudoers.d/rpmsg-deploy
sudo chmod 440 /etc/sudoers.d/rpmsg-deploy
```

### 2. Deploy

```bash
make deploy      # sync + derle + firmware at + client at + reboot
```

Sürecin tamamı bu tek komutla yürüyor ve yaklaşık 30 saniye sürüyor.

| Hedef | Ne Yapar? |
|---|---|
| `make sync` | repo'yu `~/zephyrproject/led_ipc` build alanına kopyalar |
| `make build` | `west build -p always` çalıştırır |
| `make fw` | firmware'i karta atar |
| `make client` | sadece `led_ctrl`'yi atar (reboot gerekmez) |
| `make deploy` | hepsini yapar ve kartı yeniden başlatır |
| `make clean` | build klasörünü siler |

> **Kaynak dosyalar repo'da tutulur.** `~/zephyrproject/led_ipc` yalnızca build alanıdır; orada dosya düzenlemeyin, çünkü `make sync` bir sonraki çalıştırmada üzerine yazacaktır. Akışı tersine çevirirseniz repo zamanla eskir.

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

> **`echo stop > /sys/class/remoteproc/remoteproc3/state` işe yaramıyor:**
> ```
> k3_r5_rproc_stop: timeout waiting for rproc completion event
> remoteproc remoteproc3: can't stop rproc: -16
> ```
> Takılmış bir çekirdek "durdum" onayını gönderemediği için `stop` komutu zaman aşımına uğruyor. Sistem, açılışta symlink'in gösterdiği firmware'i zaten otomatik başlattığından, dosyayı değiştirip yeniden başlatmak yeterli. `stop` ile uğraşarak vakit kaybetmeyin.

---

## Komut Protokolü

Tek byte'lık komutlar yerine yapılandırılmış mesajlar kullanılıyor. `ipc_proto.h` dosyasının iki tarafta da birebir aynı olması gerekiyor.

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

**`packed` neden şart?** İki taraf farklı derleyicilerle derleniyor: host tarafında `gcc`, Zephyr tarafında `arm-zephyr-eabi-gcc`. `packed` olmadan derleyicilerden biri yapıya dolgu byte'ları ekleyebilir; bu durumda boyutlar uyuşmaz ve veri bozulur.

**`version` alanı neden var?** Protokol ileride değişirse eski firmware, sessizce yanlış davranmak yerine `RESP_ERR_VER` döndürür.

### Komut Tablosu

| Kod | Komut | `value` | Açıklama |
|---|---|---|---|
| `0x00` | `CMD_PING` | — | `0xABCD` |
| `0x01` | `CMD_LED_SET` | 0/1 | LED |
| `0x02` | `CMD_LED_GET` | — |  LED Durum |
| `0x10` | `CMD_MOTOR_STEP` | ±adım | 4096 = 1 tur |
| `0x11` | `CMD_MOTOR_STOP` | — | Step motoru durdur |
| `0x12` | `CMD_MOTOR_GET` | — | Step motor pozisyonu |
| `0x13` | `CMD_MOTOR_SPD` | µs | Adım arası süresi, 1000–100000 |
| `0x20` | `CMD_SERVO_SET` | 0–180 | Derece |
| `0x21` | `CMD_SERVO_GET` | — | Açı (−1 = kapalı) |
| `0x22` | `CMD_SERVO_OFF` | — | Tetiklemeyi kes |
| `0x30` | `CMD_STATUS_GET` | — | (Ayrılmış) |

### Cevap Kodları

| Kod | Anlam |
|---|---|
| `RESP_OK` | Başarılı |
| `RESP_ERR_CMD` | Bilinmeyen komut |
| `RESP_ERR_ID` | Geçersiz id |
| `RESP_ERR_VALUE` | Geçersiz değer |
| `RESP_ERR_VER` | Protokol sürümü uyuşmuyor |

### Linux Tarafı: Endpoint Açma

Bu kernel'de `rpmsg_tty` modülü bulunmuyor (`modprobe: FATAL: Module rpmsg_tty not found`); onun yerine `rpmsg_char` ve `rpmsg_ctrl` var. Kanallar duyurulsa da `/dev/rpmsgX` cihazı kendiliğinden oluşmuyor; `/dev/rpmsg_ctrl1` üzerinden bir ioctl çağrısıyla açmak gerekiyor:

```c
struct rpmsg_endpoint_info ept = {0};
strncpy(ept.name, "rpmsg-client-sample", sizeof(ept.name) - 1);
ept.src = 0xFFFFFFFF;
ept.dst = 0x400;
ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept);   /* → /dev/rpmsg1 belirir */
```

`led_ctrl` bu adımı kendisi hallettiği için reboot sonrasında tek bir komut yeterli; elle yapmanız gereken bir şey yok.

---

## Yeni Komut Ekleme

Dispatch tablosu sayesinde yeni bir komut eklemek üç küçük değişiklikten ibaret; uzayıp giden `if/else` zincirlerine gerek kalmıyor.

**1.** `ipc_proto.h` içindeki enum'a yeni komutu ekleyin:

```c
CMD_MOTOR_HOME = 0x14,
```

**2.** `main_remote.c` içinde handler'ını yazın:

```c
static int handle_motor_home(const struct ipc_cmd *cmd, struct ipc_resp *resp)
{
    motor_target = -motor_position;
    k_sem_give(&motor_sem);
    resp->value = 0;
    return RESP_OK;
}
```

**3.** Yine `main_remote.c` içinde komut tablosuna kaydedin:

```c
static const struct cmd_entry cmd_table[] = {
    ...
    { CMD_MOTOR_HOME, handle_motor_home },
};
```

> Handler fonksiyonlarının `cmd_table`'dan önce tanımlanmış olması gerekiyor.

---

## Kritik Kconfig Ayarları

| Ayar | Neden Şart? |
|---|---|
| `CONFIG_PINCTRL=y` | Yoksa overlay'deki `pinctrl-0` sessizce yok sayılır; konsol "LED ON" der ama LED yanmaz |
| `CONFIG_LOG_BACKEND_UART=y` | `CONFIG_UART_CONSOLE=y` sadece `printk` çıktısını yönlendirir; `LOG_INF` için ayrı backend gerekir |
| `CONFIG_SHELL=n` | Orijinal örnek konsolu RPMsg'e bağlar; RPMsg açılamazsa firmware tamamen sessiz kalır |
| `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` | Servo pulse periyodunun düzgün olması için gerekli (aşağıdaki tick tuzağına bakın) |
| `CONFIG_GPIO=y` | GPIO sürücüsü için |

Öte yandan iki ayardan uzak durun: `CONFIG_LOG_MODE_MINIMAL` log backend'ini devre dışı bıraktığı için tüm çıktıyı kaybettiriyor; `CONFIG_OPENAMP_WITH_DCACHE=y` ise bu SoC'te hiçbir etki göstermiyor (ayrıntısı aşağıda).

---

## Tuzaklar ve Öğrenilen Dersler

### Sinsi Tuzak: Linux'un Pinmux'unu Miras Almak

**Belirti:** Zephyr çalışıyor, konsolda `LED ON` yazıyor ama LED yanmıyor. Sonra Linux'tan bir kez `sudo gpioset gpiochip3 17=1` çalıştırıyorsunuz ve o andan itibaren Zephyr'in kontrolü de çalışmaya başlıyor. Kartı yeniden başlattığınızda sorun geri geliyor.

**Sebep:** `CONFIG_PINCTRL=y` eksik. Bu durumda overlay'deki `pinctrl-0` sessizce yok sayılıyor ve derleme hatası bile alınmıyor; Zephyr pad'i hiç yapılandırmıyor. `gpioset` komutu pad'i MUX_MODE_7'ye aldığı ve bu ayar kalıcı olduğu için Zephyr çalışıyormuş gibi görünüyor.

**Ders:** Bir şey ancak başka bir şey önceden yapıldığında çalışıyorsa, çalışan sizin kodunuz değildir. Testleri reboot sonrasında, hiçbir Linux GPIO komutu çalıştırmadan yapın.

### `CONFIG_OPENAMP_WITH_DCACHE` Bu SoC'te Çalışmıyor

Bu ayar diğer board overlay'lerinin hemen hepsinde (imx8mp, imx93, stm32mp...) bulunduğu için bizde de eksik olduğunu düşünüp ekledik, ama etkisiz çıktı:

```
warning: OPENAMP_WITH_DCACHE was assigned the value 'y' but got the value 'n'.
Check these unsatisfied dependencies: CACHE_MANAGEMENT (=n)
```

Bağımlılık zinciri şöyle: `OPENAMP_WITH_DCACHE` → `CACHE_MANAGEMENT` → `depends on DCACHE || ICACHE`. `.config` dosyasında `DCACHE=y`, `ICACHE=y` ve `CPU_HAS_DCACHE=y` görünmesine rağmen `CACHE_MANAGEMENT` yine `n` kalıyor, çünkü TI'ın J722S R5F portu cache management backend'ini henüz implemente etmemiş. `CONFIG_CACHE=y` veya `CONFIG_CACHE_MANAGEMENT=y` eklemek de sonucu değiştirmiyor.

**Ders:** Build çıktısını `tail -25` ile izlerseniz bu uyarıyı hiç göremezsiniz, çünkü Kconfig uyarıları çıktının en başında yer alıyor. Bir ayarın gerçekten etkin olduğundan emin olmak için üretilen `.config` dosyasına bakın:

```bash
grep -E "^CONFIG_PINCTRL" ~/zephyrproject/zephyr/build/zephyr/.config
```

### İlk Takılmayı Ne Çözdü? Açıkçası Bilmiyoruz

İlk denemede firmware açıldı ve `remote processor is now up` mesajı geldi, ama `rpmsg host is online` hiç görünmedi. Bir sonraki turda aynı anda birden fazla şeyi değiştirdik ve sistem çalışmaya başladı:

- Konsolu UART'a aldık
- RPMsg shell'i kapattık (`CONFIG_SHELL=n`)
- Stack ve heap boyutlarıyla oynadık
- `CONFIG_OPENAMP_WITH_DCACHE=y` ekledik (sonradan etkisiz olduğu anlaşıldı)

Hangi değişikliğin sorunu çözdüğünü kesin olarak bilmiyoruz. En güçlü şüphelimiz, shell'in RPMsg'e bağlanmasının kanal kurulumu sırasında çakışma yaratması.

**Ders:** Aynı anda birden fazla değişkeni değiştirmeyin; yoksa "ne çözdü" sorusunun cevabı sonsuza kadar kaybolur.

### Firmware'i Önce Konuşturun

Orijinal örnekte `CONFIG_PRINTK=n` ayarlı ve konsol RPMsg üzerinden çalışıyor. RPMsg açılamadığında firmware tamamen sessiz kalıyor ve nerede takıldığını görmenin hiçbir yolu olmuyor. Bu yüzden ilk iş konsolu UART'a almak olmalı; hata ayıklamaya ondan sonra başlayın.

### Servo Tık Tık Ediyor: Tick Granülaritesi

**Belirti:** Servo boşta dururken bile sürekli mikro düzeltmeler yapıyor ve tık tık sesler çıkarıyor.

**Sebep:** Zephyr'in varsayılan tick frekansı 100 Hz, yani 10 ms granülarite. `k_usleep(18500)` çağırdığınızda çekirdek yalnızca tick sınırlarında uyanabildiği için periyot, sabit 20 ms yerine 11.5 ile 21.5 ms arasında zıplıyor. Pals genişliğinin kendisi doğru (`k_busy_wait` tick'e bağlı değil) ama palslar arası mesafe oynak olduğu için servo tereddüt ediyor.

**Çözüm:** `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` ile granülarite 100 µs'ye iniyor. Bedeli, saniyede 10.000 tick kesmesinin CPU'nun yaklaşık %1–2'sine mal olması; R5 için ihmal edilebilir bir yük. Karşılığında tüm `k_usleep`/`k_sleep` çağrıları 100 µs hassasiyetine kavuşuyor, hatta step motorun zamanlaması da bu sayede düzeldi.

### SG90 Pals Aralığı 500–2500 µs

Yaygın kaynaklar 1000–2000 µs der, ama piyasadaki SG90 klonlarının çoğu 500–2500 µs bekliyor. 1000–2000 aralığında kalırsanız servo mekanik aralığının ancak yarısını kullanabiliyor ve açılar ikiye bölünmüş gibi hissediliyor.

> Bazı klonlar 500 µs'de mekanik sınıra dayanıp inleyebiliyor. Böyle bir ses duyarsanız `soff` verin ve aralığı biraz daraltın (örneğin 600–2400).

### `tee` ile /dev/rpmsg1'e Yazmayın

```bash
echo -n "1" | sudo tee /dev/rpmsg1     # cihaz yoksa düz dosya oluşturur!
```

`/dev/rpmsg1` her reboot sonrasında kayboluyor; `tee` ise cihaz yerinde değilse aynı isimde sıradan bir dosya oluşturuyor. Yazma işlemi başarılı görünse de mesaj hiçbir yere gitmiyor, üstelik o dosya diskte kaldığı için sonraki denemeleri de bozuyor.

```bash
$ ls -la /dev/rpmsg*
crw------- ... /dev/rpmsg0     # 'c' = karakter cihazı ✓
-rw-r--r-- ... /dev/rpmsg1     # '-' = düz dosya ✗
```

`led_ctrl` içindeki `S_ISCHR()` kontrolü bu durumu otomatik olarak yakalıyor.

### virtio0 ile virtio1'i Karıştırmayın

`dmesg` çıktısındaki `virtio0: rpmsg host is online` satırı sizin firmware'inize ait değil; o satır MCU-R5'ten (remoteproc2, TI'ın ping-pong firmware'i) geliyor ve açılıştan beri orada. Sizin kanalınız virtio1 olarak görünüyor:

```bash
ls -la /sys/bus/rpmsg/devices/
# .../remoteproc3/rproc-virtio.7.auto/virtio1/virtio1.rpmsg-client-sample.-1.1024
```

### Overlay'de `#include` Unutmayın

```
devicetree error: ...overlay:5 (column 30): parse error: expected number or parenthesized expression
```

Bu hatanın sebebi, `GPIO_ACTIVE_HIGH` gibi sabitlerin dt-binding başlıklarından gelmesi. Dosyanın en üstüne şu satırı eklemek gerekiyor:

```c
#include <zephyr/dt-bindings/gpio/gpio.h>
```

### GPIO Node'ları Varsayılan Olarak Kapalı

J722S'te `dts/arm/ti/j722s_main.dtsi` içindeki tüm GPIO node'ları `status = "disabled"` olarak geliyor. Kullandığınız her bank'ı overlay'de açmanız gerekiyor:

```
&main_gpio1_0 { status = "okay"; };
```

### Adresler Zaten Hizalı, Boş Yere Aramayın

Zephyr'in board tanımı `ddr0` bölgesini `0xa2000000` adresine yerleştiriyor; Linux da aynı adrese `main-r5fss-dma-memory-region@a2000000` bölgesini atıyor. İkisi birebir uyuşuyor, çünkü resmi board tanımı bu hizalamayı zaten halletmiş. Bununla ayrıca uğraşmanıza gerek yok.

### Kaynak Tek Yerde Olsun

Kaynak repo'da durur ve `make sync` onu build alanına kopyalar. Akışı tersine çevirirseniz — yani build alanında düzenleyip repo'ya elle kopyalamaya kalkarsanız — repo er ya da geç eskir. Bunu bir kez yaşadık: `CONFIG_PINCTRL` ayarı repo'ya geçmemişti ve bu unutkanlık bize saatlere mal oldu.

### Echo Testi Yanıltıcı Olabilir

`timeout 2 cat /dev/rpmsg1` boş dönse bile mesaj karşı tarafa ulaşmış olabilir. Kesin kanıt istiyorsanız seri konsola bakın.

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

Donanım zamanlaması RPMsg trafiğinden etkilenmesin diye motor ve servo thread'leri daha yüksek öncelikte çalışıyor.

### Step Motor (28BYJ-48, half-step)

```c
static const uint8_t half_step[8][4] = {
    {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
    {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1},
};
```

Dişli oranı 1:64 ve motorun kendisi tur başına 64 half-step attığı için 4096 half-step bir tam tura karşılık geliyor. Varsayılan hız 2000 µs/adım, yani tur başına yaklaşık 8 saniye. Hareket bittiğinde fazlar bırakılıyor; motor boşta akım çekmiyor ve ısınmıyor.

### Servo (SG90, yazılım PWM)

```c
#define SERVO_PERIOD_US    20000    /* 50 Hz */
#define SERVO_MIN_US       500      /* 0° */
#define SERVO_MAX_US       2500     /* 180° */
```

Pals `k_busy_wait()` ile üretiliyor, çünkü `k_usleep` kısa sürelerde tick granülaritesine takılıyor. Busy-wait CPU harcıyor elbette, ama süre yalnızca 0.5–2.5 ms ve karşılığında zamanlama kesin oluyor.

**Neden donanım PWM kullanmadık?** Zephyr'in `j722s_main.dtsi` dosyasında hiç PWM node'u tanımlı değil; sürücülerin kendisi mevcut (`pwm_ti_am3352_ehrpwm.c`, `pwm_ti_am3352_ecap.c`) ama node'u elle tanımlamak ve Linux'un kullandığı PWM bloğunu ondan koparmak gerekecekti. Yazılım PWM ise hiçbir ek uğraş gerektirmeden çalıştı.

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

Her şey yolundaysa açılışta şuna benzer bir çıktı görmelisiniz:

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

- [ ] **Ramp** — step motora hızlanma/yavaşlama eğrisi eklemek, böylece adım kaçırmadan daha yüksek hızlara çıkmak
- [ ] **Jitter ölçümü** — A53 yük altındayken R5'in zamanlamasının gerçekten etkilenmediğini ölçerek göstermek
- [ ] **Shared memory / zero-copy** — görüntü işleme için: büyük buffer'lar RPMsg'den geçmediğinden `ddr0` bölgesini doğrudan kullanıp RPMsg'i yalnızca "frame N hazır, şu adreste" sinyali için kullanmak
- [ ] **Donanım PWM** — yüksek mikro-adım frekansları için J722S PWM node'unu Zephyr'de tanımlamak
- [ ] **Çoklu motor** — protokoldeki `id` alanı bunun için zaten hazır

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

> Bu proje, [Zephyr'i R5 çekirdeğinde çalıştırma](https://github.com/MehmetEmreee/zephyr-t3gemstone-o1-r5f) çalışmasının devamı niteliğinde. Orada Zephyr, remoteproc3 üzerinde bağımsız olarak çalışıyordu (hello_world ve jitter demosu); bu projede ise Linux ile çift yönlü haberleşme kuruldu.

---

<div align="center">

# T3-Gemstone-O1 Deterministic Motor Control

**Linux ↔ Zephyr AMP bridge — real-time hardware control on the TI AM67A (J722S)**

Commands come from Linux; the hardware is driven with deterministic timing by Zephyr running on a Cortex-R5F.

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

Linux (A53) sends a command and gets back to its own work, while Zephyr on the R5F drives the motor at its own pace, without interruption. Even if the Linux side gets busy, stalls, or crashes outright, the motor loop keeps running unaffected. That is exactly why we use AMP in the first place.

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

## Why (Asymmetric Multi-Processing) AMP?

The question I started this project with was:

> *"If my Linux side is slow, what good is running one core in real time? If a person has one leg going 100 km/h and the other 60 km/h, they have to drop to 60 to move steadily."*

That intuition assumes both cores do the same work and have to wait for each other, and breaking that assumption is precisely what AMP is for. A better analogy would be a conductor and a metronome:

| | A53 · Linux | R5F · Zephyr |
|---|---|---|
| **Best at** | Throughput — processing lots of data fast | Latency and determinism — processing little data exactly on time |
| **Job** | Vision, networking, libraries, decisions | Motor loop, pulse timing, reflexes |
| **Measured by** | Average speed | Worst-case latency |
| **If it misses a cycle** | A frame drops, nobody notices | The motor jerks, a step is lost, a winding burns |

In motor control what matters is not the average speed but meeting every deadline, every single time. Linux usually manages this (with PREEMPT_RT), but a network interrupt, a driver lock, or a memory operation can occasionally delay it by milliseconds. Its average performance is excellent; its worst case is unpredictable. On the R5F side there are no layers in the way, so it shows up exactly when it promised to.

One thing worth stressing: the R5F is not the faster core — in raw compute the A53 wins by a wide margin. What the R5F brings is reliability. And since the two cores never wait for each other, the overall quality of the system comes from combining their strengths rather than sinking to the level of the slowest link.

---

## Architecture

### Layers

The communication path is not a single API but three layers stacked on top of each other:

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

A command travels like this: the `write(/dev/rpmsg1, &cmd)` call places an rpmsg packet in shared memory, the mailbox raises an interrupt on the R5, Zephyr's callback wakes up, `dispatch()` calls the matching handler, and finally `gpio_pin_set_dt()` drives the pin.

The mailbox plays an important role here: instead of constantly polling memory, the R5 wakes up only when there is actual work to do.

### Core Map

| remoteproc | Address | Type | Status |
|---|---|---|---|
| remoteproc0/1 | `7e000000/7e200000.dsp` | C7x DSP | irrelevant |
| remoteproc2 | `79000000.r5f` | MCU-domain R5F | **leave it alone** — runs TI's ping-pong firmware |
| **remoteproc3** | `78400000.r5f` | **MAIN-domain R5F** | ✅ **Zephyr runs here** |

> **Warning:** Seeing `virtio0: rpmsg host is online` in `dmesg` can be misleading; that line belongs to remoteproc2 (the MCU-domain R5F) and has been there since boot. Our channel is virtio1.

### Relationship with the TI MCU+ SDK

TI's MCU+ SDK documentation describes its own IPC stack, built on FreeRTOS/NORTOS. Since our R5 runs Zephyr, those APIs are not used here directly; the bridge is built on RPMsg/virtio, the common protocol both sides speak. The concepts still map one to one:

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
LED ────│ 31  32  │ (PWM-ECAP0 — avoid)                 │
        │ 33  34  │ (PWM-1B — avoid)                    │
IN4 ────│ 35  36  │ ← IN3                               │
        │ 37  38  │                                     │
        │ 39  40  │                                     │
        └───────────────────────────────────────────────┘
```

### Pin Identity Table

Every pin here has five different names, and the only reliable link between them is the pinmux offset. We keep this table so nobody gets lost between the naming schemes:

| Function | Header | Gemstone | Linux | TI pad | Zephyr node + offset | pinctrl |
|---|---|---|---|---|---|---|
| **LED** | 31 | GPIO-6 | `gpiochip3` 17 | `SPI0_CLK.GPIO1_17` | `main_gpio1_0` 17 | `hat_31_gpio` (`0x1bc`) |
| **IN1** | 11 | GPIO-17 | `gpiochip3` 8 | `MCASP0_AXR2.GPIO1_8` | `main_gpio1_0` 8 | `hat_11_gpio` |
| **IN2** | 29 | GPIO-5 | `gpiochip3` 15 | `SPI0_CS0.GPIO1_15` | `main_gpio1_0` 15 | `hat_29_gpio` |
| **IN3** | 36 | GPIO-16 | `gpiochip3` 7 | `MCASP0_AXR3.GPIO1_7` | `main_gpio1_0` 7 | `hat_36_gpio` |
| **IN4** | 35 | GPIO-19 | `gpiochip3` 12 | `MCASP0_AFSX.GPIO1_12` | `main_gpio1_0` 12 | `hat_35_gpio` |
| **Servo** | 13 | GPIO-27 | `gpiochip2` 33 | `GPMC0_OEn_REn.GPIO0_33` | `main_gpio0_1` **1** | `hat_13_gpio` (`0x088`) |

To get from a pinmux offset to a PADCONFIG number, just divide by 4: for example `0x1bc / 4 = 111`, so PADCONFIG111.

**Bank offset warning:** The J722S GPIO blocks are split in two on the Zephyr side. `main_gpio0_0` covers lines 0–31 and `main_gpio0_1` covers lines 32–63, but each block counts from 0 internally. So the pin that Linux calls `gpiochip2` line 33 becomes offset 33−32 = 1 on `main_gpio0_1` in Zephyr.

### Connections

| Component | Connection |
|---|---|
| **LED** | anode → 330Ω → pin 31, cathode → GND |
| **28BYJ-48 + ULN2003** | IN1→11, IN2→29, IN3→36, IN4→35, VCC→2, GND→6 |
| **SG90 servo** | signal→13, 5V→4, GND→9 |

> ⚠️ **Power:** The SG90 draws 200–700 mA while moving and the 28BYJ-48 draws around 200 mA. If both are powered from the board's 5V pin at the same time, the voltage sags, the board resets, and the SD card may get corrupted. For anything serious, use an external supply with a common ground.

---

## Quick Start

### 1. One-Time Setup

```bash
# SSH key (host → board)
ssh-copy-id gemstone@192.168.7.2

# Passwordless deploy (on the board) — sudo can't prompt for a password over SSH
echo "gemstone ALL=(ALL) NOPASSWD: /bin/cp /tmp/zephyr.elf /lib/firmware/zephyr.elf, /sbin/reboot" \
  | sudo tee /etc/sudoers.d/rpmsg-deploy
sudo chmod 440 /etc/sudoers.d/rpmsg-deploy
```

### 2. Deploy

```bash
make deploy      # sync + build + push firmware + push client + reboot
```

The whole process runs from this single command and takes about 30 seconds.

| Target | What it does |
|---|---|
| `make sync` | copies the repo into the `~/zephyrproject/led_ipc` build area |
| `make build` | runs `west build -p always` |
| `make fw` | pushes the firmware to the board |
| `make client` | pushes only `led_ctrl` (no reboot needed) |
| `make deploy` | does all of the above and reboots the board |
| `make clean` | deletes the build directory |

> **The source lives in the repo.** `~/zephyrproject/led_ipc` is only a build area, so don't edit files there — `make sync` will overwrite them on the next run. Working the other way around eventually leaves the repo stale.

### 3. Usage

```bash
sudo ~/led_ctrl ping           # connectivity test → 0xABCD
sudo ~/led_ctrl led 1          # LED on
sudo ~/led_ctrl ledget         # read LED state

sudo ~/led_ctrl step 4096      # 1 full rotation forward (fire-and-forget)
sudo ~/led_ctrl step -512      # reverse
sudo ~/led_ctrl speed 1500     # step interval in µs (min 1000)
sudo ~/led_ctrl mget           # read position
sudo ~/led_ctrl mstop          # stop

sudo ~/led_ctrl servo 90       # 0–180°
sudo ~/led_ctrl sget
sudo ~/led_ctrl soff           # cut the pulse (no current draw, no heat)

sudo ~/led_ctrl raw 0x99 0 0   # unknown command → RESP_ERR_CMD
```

### 4. Starting the Firmware

```bash
sudo reboot
```

> **`echo stop > /sys/class/remoteproc/remoteproc3/state` does not work:**
> ```
> k3_r5_rproc_stop: timeout waiting for rproc completion event
> remoteproc remoteproc3: can't stop rproc: -16
> ```
> A hung core cannot send its "I stopped" acknowledgment, so the `stop` command simply times out. Since the system auto-starts whatever firmware the symlink points to at boot, replacing the file and rebooting is all it takes. Don't waste time fighting `stop`.

---

## Command Protocol

Instead of single-byte commands, the protocol uses structured messages. `ipc_proto.h` has to be byte-for-byte identical on both sides.

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

**Why `packed` is essential:** The two sides are built with different compilers — `gcc` on the host, `arm-zephyr-eabi-gcc` for Zephyr. Without `packed`, one of them may insert padding bytes into the struct; the sizes then stop matching and the data gets corrupted.

**Why the `version` field exists:** If the protocol ever changes, old firmware returns `RESP_ERR_VER` instead of silently misbehaving.

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
| `0x22` | `CMD_SERVO_OFF` | — | cut the pulse |
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

This kernel doesn't ship the `rpmsg_tty` module (`modprobe: FATAL: Module rpmsg_tty not found`), but `rpmsg_char` and `rpmsg_ctrl` are available. Channels get announced, yet `/dev/rpmsgX` is not created automatically; the device has to be opened through an ioctl on `/dev/rpmsg_ctrl1`:

```c
struct rpmsg_endpoint_info ept = {0};
strncpy(ept.name, "rpmsg-client-sample", sizeof(ept.name) - 1);
ept.src = 0xFFFFFFFF;
ept.dst = 0x400;
ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept);   /* → /dev/rpmsg1 appears */
```

`led_ctrl` takes care of this step itself, so a single command after reboot is enough; there is nothing to do manually.

---

## Adding New Commands

Thanks to the dispatch table, adding a new command comes down to three small changes, with no ever-growing `if/else` chains.

**1.** Add the new command to the enum in `ipc_proto.h`:

```c
CMD_MOTOR_HOME = 0x14,
```

**2.** Write its handler in `main_remote.c`:

```c
static int handle_motor_home(const struct ipc_cmd *cmd, struct ipc_resp *resp)
{
    motor_target = -motor_position;
    k_sem_give(&motor_sem);
    resp->value = 0;
    return RESP_OK;
}
```

**3.** Register it in the command table, also in `main_remote.c`:

```c
static const struct cmd_entry cmd_table[] = {
    ...
    { CMD_MOTOR_HOME, handle_motor_home },
};
```

> Handler functions need to be defined before `cmd_table`.

---

## Critical Kconfig Settings

| Setting | Why it's essential |
|---|---|
| `CONFIG_PINCTRL=y` | Without it, `pinctrl-0` in the overlay is silently ignored; the console says "LED ON" but the LED stays dark |
| `CONFIG_LOG_BACKEND_UART=y` | `CONFIG_UART_CONSOLE=y` only routes `printk` output; `LOG_INF` needs its own backend |
| `CONFIG_SHELL=n` | The original sample binds the console to RPMsg; if RPMsg fails to open, the firmware goes completely silent |
| `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` | Needed for a stable servo pulse period (see the tick trap below) |
| `CONFIG_GPIO=y` | For the GPIO driver |

Two settings to stay away from: `CONFIG_LOG_MODE_MINIMAL` disables the log backend and all output disappears, and `CONFIG_OPENAMP_WITH_DCACHE=y` has no effect on this SoC (details below).

---

## Pitfalls & Lessons Learned

### Sneaky Trap: Inheriting Linux's Pinmux

**Symptom:** Zephyr runs and the console says `LED ON`, but the LED stays off. Then you run `sudo gpioset gpiochip3 17=1` once from Linux, and from that moment on Zephyr's control starts working too. Reboot the board and the problem comes back.

**Cause:** `CONFIG_PINCTRL=y` is missing. In that case the overlay's `pinctrl-0` is silently ignored — you don't even get a build error — and Zephyr never configures the pad. The `gpioset` command sets the pad to MUX_MODE_7, and because that setting is persistent, Zephyr merely appears to work.

**Lesson:** If something only works after something else has been done first, then it is not your code that is working. Always test right after a reboot, without touching any Linux GPIO commands.

### `CONFIG_OPENAMP_WITH_DCACHE` Doesn't Work on This SoC

Almost every other board overlay (imx8mp, imx93, stm32mp...) carries this option, so we assumed it was missing here and added it. It turned out to have no effect:

```
warning: OPENAMP_WITH_DCACHE was assigned the value 'y' but got the value 'n'.
Check these unsatisfied dependencies: CACHE_MANAGEMENT (=n)
```

The dependency chain goes `OPENAMP_WITH_DCACHE` → `CACHE_MANAGEMENT` → `depends on DCACHE || ICACHE`. Even though `.config` shows `DCACHE=y`, `ICACHE=y` and `CPU_HAS_DCACHE=y`, `CACHE_MANAGEMENT` still ends up as `n`, because the TI J722S R5F port simply hasn't implemented the cache management backend yet. Adding `CONFIG_CACHE=y` or `CONFIG_CACHE_MANAGEMENT=y` doesn't change anything either.

**Lesson:** If you watch the build output with `tail -25`, you will never see this warning, because Kconfig warnings appear at the very beginning of the output. To be sure a setting is actually in effect, check the generated `.config`:

```bash
grep -E "^CONFIG_PINCTRL" ~/zephyrproject/zephyr/build/zephyr/.config
```

### What Fixed the First Hang? Honestly, We Don't Know

On the first attempt the firmware booted and `remote processor is now up` appeared, but `rpmsg host is online` never followed. In the next round we changed several things at once, and the system started working:

- Moved the console to the UART
- Disabled the RPMsg shell (`CONFIG_SHELL=n`)
- Adjusted the stack and heap sizes
- Added `CONFIG_OPENAMP_WITH_DCACHE=y` (later found to be ineffective)

We can't say for certain which change fixed it. Our strongest suspect is the shell binding to RPMsg and interfering with the channel setup.

**Lesson:** Never change more than one variable at a time; otherwise the answer to "what actually fixed it" is lost for good.

### Make the Firmware Talk First

The original sample ships with `CONFIG_PRINTK=n` and its console routed over RPMsg. When RPMsg fails to come up, the firmware stays completely silent and there is no way to see where it got stuck. So the very first step should be moving the console to the UART; debugging comes after that.

### The Servo Keeps Ticking: Tick Granularity

**Symptom:** Even while idle, the servo keeps making tiny corrections and produces audible ticking sounds.

**Cause:** Zephyr's default tick rate is 100 Hz, which means 10 ms granularity. When you call `k_usleep(18500)`, the kernel can only wake at tick boundaries, so the period jumps between 11.5 and 21.5 ms instead of holding a steady 20 ms. The pulse width itself is correct (`k_busy_wait` doesn't depend on ticks), but the spacing between pulses is erratic, and the servo hesitates.

**Fix:** `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000` brings the granularity down to 100 µs. The cost is that 10,000 tick interrupts per second eat roughly 1–2% of the CPU, which is negligible for the R5. In return, every `k_usleep`/`k_sleep` call gets 100 µs precision — the stepper timing improved as a nice side effect.

### SG90 Pulse Range Is 500–2500 µs

Most sources quote 1000–2000 µs, but the majority of SG90 clones actually expect 500–2500 µs. If you stick to 1000–2000, the servo only covers half of its mechanical range and every angle feels halved.

> Some clones hit their mechanical limit at 500 µs and start whining. If you hear that noise, send `soff` and narrow the range a little (600–2400 works well).

### Don't Write to /dev/rpmsg1 with `tee`

```bash
echo -n "1" | sudo tee /dev/rpmsg1     # creates a regular file if the device is missing!
```

`/dev/rpmsg1` disappears after every reboot, and if the device isn't there, `tee` happily creates a regular file with the same name. The write looks successful, but the message goes nowhere — and since that file stays on disk, it breaks the following attempts as well.

```bash
$ ls -la /dev/rpmsg*
crw------- ... /dev/rpmsg0     # 'c' = character device ✓
-rw-r--r-- ... /dev/rpmsg1     # '-' = regular file ✗
```

The `S_ISCHR()` check in `led_ctrl` catches this automatically.

### Don't Confuse virtio0 with virtio1

The `virtio0: rpmsg host is online` line in `dmesg` does not belong to your firmware; it comes from the MCU-domain R5F (remoteproc2, TI's ping-pong firmware) and has been there since boot. Your channel shows up as virtio1:

```bash
ls -la /sys/bus/rpmsg/devices/
# .../remoteproc3/rproc-virtio.7.auto/virtio1/virtio1.rpmsg-client-sample.-1.1024
```

### Don't Forget the `#include` in the Overlay

```
devicetree error: ...overlay:5 (column 30): parse error: expected number or parenthesized expression
```

This error appears because constants like `GPIO_ACTIVE_HIGH` come from the dt-binding headers. The overlay needs this include at the top:

```c
#include <zephyr/dt-bindings/gpio/gpio.h>
```

### GPIO Nodes Come Disabled by Default

In the J722S's `dts/arm/ti/j722s_main.dtsi`, every GPIO node ships as `status = "disabled"`. Each bank you use has to be enabled in the overlay:

```
&main_gpio1_0 { status = "okay"; };
```

### The Addresses Are Already Aligned, So Don't Go Hunting

The Zephyr board definition places `ddr0` at `0xa2000000`, and Linux assigns `main-r5fss-dma-memory-region@a2000000` to the same address. They match exactly, because the official board definition already took care of this alignment. There is nothing extra to do here.

### Keep the Source in One Place

The source lives in the repo, and `make sync` copies it into the build area. Reverse that flow — editing in the build area and copying back into the repo by hand — and the repo will eventually go stale. It happened to us once: the `CONFIG_PINCTRL` setting never made it back into the repo, and that one slip cost us hours.

### The Echo Test Can Be Deceiving

`timeout 2 cat /dev/rpmsg1` can come back empty even though the message actually arrived. If you want definitive proof, look at the serial console.

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

The motor and servo threads run at higher priority so that hardware timing is never disturbed by RPMsg traffic.

### Stepper Motor (28BYJ-48, half-step)

```c
static const uint8_t half_step[8][4] = {
    {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
    {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1},
};
```

The gear ratio is 1:64 and the motor itself takes 64 half-steps per revolution, so 4096 half-steps equal one full rotation. The default speed is 2000 µs per step, roughly 8 seconds per revolution. When a move finishes, the phases are released; the motor draws no current and stays cool while idle.

### Servo (SG90, software PWM)

```c
#define SERVO_PERIOD_US    20000    /* 50 Hz */
#define SERVO_MIN_US       500      /* 0° */
#define SERVO_MAX_US       2500     /* 180° */
```

The pulse is generated with `k_busy_wait()`, because `k_usleep` runs into tick-granularity problems at short durations. Busy-waiting does burn CPU, but only for 0.5–2.5 ms at a time, and in return the timing is exact.

**Why not hardware PWM?** Zephyr's `j722s_main.dtsi` defines no PWM nodes at all; the drivers themselves exist (`pwm_ti_am3352_ehrpwm.c`, `pwm_ti_am3352_ecap.c`), but we would have had to define the node by hand and pry the PWM block away from Linux. Software PWM worked with no extra effort.

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

If everything is in order, the boot output should look like this:

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

- [ ] **Ramp** — add an acceleration/deceleration curve to the stepper so it can go faster without missing steps
- [ ] **Jitter measurement** — measure timing stability under load and show that the R5 really is unaffected while the A53 is busy
- [ ] **Shared memory / zero-copy** — for vision work: large buffers don't fit through RPMsg, so use the `ddr0` region directly and keep RPMsg only for "frame N is ready at address X" signaling
- [ ] **Hardware PWM** — define the J722S PWM node in Zephyr, for high micro-stepping frequencies
- [ ] **Multiple motors** — the `id` field in the protocol is already there for this

---

## References

| | |
|---|---|
| Zephyr BeagleY-AI board | [docs.zephyrproject.org](https://docs.zephyrproject.org/latest/boards/beagle/beagley_ai/) |
| Zephyr J722S R5 support | [PR #80344](https://github.com/zephyrproject-rtos/zephyr/pull/80344) |
| TI AM67A | [ti.com/product/AM67A](https://www.ti.com/product/AM67A) |
| T3 Gemstone | [docs.t3gemstone.org](https://docs.t3gemstone.org) |
| Based on the sample | `zephyr/samples/subsys/ipc/openamp_rsc_table` |
| TI MCU+ SDK (conceptual) | [J722S API guide](https://software-dl.ti.com/jacinto7/esd/processor-sdk-rtos-j722s/) |

> This project is a follow-up to [running Zephyr on the R5 core](https://github.com/MehmetEmreee/zephyr-t3gemstone-o1-r5f). There, Zephyr ran standalone on remoteproc3 (hello_world and a jitter demo); here, bidirectional communication with Linux is established.
