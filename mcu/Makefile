# GL-C-301W MS51FC0AE firmware - SDCC build
#   make            -> build/gl_c_301w_ms51.hex / .bin
#   make test       -> host unit test of the TuyaMCU protocol layer
# Needs Nuvoton's MS51 BSP for the register headers: ./get_bsp.sh

BSP_DIR  ?= bsp/MS51_BSP/MS51FC0AE_MS51XC0BE_MS51EB0AE_MS51EC0AE_MS51TC0AE_MS51PC0AE
SDCC     ?= sdcc
BUILD    := build
TARGET   := gl_c_301w_ms51

INC      := -Isrc -I$(BSP_DIR)/Library/Device/Include -I$(BSP_DIR)/Library/StdDriver/inc
CFLAGS   := -D__SDCC__ -mmcs51 --model-small --opt-code-size --std-sdcc11 $(INC)
LDFLAGS  := -mmcs51 --model-small --iram-size 256 --xram-size 2048 --code-size 32768

SRCS     := src/main.c src/proto.c
RELS     := $(patsubst src/%.c,$(BUILD)/%.rel,$(SRCS))

all: $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

$(BUILD)/%.rel: src/%.c src/config.h src/proto.h | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).ihx: $(RELS)
	$(SDCC) $(LDFLAGS) $(RELS) -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).ihx
	packihx $< > $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).ihx
	makebin -p $< $@

$(BUILD):
	mkdir -p $(BUILD)

test:
	mkdir -p $(BUILD)
	cc -Wall -Wextra -O1 -Isrc -o $(BUILD)/test_proto test/test_proto.c src/proto.c
	./$(BUILD)/test_proto

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
