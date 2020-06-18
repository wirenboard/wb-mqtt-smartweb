ifeq ($(DEB_TARGET_ARCH),armel)
CROSS_COMPILE=arm-linux-gnueabi-
endif

CXX=$(CROSS_COMPILE)g++
CXX_PATH := $(shell which $(CROSS_COMPILE)g++-4.7)

CC=$(CROSS_COMPILE)gcc
CC_PATH := $(shell which $(CROSS_COMPILE)gcc-4.7)

ifneq ($(CXX_PATH),)
	CXX=$(CROSS_COMPILE)g++-4.7
endif

ifneq ($(CC_PATH),)
	CC=$(CROSS_COMPILE)gcc-4.7
endif

CXXFLAGS= -std=c++11 -Wall -ggdb
LDFLAGS= -ljsoncpp -lwbmqtt1 -lmosquittopp -lmosquitto -ldb_cxx

SRC:=main.cpp

OBJ=$(SRC:.cpp=.o)
BIN=wb-mqtt-smartweb

all: $(BIN)

%.o: %.cpp
	${CXX} -c $< -o $@ ${CXXFLAGS}

$(BIN): $(OBJ)
	${CXX} $^ ${LDFLAGS} -o $@ $(TEST_LIBS) $(SERIAL_LIBS)

clean:
	rm -f $(OBJ)
	rm -f $(BIN)

install: all
	mkdir -p $(DESTDIR)/usr/share/wb-mqtt-confed/schemas

	install -d $(DESTDIR)
	install -d $(DESTDIR)/etc
	install -d $(DESTDIR)/usr/bin

	install -m 0644  wb-mqtt-smartweb.schema.json $(DESTDIR)/usr/share/wb-mqtt-confed/schemas/wb-mqtt-smartweb.schema.json
	install -m 0644  config.json $(DESTDIR)/etc/wb-mqtt-smartweb.conf.sample
	install -m 0755  $(BIN) $(DESTDIR)/usr/bin/$(BIN)
