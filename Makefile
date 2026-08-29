CC = clang
CFLAGS = -Wall -Wextra -I$(INC_DIR)

OUT_DIR = ./build
SRC_DIR = ./src
INC_DIR = ./include

TARGET = $(OUT_DIR)/watcher

SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OUT_DIR)/%.o,$(SOURCES))
DEPS = $(OBJECTS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS)

$(OUT_DIR)/%.o: $(SRC_DIR)/%.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT_DIR):
	@mkdir -p $(OUT_DIR)

-include $(DEPS)

clean:
	rm -rf $(OUT_DIR)

.PHONY: all clean
