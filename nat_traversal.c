#include "nat_traversal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#define MAX_HOLE_PUNCH_PEERS 16
#define MAX_LOCAL_IPS 8

static NatEndpoint localEndpoint = {0};
static HolePunchContext holePunchPeers[MAX_HOLE_PUNCH_PEERS] = {0};
static int udpHolePunchSocket = -1;
static bool natTraversalInitialized = false;

// Forward declarations
static bool detect_nat_type(void);
static void get_local_ips(char ips[][64], int *count);
static HolePunchContext* find_hole_punch_context(const char *peerAddr, int peerPort);
static HolePunchContext* get_free_hole_punch_context(void);
static void send_hole_punch_packet(HolePunchContext *ctx, NatPacketType type);
static void handle_hole_punch_packet(const unsigned char *data, int len, struct sockaddr_in *from);

bool nat_traversal_init(int tcpPort) {
    if (natTraversalInitialized) {
        return true;
    }

    memset(&localEndpoint, 0, sizeof(localEndpoint));
    localEndpoint.localPort = tcpPort + NAT_TRAVERSAL_PORT_OFFSET;
    localEndpoint.lastUpdate = time(NULL);

    char localIPs[MAX_LOCAL_IPS][64];
    int ipCount = 0;
    get_local_ips(localIPs, &ipCount);
    
    if (ipCount > 0) {
        for (int i = 0; i < ipCount; i++) {
            if (strcmp(localIPs[i], "127.0.0.1") != 0) {
                strcpy(localEndpoint.localIP, localIPs[i]);
                break;
            }
        }
        if (strlen(localEndpoint.localIP) == 0) {
            strcpy(localEndpoint.localIP, localIPs[0]);
        }
    } else {
        strcpy(localEndpoint.localIP, "127.0.0.1");
    }

    // Create UDP socket for hole punching
    udpHolePunchSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpHolePunchSocket < 0) {
        perror("nat_traversal: socket creation failed");
        return false;
    }

    int flags = fcntl(udpHolePunchSocket, F_GETFL, 0);
    fcntl(udpHolePunchSocket, F_SETFL, flags | O_NONBLOCK);

    int opt = 1;
    setsockopt(udpHolePunchSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in bindAddr = {0};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(localEndpoint.localPort);

    if (bind(udpHolePunchSocket, (struct sockaddr*)&bindAddr, 
             sizeof(bindAddr)) < 0) {
        perror("nat_traversal: bind failed");
        close(udpHolePunchSocket);
        return false;
    }

    localEndpoint.natDetected = detect_nat_type();

    for (int i = 0; i < MAX_HOLE_PUNCH_PEERS; i++) {
        holePunchPeers[i].udpSocket = -1;
        holePunchPeers[i].holePunchActive = false;
        holePunchPeers[i].holePunchSuccess = false;
    }

    natTraversalInitialized = true;
    fprintf(stderr, "[NAT] Initialized on %s:%d (NAT detected: %s)\n",
            localEndpoint.localIP, localEndpoint.localPort,
            localEndpoint.natDetected ? "yes" : "no");

    return true;
}

void nat_traversal_cleanup(void) {
    if (!natTraversalInitialized) return;

    if (udpHolePunchSocket >= 0) {
        close(udpHolePunchSocket);
        udpHolePunchSocket = -1;
    }

    for (int i = 0; i < MAX_HOLE_PUNCH_PEERS; i++) {
        if (holePunchPeers[i].udpSocket >= 0) {
            close(holePunchPeers[i].udpSocket);
            holePunchPeers[i].udpSocket = -1;
        }
    }
    natTraversalInitialized = false;
}

bool nat_start_hole_punch(const char *peerAddr, int peerPort) {
    if (!natTraversalInitialized) return false;

    HolePunchContext *ctx = find_hole_punch_context(peerAddr, peerPort);
    if (!ctx) {
        ctx = get_free_hole_punch_context();
        if (!ctx) {
            fprintf(stderr, "[NAT] No free hole punch context for %s:%d\n",
                    peerAddr, peerPort);
            return false;
        }
    }

    snprintf(ctx->peerID, sizeof(ctx->peerID), "%s:%d", peerAddr, peerPort);
    strcpy(ctx->remote.localIP, peerAddr);
    ctx->remote.localPort = peerPort + NAT_TRAVERSAL_PORT_OFFSET;
    
    ctx->remoteAddr.sin_family = AF_INET;
    ctx->remoteAddr.sin_port = htons(ctx->remote.localPort);
    inet_pton(AF_INET, peerAddr, &ctx->remoteAddr.sin_addr);
    ctx->remoteAddrLen = sizeof(ctx->remoteAddr);

    ctx->holePunchActive = true;
    ctx->holePunchSuccess = false;
    ctx->lastAttempt = time(NULL);
    ctx->attemptCount = 0;

    fprintf(stderr, "[NAT] Starting hole punch to %s:%d\n", peerAddr, peerPort);

    send_hole_punch_packet(ctx, NAT_PACKET_HOLE_PUNCH_REQUEST);

    return true;
}

bool nat_send_data(const char *peerAddr, int peerPort, const void *data, size_t len) {
    if (!natTraversalInitialized) return false;

    HolePunchContext *ctx = find_hole_punch_context(peerAddr, peerPort);
    if (!ctx || !ctx->holePunchSuccess) {
        return false;
    }

    unsigned char packet[len + 1];
    packet[0] = NAT_PACKET_HOLE_PUNCH_DATA;
    memcpy(packet + 1, data, len);

    int sent = sendto(udpHolePunchSocket, packet, len + 1, 0,
                      (struct sockaddr*)&ctx->remoteAddr, 
                      ctx->remoteAddrLen);

    return sent > 0;
}

void nat_process_packets(void) {
    if (!natTraversalInitialized) return;

    unsigned char buffer[4096];
    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);

    while (true) {
        int len = recvfrom(udpHolePunchSocket, buffer, sizeof(buffer), 0,
                          (struct sockaddr*)&fromAddr, &fromLen);
        
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more packets
            }
            perror("nat_process_packets: recvfrom");
            break;
        }

        if (len > 0) {
            handle_hole_punch_packet(buffer, len, &fromAddr);
        }
    }

    // Process active hole punch attempts
    time_t now = time(NULL);
    for (int i = 0; i < MAX_HOLE_PUNCH_PEERS; i++) {
        HolePunchContext *ctx = &holePunchPeers[i];
        
        if (!ctx->holePunchActive) continue;

        // Timeout check
        if (now - ctx->lastAttempt > HOLE_PUNCH_TIMEOUT_SEC) {
            if (ctx->attemptCount >= MAX_HOLE_PUNCH_ATTEMPTS) {
                fprintf(stderr, "[NAT] Hole punch timeout for %s\n", ctx->peerID);
                ctx->holePunchActive = false;
                continue;
            }
        }

        // Send periodic hole punch packets
        if (now - ctx->lastAttempt >= (HOLE_PUNCH_INTERVAL_MS / 1000)) {
            send_hole_punch_packet(ctx, NAT_PACKET_HOLE_PUNCH_REQUEST);
            ctx->attemptCount++;
            ctx->lastAttempt = now;
        }
    }
}

bool nat_is_hole_punch_established(const char *peerAddr, int peerPort) {
    if (!natTraversalInitialized) return false;

    HolePunchContext *ctx = find_hole_punch_context(peerAddr, peerPort);
    return ctx && ctx->holePunchSuccess;
}

NatEndpoint* nat_get_local_endpoint(void) {
    return natTraversalInitialized ? &localEndpoint : NULL;
}

void nat_send_tcp_coordination(void *ssl, const char *myAddr, int myPort) {
    if (!natTraversalInitialized || !ssl) return;

    char coordMsg[256];
    int len = snprintf(coordMsg, sizeof(coordMsg), 
                      "NAT_COORD:%s:%d:%s:%d:%d",
                      localEndpoint.localIP, localEndpoint.localPort,
                      localEndpoint.publicIP, localEndpoint.publicPort,
                      localEndpoint.natDetected ? 1 : 0);

    // This would use SSL_write - assuming SSL* type
    // SSL_write((SSL*)ssl, coordMsg, len);
    // For now, just print debug info
    fprintf(stderr, "[NAT] Would send coordination: %s\n", coordMsg);
}

void nat_handle_tcp_coordination(const char *data, int len, const char *peerAddr, int peerPort) {
    if (!natTraversalInitialized) return;

    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';

    if (strncmp(msg, "NAT_COORD:", 10) != 0) return;

    char *coords = msg + 10;
    char localIP[64], publicIP[64];
    int localPort, publicPort, natDetected;

    if (sscanf(coords, "%63[^:]:%d:%63[^:]:%d:%d",
               localIP, &localPort, publicIP, &publicPort, &natDetected) == 5) {
        
        fprintf(stderr, "[NAT] Received coordination from %s:%d\n", peerAddr, peerPort);
        
        // Start hole punching to this peer
        nat_start_hole_punch(peerAddr, peerPort);
    }
}

// Private functions
static bool detect_nat_type(void) {
    // Simple NAT detection: compare local IP with what we think is public
    // In a real implementation, you'd use STUN or similar
    
    // For now, assume NAT if local IP is private
    if (strncmp(localEndpoint.localIP, "192.168.", 8) == 0 ||
        strncmp(localEndpoint.localIP, "10.", 3) == 0 ||
        strncmp(localEndpoint.localIP, "172.", 4) == 0) {
        strcpy(localEndpoint.publicIP, "unknown");
        localEndpoint.publicPort = localEndpoint.localPort;
        return true;
    }
    
    strcpy(localEndpoint.publicIP, localEndpoint.localIP);
    localEndpoint.publicPort = localEndpoint.localPort;
    return false;
}

static void get_local_ips(char ips[][64], int *count) {
    *count = 0;
    struct ifaddrs *ifap, *ifa;
    
    if (getifaddrs(&ifap) == -1) {
        strcpy(ips[0], "127.0.0.1");
        *count = 1;
        return;
    }

    for (ifa = ifap; ifa != NULL && *count < MAX_LOCAL_IPS; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET && !(ifa->ifa_flags & IFF_LOOPBACK)) {
            struct sockaddr_in *sa = (struct sockaddr_in*)ifa->ifa_addr;
            inet_ntop(AF_INET, &sa->sin_addr, ips[*count], 64);
            (*count)++;
        }
    }

    freeifaddrs(ifap);
    
    if (*count == 0) {
        strcpy(ips[0], "127.0.0.1");
        *count = 1;
    }
}

static HolePunchContext* find_hole_punch_context(const char *peerAddr, int peerPort) {
    char peerID[64];
    snprintf(peerID, sizeof(peerID), "%s:%d", peerAddr, peerPort);
    
    for (int i = 0; i < MAX_HOLE_PUNCH_PEERS; i++) {
        if (strcmp(holePunchPeers[i].peerID, peerID) == 0) {
            return &holePunchPeers[i];
        }
    }
    return NULL;
}

static HolePunchContext* get_free_hole_punch_context(void) {
    for (int i = 0; i < MAX_HOLE_PUNCH_PEERS; i++) {
        if (!holePunchPeers[i].holePunchActive && 
            strlen(holePunchPeers[i].peerID) == 0) {
            return &holePunchPeers[i];
        }
    }
    return NULL;
}

static void send_hole_punch_packet(HolePunchContext *ctx, NatPacketType type) {
    unsigned char packet[64];
    packet[0] = type;
    
    // Add local endpoint info for coordination
    int len = 1;
    if (type == NAT_PACKET_HOLE_PUNCH_REQUEST) {
        len += snprintf((char*)packet + 1, sizeof(packet) - 1,
                       "%s:%d", localEndpoint.localIP, 
                       localEndpoint.localPort);
    }

    int sent = sendto(udpHolePunchSocket, packet, len, 0,
                      (struct sockaddr*)&ctx->remoteAddr, 
                      ctx->remoteAddrLen);

    if (sent > 0) {
        fprintf(stderr, "[NAT] Sent hole punch packet (type %d) to %s\n",
                type, ctx->peerID);
    }
}

static void handle_hole_punch_packet(const unsigned char *data, int len, struct sockaddr_in *from) {
    if (len < 1) return;

    char fromIP[64];
    inet_ntop(AF_INET, &from->sin_addr, fromIP, sizeof(fromIP));
    int fromPort = ntohs(from->sin_port);

    NatPacketType type = (NatPacketType)data[0];
    
    switch (type) {
        case NAT_PACKET_HOLE_PUNCH_REQUEST: {
            fprintf(stderr, "[NAT] Received hole punch request from %s:%d\n",
                    fromIP, fromPort);
            
            // Send response
            unsigned char response[64];
            response[0] = NAT_PACKET_HOLE_PUNCH_RESPONSE;
            int respLen = 1 + snprintf((char*)response + 1, 
                                      sizeof(response) - 1,
                                      "%s:%d", localEndpoint.localIP, 
                                      localEndpoint.localPort);
            
            sendto(udpHolePunchSocket, response, respLen, 0,
                   (struct sockaddr*)from, sizeof(*from));
            
            // Mark hole punch as successful for this peer
            // (In practice, you'd want more sophisticated success detection)
            break;
        }
        
        case NAT_PACKET_HOLE_PUNCH_RESPONSE: {
            fprintf(stderr, "[NAT] Received hole punch response from %s:%d\n",
                    fromIP, fromPort);
            
            // Find context and mark as successful
            for (int i = 0; i < MAX_HOLE_PUNCH_PEERS; i++) {
                HolePunchContext *ctx = &holePunchPeers[i];
                if (ctx->holePunchActive &&
                    ctx->remoteAddr.sin_addr.s_addr == from->sin_addr.s_addr) {
                    ctx->holePunchSuccess = true;
                    ctx->holePunchActive = false;
                    fprintf(stderr, "[NAT] Hole punch successful for %s\n", ctx->peerID);
                    break;
                }
            }
            break;
        }
        
        case NAT_PACKET_HOLE_PUNCH_DATA: {
            // Handle application data received through hole punch
            if (len > 1) {
                fprintf(stderr, "[NAT] Received %d bytes of data from %s:%d\n",
                        len - 1, fromIP, fromPort);
                // Application would process data[1..len-1] here
            }
            break;
        }
    }
}
