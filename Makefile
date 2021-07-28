SOURCE_DIR=src
HEADER_DIR=include
INTERFACE_DIR=interface
GENERATED_DIR=generated
BUILD_DIR=out

SOURCES=$(wildcard $(SOURCE_DIR)/*.c)
OBJECTS=$(patsubst $(SOURCE_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
GENERATED_IFACES=$(patsubst $(INTERFACE_DIR)/%.xml,\
							$(GENERATED_DIR)/%.c,\
							$(wildcard $(INTERFACE_DIR)/*.xml))
OBJECTS+=$(patsubst $(GENERATED_DIR)/%.c,$(BUILD_DIR)/%.o,$(GENERATED_IFACES))

TARGET=mdrd

LIBMDR_DIR=libmdr
LIBMDR=$(LIBMDR_DIR)/libmdr.a

CFLAGS=$(shell pkg-config --cflags gio-2.0 gio-unix-2.0) \
	   -g \
	   -I $(GENERATED_DIR) \
	   -I $(HEADER_DIR) \
	   -I ./ \
	   -Ilibmdr/include
LDFLAGS=$(shell pkg-config --libs gio-2.0 gio-unix-2.0) \
		-g

all: $(TARGET)

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(GENERATED_DIR)
	rm -f $(TARGET)
	cd $(LIBMDR_DIR) && make clean

$(TARGET): $(OBJECTS) $(LIBMDR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/main.o: $(SOURCE_DIR)/main.c | $(BUILD_DIR) $(GENERATED_IFACES)
	$(CC) $(CFLAGS) -c -I $(HEADER_DIR) -o $@ $<

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c | $(HEADER_DIR)/%.h $(BUILD_DIR) $(GENERATED_IFACES)
	$(CC) $(CFLAGS) -c -I $(HEADER_DIR) -o $@ $<

$(BUILD_DIR)/%.o: $(GENERATED_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -I $(HEADER_DIR) -o $@ $<

$(GENERATED_DIR)/%.c: $(INTERFACE_DIR)/%.xml | $(GENERATED_DIR)
	gdbus-codegen $< --generate-c-code $(patsubst %.c,%,$@)

$(LIBMDR):
	cd $(LIBMDR_DIR) && make

$(BUILD_DIR):
	mkdir -p $@

$(GENERATED_DIR):
	mkdir -p $@
