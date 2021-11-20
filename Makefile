SOURCE_DIR=src
HEADER_DIR=include
INTERFACE_DIR=interface
GENERATED_DIR=generated
BUILD_DIR=out

SOURCES=$(wildcard $(SOURCE_DIR)/*.c)
OBJECTS=$(patsubst $(SOURCE_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

INTERFACES=$(wildcard $(INTERFACE_DIR)/*.xml)
GENERATED_INTERFACE_HEADERS=$(patsubst $(INTERFACE_DIR)/%.xml,$(GENERATED_DIR)/%.h,$(INTERFACES))
GENERATED_INTERFACE_SOURCES=$(patsubst $(INTERFACE_DIR)/%.xml,$(GENERATED_DIR)/%.c,$(INTERFACES))
OBJECTS+=$(patsubst $(GENERATED_DIR)/%.c,$(BUILD_DIR)/%.o,$(GENERATED_INTERFACE_SOURCES))

TARGET=mdrd

LIBMDR_DIR=libmdr
LIBMDR=$(LIBMDR_DIR)/libmdr.a

CFLAGS=$(shell pkg-config --cflags gio-2.0 gio-unix-2.0) \
	   -Wall \
	   -Wpedantic \
	   -g \
	   -I $(GENERATED_DIR) \
	   -I $(HEADER_DIR) \
	   -Ilibmdr/include
LDFLAGS=$(shell pkg-config --libs gio-2.0 gio-unix-2.0) \
		-g

GDBUS_CODEGEN=$(shell pkg-config --variable=gdbus_codegen gio-2.0)

all: $(TARGET)

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(GENERATED_DIR)
	rm -f $(TARGET)
	cd $(LIBMDR_DIR) && make clean

$(TARGET): $(OBJECTS) $(LIBMDR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# $(BUILD_DIR)/main.o: $(SOURCE_DIR)/main.c | $(BUILD_DIR) $(GENERATED_IFACES) $(LIBMDR)
# 	$(CC) $(CFLAGS) -c -I $(HEADER_DIR) -o $@ $<

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c | $(HEADER_DIR)/%.h $(BUILD_DIR) $(GENERATED_INTERFACE_HEADERS) $(LIBMDR)
	$(CC) $(CFLAGS) -c -I $(HEADER_DIR) -o $@ $<

$(BUILD_DIR)/%.o: $(GENERATED_DIR)/%.c | $(BUILD_DIR) $(LIBMDR) $(GENERATED_INTERFACE_HEADERS)
	$(CC) $(CFLAGS) -c -I $(HEADER_DIR) -o $@ $<

$(GENERATED_DIR)/%.h: $(INTERFACE_DIR)/%.xml | $(GENERATED_DIR)
	$(GDBUS_CODEGEN) $< --header --output $@

$(GENERATED_DIR)/%.c: $(INTERFACE_DIR)/%.xml | $(GENERATED_DIR)
	$(GDBUS_CODEGEN) $< --body --output $@

$(LIBMDR): .FORCE $(LIBMDR_DIR)
	cd $(LIBMDR_DIR) && make

$(LIBMDR_DIR):
	git clone -b 'v0.5' --depth 1 https://github.com/AndreasOlofsson/libmdr

.PRECIOUS: $(GENERATED_INTERFACE_HEADERS)

.FORCE:

.PHONY: all clean

$(BUILD_DIR):
	mkdir -p $@

$(GENERATED_DIR):
	mkdir -p $@
