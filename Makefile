# compiler/flags
CC = gcc
CFLAGS = -pthread -Wall -g -MMD -MP -I./src

# specify source/build directories
SRC_DIR = src
BUILD_DIR = build

# find sources
SRC = $(wildcard $(SRC_DIR)/*.c)
OBJ = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC))
DEP = $(OBJ:.o=.d)

# target executable
TARGET = $(BUILD_DIR)/server

# default rule should build executable
all: $(TARGET)

# linking
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET)

# compile objects
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# create build directory if necessary
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# dependencies
-include $(DEP)

# clean up build directory
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)