ifneq ($(DEB_HOST_MULTIARCH),)
	CROSS_COMPILE ?= $(DEB_HOST_MULTIARCH)-
endif

ifeq ($(origin CC),default)
	CC := $(CROSS_COMPILE)gcc
endif
ifeq ($(origin CXX),default)
	CXX := $(CROSS_COMPILE)g++
endif

# extract Git revision and version number from debian/changelog
GIT_REVISION:=$(shell git rev-parse HEAD)
DEB_VERSION:=$(shell head -1 debian/changelog | awk '{ print $$2 }' | sed 's/[\(\)]//g')

TARGET = wb-mqtt-smartweb
SRC_DIRS ?= src

ifeq ($(DEBUG),)
	BUILD_DIR ?= build/release
	CMAKE_BUILD_TYPE=Release
else
	BUILD_DIR ?= build/debug
	CMAKE_BUILD_TYPE=Debug
endif

COMMON_SRCS := $(shell find $(SRC_DIRS) \( -name *.cpp -or -name *.c \) -and -not -name main.cpp)
COMMON_OBJS := $(COMMON_SRCS:%=$(BUILD_DIR)/%.o)

LDFLAGS = -lwbmqtt1 -lpthread
CXXFLAGS = -std=c++17 -Wall -Werror -I$(SRC_DIRS) -DWBMQTT_COMMIT="$(GIT_REVISION)" -DWBMQTT_VERSION="$(DEB_VERSION)" -Wno-psabi
CFLAGS = -Wall -I$(SRC_DIR)

ifeq ($(DEBUG),)
	CXXFLAGS += -O2
else
	CXXFLAGS += -g -O0 -fprofile-arcs -ftest-coverage -ggdb
	LDFLAGS += -lgcov
endif


TEST_DIR = test
TEST_SRCS := $(shell find $(TEST_DIR) \( -name *.cpp -or -name *.c \) -and -not -name main.cpp)
TEST_OBJS := $(TEST_SRCS:%=$(BUILD_DIR)/%.o)
TEST_TARGET = test-app
TEST_LDFLAGS = -lgtest -lwbmqtt_test_utils

all: $(TARGET)

$(TARGET): $(COMMON_OBJS) $(BUILD_DIR)/src/main.cpp.o
	${CXX} -o $(BUILD_DIR)/$@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.c.o: %.c
	${CC} -c $< -o $@ ${CFLAGS}

$(BUILD_DIR)/%.cpp.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/test/%.o: test/%.cpp
	$(CXX) -c $(CXXFLAGS) -o $@ $^

test: $(TEST_DIR)/$(TEST_TARGET)
	if [ "$(shell arch)" != "armv7l" ] && [ "$(CROSS_COMPILE)" = "" ] || [ "$(CROSS_COMPILE)" = "x86_64-linux-gnu-" ]; then \
		valgrind --error-exitcode=180 -q $(TEST_DIR)/$(TEST_TARGET) || \
		if [ $$? = 180 ]; then \
			echo "*** VALGRIND DETECTED ERRORS ***" 1>& 2; \
			exit 1; \
		fi \
	else \
		$(TEST_DIR)/$(TEST_TARGET); \
	fi

$(TEST_DIR)/$(TEST_TARGET): $(TEST_OBJS) $(COMMON_OBJS) $(BUILD_DIR)/test/main.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS) $(TEST_LDFLAGS) -fno-lto

clean:
	rm -rf $(BUILD_DIR) $(TEST_DIR)/*.o $(TEST_DIR)/$(TEST_TARGET)

install:
	install -d $(DESTDIR)/var/lib/wb-mqtt-smartweb
	install -d $(DESTDIR)/etc/wb-mqtt-smartweb.conf.d/classes
	install -d $(DESTDIR)/usr/share/wb-mqtt-smartweb/classes
	install -D -m 0644  wb-mqtt-smartweb.schema.json $(DESTDIR)/usr/share/wb-mqtt-confed/schemas/wb-mqtt-smartweb.schema.json
	install -D -m 0644  wb-mqtt-smartweb-class.schema.json $(DESTDIR)/usr/share/wb-mqtt-confed/schemas/wb-mqtt-smartweb-class.schema.json
	install -D -m 0644  config.json $(DESTDIR)/etc/wb-mqtt-smartweb.conf
	install -D -m 0755 $(BUILD_DIR)/$(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	install -D -m 0644  wb-mqtt-smartweb.wbconfigs $(DESTDIR)/etc/wb-configs.d/21wb-mqtt-smartweb
	cp -r classes $(DESTDIR)/usr/share/wb-mqtt-smartweb

.PHONY: all test clean
