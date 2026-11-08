# netmgr 示例程序构建脚本
#   net_demo      整体流程示例（api + core + sys 全量）
#   demo_core     仲裁层示例：以太网 + WiFi 双链路，交互式演示 core 全部接口
#   demo_iface    sys 层接口模块独立示例
#   demo_route    sys 层路由模块独立示例
#   demo_dhcp     sys 层 DHCP 模块独立示例
#   demo_proc     common 层进程模块独立示例
#   demo_dns      sys 层 DNS 模块独立示例
#   demo_event    sys 层事件模块独立示例
#
# 本地编译：  make                  构建全部示例
# 单个示例：  make demo_event       只构建指定示例（体现下层可独立编译）
# 交叉编译：  make CC=arm-linux-gnueabihf-gcc
# 清理：      make clean

#CC ?= gcc
CC = /media/cp/1f4ae5c9-7b98-4e3d-b846-2a0bd3c8cfa2/home/zhh/work/yt815/数智化中枢网关/gcc-linaro-10.2.1-2021.01-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -g -O0
INC = -I../api -I../core -I../sys -I../common
LIBS = -lpthread

FULL_SRCS = ../api/net_mgr.c \
	../core/net_mgr_core.c \
	../core/net_mgr_arbiter.c \
	../core/net_mgr_link.c \
	../core/net_mgr_eth.c \
	../core/net_mgr_wifi.c \
	../core/net_mgr_cell.c \
	../sys/net_mgr_iface.c \
	../sys/net_mgr_route.c \
	../sys/net_mgr_dhcp.c \
	../common/net_mgr_proc.c \
	../sys/net_mgr_dns.c \
	../sys/net_mgr_wpa.c \
	../sys/net_mgr_ap.c \
	../sys/net_mgr_event.c \
	../common/net_mgr_util.c

CORE_SRCS = ../core/net_mgr_core.c \
	../core/net_mgr_arbiter.c \
	../core/net_mgr_link.c \
	../core/net_mgr_eth.c \
	../core/net_mgr_wifi.c \
	../core/net_mgr_cell.c \
	../sys/net_mgr_iface.c \
	../sys/net_mgr_route.c \
	../sys/net_mgr_dhcp.c \
	../common/net_mgr_proc.c \
	../sys/net_mgr_dns.c \
	../sys/net_mgr_wpa.c \
	../sys/net_mgr_ap.c \
	../sys/net_mgr_event.c \
	../common/net_mgr_util.c

IFACE_SRCS = ../sys/net_mgr_iface.c ../common/net_mgr_util.c
ROUTE_SRCS = ../sys/net_mgr_route.c ../common/net_mgr_util.c
DHCP_SRCS = ../sys/net_mgr_dhcp.c
PROC_SRCS = ../common/net_mgr_proc.c
DNS_SRCS = ../sys/net_mgr_dns.c ../common/net_mgr_util.c
EVENT_SRCS = ../sys/net_mgr_event.c
# wpa 自管 wpa_supplicant 进程，依赖 common 层进程控制（向下依赖，不引入 sys 内横向依赖）
WPA_SRCS = ../sys/net_mgr_wpa.c ../common/net_mgr_proc.c
# AP 模块只依赖 common 层（进程控制），可单文件编入
AP_SRCS = ../sys/net_mgr_ap.c ../common/net_mgr_proc.c

ALL_TARGETS = net_demo demo_core demo_iface demo_route demo_dhcp demo_proc demo_dns demo_event demo_wpa demo_ap

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

demo_ap: demo_ap.c $(AP_SRCS)
	$(CC) $(CFLAGS) $(INC) -o $@ demo_ap.c $(AP_SRCS) $(LIBS)

clean:
	rm -f $(ALL_TARGETS)

.PHONY: all clean
