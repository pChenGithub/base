# netmgr 示例程序构建脚本
#   net_demo      整体流程示例（api + core + sys 全量）
#   demo_core     仲裁层示例：以太网 + WiFi 双链路，交互式演示 core 全部接口
#   demo_iface    sys 层接口模块独立示例
#   demo_route    sys 层路由模块独立示例
#   demo_dhcp     sys 层 DHCP 模块独立示例
#   demo_proc     sys 层进程模块独立示例
#   demo_dns      sys 层 DNS 模块独立示例
#   demo_event    sys 层事件模块独立示例
#
# 本地编译：  make                  构建全部示例
# 单个示例：  make demo_event       只构建指定示例（体现下层可独立编译）
# 交叉编译：  make CC=arm-linux-gnueabihf-gcc
# 清理：      make clean

CC ?= gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -g -O0
INC = -I../api -I../core -I../sys -I../common
LIBS = -lpthread

FULL_SRCS = ../api/net_mgr.c \
	../core/net_mgr_core.c \
	../core/net_mgr_link.c \
	../core/net_mgr_eth.c \
	../core/net_mgr_wifi.c \
	../core/net_mgr_cell.c \
	../sys/net_mgr_iface.c \
	../sys/net_mgr_route.c \
	../sys/net_mgr_dhcp.c \
	../sys/net_mgr_proc.c \
	../sys/net_mgr_dns.c \
	../sys/net_mgr_wpa.c \
	../sys/net_mgr_event.c \
	../common/net_mgr_util.c

CORE_SRCS = ../core/net_mgr_core.c \
	../core/net_mgr_link.c \
	../core/net_mgr_eth.c \
	../core/net_mgr_wifi.c \
	../core/net_mgr_cell.c \
	../sys/net_mgr_iface.c \
	../sys/net_mgr_route.c \
	../sys/net_mgr_dhcp.c \
	../sys/net_mgr_proc.c \
	../sys/net_mgr_dns.c \
	../sys/net_mgr_wpa.c \
	../sys/net_mgr_event.c \
	../common/net_mgr_util.c

IFACE_SRCS = ../sys/net_mgr_iface.c ../common/net_mgr_util.c
ROUTE_SRCS = ../sys/net_mgr_route.c ../common/net_mgr_util.c
DHCP_SRCS = ../sys/net_mgr_dhcp.c
PROC_SRCS = ../sys/net_mgr_proc.c
DNS_SRCS = ../sys/net_mgr_dns.c ../common/net_mgr_util.c
EVENT_SRCS = ../sys/net_mgr_event.c
WPA_SRCS = ../sys/net_mgr_wpa.c

ALL_TARGETS = net_demo demo_core demo_iface demo_route demo_dhcp demo_proc demo_dns demo_event demo_wpa

all: $(ALL_TARGETS)

net_demo: net_demo.c $(FULL_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ net_demo.c $(FULL_SRCS) $(LIBS)

demo_core: demo_core.c $(CORE_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_core.c $(CORE_SRCS) $(LIBS)

demo_iface: demo_iface.c $(IFACE_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_iface.c $(IFACE_SRCS) $(LIBS)

demo_route: demo_route.c $(ROUTE_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_route.c $(ROUTE_SRCS) $(LIBS)

demo_dhcp: demo_dhcp.c $(DHCP_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_dhcp.c $(DHCP_SRCS) $(LIBS)

demo_proc: demo_proc.c $(PROC_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_proc.c $(PROC_SRCS) $(LIBS)

demo_dns: demo_dns.c $(DNS_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_dns.c $(DNS_SRCS) $(LIBS)

demo_event: demo_event.c $(EVENT_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_event.c $(EVENT_SRCS) $(LIBS)

demo_wpa: demo_wpa.c $(WPA_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_wpa.c $(WPA_SRCS) $(LIBS)

clean:
	rm -f $(ALL_TARGETS)

.PHONY: all clean
