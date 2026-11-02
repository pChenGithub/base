# netmgr 示例程序构建脚本
# 本地编译：  make
# 交叉编译：  make CC=arm-linux-gnueabihf-gcc
# 清理：      make clean

CC ?= gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -g -O0
INC = -I../api -I../core -I../linktype -I../sys -I../common
LIBS = -lpthread
TARGET = net_demo

SRCS = net_demo.c \
	../api/net_mgr.c \
	../core/net_mgr_core.c \
	../linktype/net_mgr_link.c \
	../linktype/net_mgr_eth.c \
	../linktype/net_mgr_wifi.c \
	../linktype/net_mgr_cell.c \
	../sys/net_mgr_iface.c \
	../sys/net_mgr_route.c \
	../sys/net_mgr_dhcp.c \
	../sys/net_mgr_proc.c \
	../sys/net_mgr_dns.c \
	../sys/net_mgr_event.c \
	../common/net_mgr_util.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
