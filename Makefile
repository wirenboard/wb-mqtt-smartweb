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

CXXFLAGS= -std=c++0x -Wall -Werror
LDFLAGS= -ljsoncpp -lwbmqtt

SRC:=main.cpp

OBJ=$(SRC:.cpp=.o)

all: wb-mqtt-smartweb

%.o: %.cpp
	${CXX} -c $< -o $@ ${CFLAGS}

wb-mqtt-smartweb: $(OBJ)
	${CXX} $^ ${LDFLAGS} -o $@ $(TEST_LIBS) $(SERIAL_LIBS)
