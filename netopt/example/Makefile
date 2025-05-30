pro ?= test
pro_nl ?= test_nl
pro_gate ?= test_gate
pro_dns ?= test_dns
pro_link ?= test_link
pro_dhcp ?= test_dhcp
pro_wifi ?= pro_wifi
#
pro_multicast_recv ?= test_nulticast_recv
#pro_multicast_send ?= test_nulticast_send

pro_raw_socket_recv ?= test_raw_socket_recv

#CC = /home/zhh/work/YT312/02_YT312门禁控制器/gcc-linaro-arm-linux-gnueabihf-4.7-2013.03-20130313_linux/bin/arm-linux-gnueabihf-gcc
#CC = /home/zhh/work/YT312/02_YT312门禁控制器/gcc-linaro-arm-linux-gnueabihf-4.7-2013.03-20130313_linux/bin/arm-linux-gnueabihf-gcc
#CC = /home/zhh/work/yt328_reader/020.源代码/toolchain/gcc-linaro-5.5.0-2017.10-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc
CC = /media/cp/1f4ae5c9-7b98-4e3d-b846-2a0bd3c8cfa2/home/zhh/work/yt815/数智化中枢网关/gcc-linaro-10.2.1-2021.01-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc
#ARCH=arm
#CFLAGS += -L/home/zhh/work/YT312/02_YT312门禁控制器/gcc-linaro-arm-linux-gnueabihf-4.7-2013.03-20130313_linux/arm-linux-gnueabihf/libc/lib/arm-linux-gnueabihf -lpthread
#CFLAGS += -L/home/zhh/work/yt328_reader/020.源代码/toolchain/gcc-linaro-5.5.0-2017.10-x86_64_arm-linux-gnueabihf/arm-linux-gnueabihf/libc/lib -lpthread

# x86
#LINK_LIBS += -lpthread


CFLAGS += -I../exeShell_obj/
SOURCE_FILE += link/net_link.c link/net_opt.c link/net_netlink.c link/net_dns.c link/net_dhcp.c ../exeShell_obj/mixShell.c
all: ${pro_wifi}
#	$(DEBUGGER) ./${pro_multicast_recv} ${ARGS}

${pro}: test.c ${SOURCE_FILE} ${LINK_LIBS}

${pro_nl} : test_nl.c ${SOURCE_FILE} ${LINK_LIBS}

${pro_gate} : test_gate.c ${SOURCE_FILE} ${LINK_LIBS}

${pro_dns} : test_dns.c ${SOURCE_FILE} ${LINK_LIBS}

${pro_link} : test_link.c ${SOURCE_FILE} ${LINK_LIBS}

${pro_dhcp} : test_dhcp.c ${SOURCE_FILE} ${LINK_LIBS}

#
TCPIP_SOURCE += tcpIp/multicast.c
${pro_multicast_recv} : ${TCPIP_SOURCE} ${LINK_LIBS}

${pro_multicast_send} : ${TCPIP_SOURCE} ${LINK_LIBS}

TCPIP_SOURCE2 += tcpIp/raw_socket.c
${pro_raw_socket_recv} : ${TCPIP_SOURCE2} ${LINK_LIBS}

#
CFLAGS += -I../ -I../../file -lpthread
${pro_wifi}: test_wifi.c ../wifi_opt.c ../wpas/os_unix.c ../wpas/wpa_ctrl.c ../../file/file_opt.c ../net_opt.c ../net_netlink.c
	${CC} -o $@ $^ ${CFLAGS}
	echo "gcc"


clean:
	rm ${pro} ${pro_nl} ${pro_gate} ${pro_dns} ${pro_link} ${pro_dhcp} 
	rm ${pro_multicast_recv} ${pro_multicast_send}
	rm ${pro_raw_socket_recv}
	rm ${pro_wifi}


install:
	cp ${pro_wifi} ~/nfs
