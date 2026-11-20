CC := clang
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS := -I../../remote -I../../utils/crc
SOURCES := ../../remote/remote_core.c ../../remote/remote_wire.c ../../remote/remote_button.c ../../utils/crc/custom_crc16.c ../../utils/crc/custom_crc32.c
BUILD := ../../build-host
.PHONY: test clean
$(BUILD):
	mkdir -p $@
$(BUILD)/test_remote: test_remote.c $(SOURCES) $(wildcard ../../remote/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) test_remote.c $(SOURCES) -o $@
$(BUILD)/test_wire: test_wire.c ../../remote/remote_wire.c ../../remote/remote_wire.h ../../utils/crc/custom_crc16.c ../../utils/crc/custom_crc32.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(filter %.c,$^) -o $@
$(BUILD)/test_ble: test_ble.c fake_idf/fake_idf.c ../../remote/remote_ble.c $(SOURCES) $(wildcard ../../remote/*.h fake_idf/*.h fake_idf/freertos/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ifake_idf test_ble.c fake_idf/fake_idf.c ../../remote/remote_ble.c $(SOURCES) -o $@
test: $(BUILD)/test_remote $(BUILD)/test_wire $(BUILD)/test_ble
	$(BUILD)/test_remote
	$(BUILD)/test_wire
	$(BUILD)/test_ble flow
	$(BUILD)/test_ble scan_error
	$(BUILD)/test_ble immediate_error
	$(BUILD)/test_ble cancel_race
	$(BUILD)/test_ble adv_config_error
	$(BUILD)/test_ble adv_start_error
	$(BUILD)/test_ble adv_immediate_error
	$(BUILD)/test_ble queue_overflow
	$(BUILD)/test_ble close_error
	$(BUILD)/test_ble lost_callback
clean:
	rm -rf $(BUILD)
