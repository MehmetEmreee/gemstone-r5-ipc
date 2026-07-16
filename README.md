# gemstone-rpmsg-led

T3 Gemstone O1 (TI AM67A / J722S) kartinda **Linux (A53) ile Zephyr (Cortex-R5F) arasinda
RPMsg haberlesmesi** ve R5'ten gercek zamanli LED kontrolu.

```
echo -n "1" > /dev/rpmsg1    ->  LED yanar
echo -n "0" > /dev/rpmsg1    ->  LED soner
```

LED'i Linux degil, MAIN-domain R5F uzerinde calisan **Zephyr** surer.

## Mimari

```
Linux (A53)                          R5F (Zephyr)
  echo 1 > /dev/rpmsg1
        |
     rpmsg + virtio  --------------->  rpmsg endpoint callback
        |                                     |
   paylasilan bellek (DDR 0xa2000000)         |
        |                                     v
   Mailbox (mbox3)  ---- kesme ---->    gpio_pin_set_dt() -> LED
```

| Katman | Bizim kurulumda |
| :--- | :--- |
| RPMsg kanali | `rpmsg-client-sample` (addr 0x400) |
| virtio | `virtio1` (remoteproc3 altinda) |
| Shared memory | DDR `0xa2000000` (`ddr0`) |
| Mailbox | `mbox3` (MAIN-domain) |

## Icerik

```
zephyr_app/                  Zephyr uygulamasi (openamp_rsc_table tabanli)
  prj.conf                   Kconfig ayarlari
  boards/
    beagley_ai_j722s_main_r5f0_0.overlay    GPIO6 + pinctrl tanimi
  src/main_remote.c          RPMsg callback + LED kontrolu
led_ctrl.c                   Linux tarafi: rpmsg endpoint acan program
docs/rpmsg_led.pdf           Tam dokumantasyon (kurulum, tuzaklar, cheat sheet)
```

## Donanim

| | |
| :--- | :--- |
| Kart | T3 Gemstone O1 |
| SoC | TI AM67A (J722S) |
| Linux | 4x Cortex-A53, kernel `6.12.24-ti` PREEMPT_RT |
| Zephyr | MAIN-domain Cortex-R5F, remoteproc3 (`78400000.r5f`) |
| Zephyr board | `beagley_ai/j722s/main_r5f0_0` |

**LED baglantisi:** anot -> 330 ohm -> **header pin 31** (GPIO6), katot -> GND (pin 6)

**Seri konsol:** UART1 (`0x2810000`), pin 8 (TX) / 10 (RX) / 6 (GND), **115200 8N1**

### GPIO6'nin bes ismi (hepsi ayni pin)

| Referans | Deger |
| :--- | :--- |
| Linux | `gpiochip3` line 17 (`gpio-643`) |
| Zephyr node | `main_gpio1_0` offset 17 |
| Zephyr pinctrl | `hat_31_gpio` (pinmux `0x1bc`) |
| TI pad | `SPI0_CLK.GPIO1_17` (D20) |
| Fiziksel | 40-pin header **pin 31** |

## Kullanim

### Derle (host)

```bash
source ~/zephyrproject/.venv/bin/activate
cp -r zephyr_app ~/zephyrproject/led_ipc
cd ~/zephyrproject/zephyr
west build -p always -b beagley_ai/j722s/main_r5f0_0 ~/zephyrproject/led_ipc
```

Ciktida `RSC_TABLE` bolumu olmali:
```
       RSC_TABLE:          88 B         1 MB      0.01%
```

### Yukle (kart)

```bash
# host
scp ~/zephyrproject/build/zephyr/zephyr_openamp_rsc_table.elf gemstone@<IP>:/tmp/zephyr.elf
gcc -o led_ctrl led_ctrl.c && scp led_ctrl gemstone@<IP>:~/

# kart
sudo cp /tmp/zephyr.elf /lib/firmware/zephyr.elf
sudo reboot
```

> `echo stop > /sys/class/remoteproc/remoteproc3/state` **calismaz** (`-16 busy`).
> Sistem acilista symlink'in gosterdigi firmware'i otomatik baslatir. Reboot yeterli.

### Test

```bash
sudo ~/led_ctrl 1                      # endpoint ac -> /dev/rpmsg1 belirir
echo -n "1" | sudo tee /dev/rpmsg1     # LED yanar
echo -n "0" | sudo tee /dev/rpmsg1     # LED soner
```

Seri konsolda:
```
<inf> openamp_rsc_table: LED (GPIO6) hazir
<dbg> platform_ipm_callback: msg received from mb 1
<inf> openamp_rsc_table: LED ON (rx 0x31)
```

## Kritik Kconfig Ayarlari

Bunlar olmadan calismaz:

| Ayar | Neden |
| :--- | :--- |
| `CONFIG_OPENAMP_WITH_DCACHE=y` | Cache tutarliligi. Yoksa RPMsg el sikismasi **hic olmaz** |
| `CONFIG_PINCTRL=y` | Yoksa overlay'deki `pinctrl-0` **sessizce yok sayilir** |
| `CONFIG_LOG_BACKEND_UART=y` | Yoksa `LOG_INF` ciktisi hicbir yere gitmez |
| `CONFIG_SHELL=n` | Orijinal ornek konsolu RPMsg'e baglar -> firmware sessiz kalir |

## En Buyuk Tuzak

**Belirti:** Zephyr calisir, konsol `LED ON` der, LED yanmaz. Sonra Linux'tan bir kez
`sudo gpioset gpiochip3 17=1` calistirirsin — o andan itibaren Zephyr'in LED kontrolu
calismaya baslar. Reboot -> yine calismaz.

**Sebep:** `CONFIG_PINCTRL=y` yok. Overlay'deki `pinctrl-0` sessizce yok sayilir, Zephyr
pad'i hic ayarlamaz. `gpioset` pad'i MUX_MODE_7'ye alir ve ayar kalici oldugu icin
Zephyr "sanki calisiyormus gibi" gorunur. Calisan sey Zephyr degil, Linux'un biraktigi
pinmux ayaridir.

**Ders:** Bir sey ancak "baska bir sey once yapilirsa" calisiyorsa, o sey senin kodun
degildir. Reboot sonrasi **hicbir Linux GPIO komutu calistirmadan** test et.

Diger tuzaklar icin `docs/rpmsg_led.pdf` bolum 6'ya bak.

## Bundan Sonrasi

- **Motor kontrolu:** Tek byte yerine `struct` (komut tipi, hedef RPM) gonderip R5'te parse etmek
- **Goruntu isleme:** Buyuk buffer'lar RPMsg'den gecmez. `ddr0` shared memory'yi dogrudan
  kullanip RPMsg'i sadece "frame N hazir, su adreste" sinyali icin kullanmak (zero-copy)

## Referanslar

- Zephyr BeagleY-AI board: https://docs.zephyrproject.org/latest/boards/beagle/beagley_ai/
- Zephyr J722S R5 destegi PR: https://github.com/zephyrproject-rtos/zephyr/pull/80344
- TI AM67A: https://www.ti.com/product/AM67A
- Gemstone: https://docs.t3gemstone.org
- Temel alinan ornek: `zephyr/samples/subsys/ipc/openamp_rsc_table`
