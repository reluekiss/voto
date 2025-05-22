#ifndef NAT_TRAVERSAL_H
#define NAT_TRAVERSAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#define MAX_HOLE_PUNCH_ATTEMPTS 10
#define HOLE_PUNCH_INTERVAL_MS  200
#define HOLE_PUNCH_TIMEOUT_SEC  5
#define NAT_TRAVERSAL_PORT_OFFSET 100  // UDP hole punch port = TCP port + 100

typedef enum {
    NAT_PACKET_HOLE_PUNCH_REQUEST = 0x10,
    NAT_PACKET_HOLE_PUNCH_RESPONSE = 0x11,
    NAT_PACKET_HOLE_PUNCH_DATA = 0x12
} NatPacketType;

typedef struct {
    char localIP[64];
    int  localPort;
    char publicIP[64];
    int  publicPort;
    bool natDetected;
    time_t lastUpdate;
} NatEndpoint;

typedef struct {
    char peerID[64];  // peer's address:port
    NatEndpoint local;
    NatEndpoint remote;
    int udpSocket;
    struct sockaddr_in remoteAddr;
    socklen_t remoteAddrLen;
    bool holePunchActive;
    bool holePunchSuccess;
    time_t lastAttempt;
    int attemptCount;
} HolePunchContext;

// Initialize NAT traversal system
bool nat_traversal_init(int tcpPort);

// Cleanup NAT traversal system
void nat_traversal_cleanup(void);

// Start hole punching process for a peer
bool nat_start_hole_punch(const char *peerAddr, int peerPort);

// Send data through established hole punch connection
bool nat_send_data(const char *peerAddr, int peerPort, const void *data, size_t len);

// Process incoming UDP packets (call from main loop)
void nat_process_packets(void);

// Check if hole punch is established for a peer
bool nat_is_hole_punch_established(const char *peerAddr, int peerPort);

// Get local endpoint info
NatEndpoint* nat_get_local_endpoint(void);

// Send hole punch coordination message via TCP
void nat_send_tcp_coordination(void *ssl, const char *myAddr, int myPort);

// Handle hole punch coordination from TCP
void nat_handle_tcp_coordination(const char *data, int len, 
                                const char *peerAddr, int peerPort);

#endif // NAT_TRAVERSAL_H
