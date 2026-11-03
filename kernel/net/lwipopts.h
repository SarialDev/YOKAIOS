#pragma once
// Optiunile lwIP 2.2.1 pentru YokaiOS (T9.2). lwIP e vendorizat neschimbat in third_party/lwip; tot ce e specific
// portului sta aici si in arch/{cc,sys_arch}.h. Fara IPv6.

#define NO_SYS                      0
#define LWIP_TIMERS                 1

// Protocoale: ARP, IPv4, ICMP, UDP, TCP (+ RAW pentru ping din kernel)
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define ARP_QUEUEING                1       // pachetul care a declansat cererea ARP se trimite dupa raspuns
#define LWIP_ICMP                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define LWIP_DHCP_PROVIDE_DNS_SERVERS 2    // serverele DNS din oferta DHCP intra in lwIP (netcfg le scrie in resolv.conf)
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    1       // doar pentru a pastra serverele; rezolvarea de nume o face libc (UDP direct)
#define DNS_MAX_SERVERS             2
#define LWIP_NETIF_LOOPBACK         0
#define LWIP_HAVE_LOOPIF            0

// API: netconn + socket-uri BSD (prefix lwip_, fara redefinirea numelor POSIX)
#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1
#define LWIP_COMPAT_SOCKETS         0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SO_RCVTIMEO            1
#define LWIP_SO_SNDTIMEO            1
#define LWIP_SO_RCVBUF              1
#define LWIP_SO_LINGER              0
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_API              1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define SO_REUSE                    1
#define LWIP_STATS                  0
#define LWIP_PROVIDE_ERRNO          1

// Nucleul: thread tcpip separat, comunicare prin mailbox-uri (fara core locking)
#define LWIP_TCPIP_CORE_LOCKING     0
#define TCPIP_THREAD_NAME           "tcpip"
#define TCPIP_THREAD_STACKSIZE      16384
#define TCPIP_THREAD_PRIO           1       // NORMAL ridicat: scheduler-ul il pune primul in coada NORMAL si preempteaza NORMAL-ul obisnuit
#define TCPIP_MBOX_SIZE             64
#define DEFAULT_RAW_RECVMBOX_SIZE   16
#define DEFAULT_UDP_RECVMBOX_SIZE   32
#define DEFAULT_TCP_RECVMBOX_SIZE   32
#define DEFAULT_ACCEPTMBOX_SIZE     16
#define DEFAULT_THREAD_STACKSIZE    16384
#define DEFAULT_THREAD_PRIO         0

// Sectiuni critice scurte (pbuf/memp), apelabile si din IRQ / task-ul de interogare al placii: sys_arch_protect
#define SYS_LIGHTWEIGHT_PROT        1
#define LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT 0
#define MEMCPY(dst, src, len)       __builtin_memcpy(dst, src, len)
#define SMEMCPY(dst, src, len)      __builtin_memcpy(dst, src, len)

// Memorie: heap-ul lwIP si pool-urile sunt statice (BSS)
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (192 * 1024)
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_UDP_PCB            12
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_TCP_PCB_LISTEN     8
#define MEMP_NUM_TCP_SEG            64
#define MEMP_NUM_NETCONN            16
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_SYS_TIMEOUT        16
#define MEMP_NUM_TCPIP_MSG_API      16
#define MEMP_NUM_TCPIP_MSG_INPKT    64
#define PBUF_POOL_SIZE              64      // cadrele primite din IRQ / poller se pun in pbuf-uri din pool (fara heap)
#define PBUF_POOL_BUFSIZE           LWIP_MEM_ALIGN_SIZE(TCP_MSS + 40 + PBUF_LINK_ENCAPSULATION_HLEN + PBUF_LINK_HLEN)
#define ETH_PAD_SIZE                0
#define LWIP_CHKSUM_ALGORITHM       2

// TCP
#define TCP_MSS                     1460
#define TCP_WND                     (16 * TCP_MSS)
#define TCP_SND_BUF                 (16 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define LWIP_TCP_SACK_OUT           0
#define LWIP_WND_SCALE              0

// Checksum-urile le face software (placa nu are offload)
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1
