KART_IP    ?= 192.168.7.2
KART_USER  ?= gemstone
ZEPHYR_APP  = $(HOME)/zephyrproject/led_ipc
ZEPHYR_DIR  = $(HOME)/zephyrproject/zephyr
BUILD_ELF   = $(ZEPHYR_DIR)/build/zephyr/zephyr_openamp_rsc_table.elf 
BOARD       = beagley_ai/j722s/main_r5f0_0

.PHONY: sync build fw client reboot deploy clean

sync:
	cp -f zephyr_app/prj.conf $(ZEPHYR_APP)/
	cp -f zephyr_app/src/main_remote.c $(ZEPHYR_APP)/src/
	cp -f zephyr_app/boards/*.overlay $(ZEPHYR_APP)/boards/
	@echo "--> kaynak senkronize edildi"

build: sync
	cd $(ZEPHYR_DIR) && west build -p always -b $(BOARD) $(ZEPHYR_APP)

fw: build
	scp $(BUILD_ELF) $(KART_USER)@$(KART_IP):/tmp/zephyr.elf
	ssh $(KART_USER)@$(KART_IP) 'sudo cp /tmp/zephyr.elf /lib/firmware/zephyr.elf'
	@echo "--> firmware yuklendi"

client:
	gcc -o /tmp/led_ctrl led_ctrl.c
	scp /tmp/led_ctrl $(KART_USER)@$(KART_IP):~/
	@echo "--> led_ctrl yuklendi"

reboot:
	-ssh $(KART_USER)@$(KART_IP) 'sudo reboot'
	@echo "--> reboot verildi, ~30sn bekle"

deploy: fw client reboot

clean:
	rm -rf $(ZEPHYR_DIR)/build
