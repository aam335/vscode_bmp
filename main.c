#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <string.h>
#include <libusb-1.0/libusb.h>
#include <stdint.h>
#include <limits.h>
#include <termios.h>
#include <signal.h>

#include <stdio.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>

#include "coroutines.h"

#define VID (0x1d50)
#define PID (0x6018)

#define INTERFACE (5)
#define ENDPOINT (0x85)

#define TRANSFER_SIZE (64)

#define MAX_RECEIVERS 64
#define MAX_PACKET_LEN 5

#define PORT 6018
#define SA struct sockaddr


#define ERROR_CREATE_THREAD -11
#define ERROR_JOIN_THREAD   -12
#define SUCCESS               0

uint32_t crStaticVar_pp = 0;
uint32_t pp(int c);
pthread_mutex_t lock;

// Record for options, either defaults or from command line
struct
{
    bool verbose;
    bool dump;
    //   int nChannels;
    //   char *chanPath;
    //   char *port;
    int speed;
} options = {.speed = 115200};

int fds[MAX_RECEIVERS];

void *tcp_server(void *arg);

pthread_t tcplistener;

int usb_feeder(void)
{
    unsigned char cbw[TRANSFER_SIZE];
    libusb_device_handle *handle;
    libusb_device *dev;
    int size;

    while (1)
    {
        if (libusb_init(NULL) < 0)
        {
            fprintf(stderr, "Failed to initalise USB interface\n");
            return (-1);
        }

        while (!(handle = libusb_open_device_with_vid_pid(NULL, VID, PID)))
        {
            usleep(500000);
        }

        if (!(dev = libusb_get_device(handle)))
            continue;

        if (libusb_claim_interface(handle, INTERFACE) < 0)
            continue;

        while (0 == libusb_bulk_transfer(handle, ENDPOINT, cbw, TRANSFER_SIZE, &size, 10))
        {
            unsigned char *c = cbw;
            if (options.dump)
            {
                cbw[size] = 0;
                printf("%s", (char *)cbw);
            }
            else
                while (size--)
                    pp(*c++);
        }

        libusb_close(handle);
    }
    return 0;
}

int main()
{
    bzero(&fds, sizeof(fds) * sizeof(int));
    if (pthread_mutex_init(&lock, NULL) != 0)
    {
        printf("\n mutex init failed\n");
        return 1;
    }
    int err = pthread_create(&tcplistener, NULL, tcp_server, NULL);
    if (err != 0)
    {
        printf("\ncan't create thread :[%s]", strerror(err));
        exit(ERROR_CREATE_THREAD);
    }
    err = pthread_join(tcplistener, NULL);
    if (err != SUCCESS)
    {
        printf("\ncan't join thread :[%s]", strerror(err));
        exit(ERROR_JOIN_THREAD);
    }
    exit(usb_feeder());
}

void put_packet();

#ifdef crStaticVar
#undef crStaticVar
#endif
#define crStaticVar crStaticVar_pp

uint8_t rxPacket[MAX_PACKET_LEN];
int rx_ptr, rx_count;
bool rx0_x80_term;
uint32_t pp(int c)
{
    crBegin;
    rx_count = 0;
    rx_ptr = 0;
    rx0_x80_term = false;

    // identify packet
    if (c == 0b01110000)
    {
        fprintf(stderr, "Overflow!\n");
        return 0;
    }
    if (c & 0x4)
    { // bit 4 is unused
        return 0;
    }

    rxPacket[rx_ptr++] = c;
    if (c == 0) // sync
    {
        rx_count = 5;
    }
    else if ((c & 0xf) == 0)
    { // timestamp ()
        if ((c & 0x80) == 0)
        {
            put_packet();
            return 0;
        }
        rx0_x80_term = true;
    }
    else
    { // SWIT
        rx_count = ((c & 0x03) == 0x03 ? 4 : c & 0x03) + 1;
    }
    crReturn(0);
    // read packet data

    if (rx_ptr >= MAX_PACKET_LEN)
    { // error?
        fprintf(stderr, "Packet overflow!\n");
        goto restart;
    }

    rxPacket[rx_ptr++] = c;

    if (!((rx0_x80_term && ((c & 0x80) == 0)) || rx_ptr == rx_count))
    {
        return 0;
    }
    put_packet();

    crFinish;
restart:
    crStaticVar = 0;

    return 0;
}

void put_packet()
{
    // packet out
    // for (int i = 0; i < rx_count; i++)
    // {
    //     printf("%02x ", rxPacket[i]);
    // }
    // printf("\n");
    //
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_RECEIVERS; i++)
    {
        if (fds[i] != 0)
        {
            if (write(fds[i], rxPacket, rx_ptr) != rx_ptr)
            {
                close(fds[i]);
                fds[i] = 0;
            }
        }
    }
    pthread_mutex_unlock(&lock);
}
void add_downstream(int fd)
{
    int i;
    pthread_mutex_lock(&lock);
    for (i = 0; i < MAX_RECEIVERS; i++)
    {
        if (fds[i] == 0)
        {
            fds[i] = fd;
        }
    }
    if (i == MAX_RECEIVERS)
    { // no free slot
        close(fd);
    }
    pthread_mutex_unlock(&lock);
}

void* tcp_server(void *arg)
{
    int sockfd, connfd;
    socklen_t len;
    struct sockaddr_in servaddr, cli;

    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
    {
        printf("socket creation failed...\n");
        exit(0);
    }
    else
        printf("Socket successfully created..\n");
    bzero(&servaddr, sizeof(servaddr));

    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    servaddr.sin_port = htons(PORT);

    // Binding newly created socket to given IP and verification
    if ((bind(sockfd, (SA *)&servaddr, sizeof(servaddr))) != 0)
    {
        printf("socket bind failed...\n");
        exit(0);
    }
    else
        printf("Socket successfully binded..\n");

    // Now server is ready to listen and verification
    if ((listen(sockfd, 5)) != 0)
    {
        printf("Listen failed...\n");
        exit(0);
    }
    else
        printf("Server listening..\n");
    len = sizeof(cli);
    while (1)
    {
        // Accept the data packet from client and verification
        connfd = accept(sockfd, (SA *)&cli, &len);
        if (connfd < 0)
        {
            printf("server acccept failed...\n");
            exit(0);
        }
        else
            printf("server acccept the client...\n");

        add_downstream(connfd);
    }
    // After chatting close the socket
    close(sockfd);
}
