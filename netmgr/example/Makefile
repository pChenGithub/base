# netmgr 示例程序构建脚本
# 本地编译：  make
# 交叉编译：  make CC=arm-linux-gnueabihf-gcc
# 清理：      make clean

CC ?= gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -g -O0
INC = -I..
LIBS = -lpthread
TARGET = net_demo

SRCS = net_demo.c \
	../net_mgr.c \
	../net_mgr_link.c \
	../net_mgr_proc.c \
	../net_mgr_dhcp.c \
	../net_mgr_dns.c \
	../net_mgr_eth.c \
	../net_mgr_wifi.c \
	../net_mgr_cell.c \
	../net_mgr_route.c \
	../net_mgr_event.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
