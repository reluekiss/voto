#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <opus/opus.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "miniaudio.h"
#include "coroutine.h"

#define SAMPLE_RATE 48000
#define CAPTURE_CHANNELS 2
#define PLAYBACK_CHANNELS 2
#define FRAME_SIZE 960
#define MAX_PACKET_SIZE 4000
#define DEFAULT_PORT 14400
#define PEER_DISCOVERY_PORT 14401
#define MAX_BUFFER_SIZE 8192
#define MAX_PEERS 16
#define MAX_ADDR_STR 64

#define BPF_LOW_CUTOFF  300.0   /* Hz */
#define BPF_HIGH_CUTOFF 3400.0  /* Hz */

typedef enum {
    PACKET_AUDIO = 0,
    PACKET_PEER_LIST = 1,
    PACKET_HELLO = 2,
    PACKET_HELLO_ACK = 3
} PacketType;

typedef enum {
    PEER_DISCONNECTED = 0,
    PEER_CONNECTING,
    PEER_CONNECTED
} PeerState;

typedef struct {
    char address[MAX_ADDR_STR];
    int port;
    int socketFd;
    SSL *ssl;
    PeerState state;
    bool counted;
    time_t lastActivity;
    float audioLevel;
    uint64_t bytesSent, bytesReceived;
    bool audioDetected;
} Peer;

typedef struct {
    int listenFd;
    int discoverySocket;
    SSL_CTX *ctx;
    Peer peers[MAX_PEERS];
    int peerCount;
} NetworkState;
static NetworkState netState = {0};

typedef struct {
    ma_device device;
    ma_rb captureRB;
    ma_rb playbackRB;
    void *captureBufferData;
    void *playbackBufferData;
    ma_bpf2  bpf;
    bool     bpfInitialized;
} AudioContext;
static AudioContext audioCtx = {0};

static OpusEncoder *opusEnc = NULL;
static OpusDecoder *opusDec = NULL;

static char myAddress[MAX_ADDR_STR] = {0};
static int myPort = DEFAULT_PORT;
#define MAX_LOCAL_ADDRS 16
static char localAddrs[MAX_LOCAL_ADDRS][MAX_ADDR_STR];
static int  localAddrCount = 0;

static bool isRunning = true;

static void signalHandler(int sig);
static void getLocalIPAddresses(void);
static bool isLocalAddress(const char *addr);
static int verifyCert(int pre, X509_STORE_CTX *ctx);
static bool setupSSL(void);
static void cleanupSSL(void);

static bool initAudioDevice(void);
static void cleanupAudioDevice(void);
static void ma_callback(ma_device *dev, void *o, const void *i, ma_uint32 f);

static void initializePeers(void);
static bool startServer(int port);

static void acceptConnectionCoroutine(void *arg);
static void connectToPeerCoroutine(void *arg);
static void handlePeerCoroutine(void *arg);
static void discoveryCoroutine(void *arg);

static void processAudioIO(void);
static void broadcastAudioToPeers(const unsigned char *data, int sz);
static void updateAudioLevels(void);
static void drawVisualization(void);
static float calculateRMS(const float *buf, int n);

static bool isPeerConnected(const char *addr, int port);
static void addPeer(const char *addr, int port);
static void handlePeerList(const char *data, int len);
static void sendPeerList(Peer *p);
static void announceSelf(void);

static unsigned char *netBuf = NULL;
static float *fbuf = NULL;
static opus_int16 *pcmBuf = NULL;

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;

    initializePeers();
    coroutine_init();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port = atoi(argv[++i]);
            myPort = port;
        } else if (!strcmp(argv[i], "-h")) {
            printf("Usage: %s [-p port] [peer[:port]]...\n", argv[0]);
            return 0;
        } else {
            char *cp = strdup(argv[i]);
            char *addr = cp;
            int prt = DEFAULT_PORT;
            char *c = strchr(cp, ':');
            if (c) {
                *c = 0;
                prt = atoi(c + 1);
            }
            addPeer(addr, prt);
            free(cp);
        }
    }

    signal(SIGINT, signalHandler);

    getLocalIPAddresses();

    if (!setupSSL())
        return 1;

    if (!initAudioDevice())
        return 1;

    {
        int err;
        opusEnc = opus_encoder_create(SAMPLE_RATE, CAPTURE_CHANNELS, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK) {
            fprintf(stderr, "Opus enc init failed: %s\n", opus_strerror(err));
        }
        opusDec = opus_decoder_create(SAMPLE_RATE, PLAYBACK_CHANNELS, &err);
        if (err != OPUS_OK) {
            fprintf(stderr, "Opus dec init failed: %s\n", opus_strerror(err));
        }
        opus_encoder_ctl(opusEnc, OPUS_SET_BITRATE(64000));
        opus_encoder_ctl(opusEnc, OPUS_SET_VBR(1));
        opus_encoder_ctl(opusEnc, OPUS_SET_VBR_CONSTRAINT(0));
        opus_encoder_ctl(opusEnc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(opusEnc, OPUS_SET_PACKET_LOSS_PERC(5));
    }

    if (!startServer(port))
        return 1;

    coroutine_go(acceptConnectionCoroutine, NULL);
    coroutine_go(discoveryCoroutine, NULL);
    announceSelf();

    while (isRunning) {
        processAudioIO();
        updateAudioLevels();
        coroutine_yield();

        static time_t last = 0;
        time_t now = time(NULL);
        if (now - last >= 1) {
            drawVisualization();
            last = now;
        }
#ifdef DEBUG
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = &netState.peers[i];
            fprintf(stderr, "[DEBUG] Peer IP: %s:%d, State: %d\n", p->address, p->port, p->state);
        }
#endif // DEBUG
        usleep(10000);
    }

    cleanupSSL();
    cleanupAudioDevice();
    opus_encoder_destroy(opusEnc);
    opus_decoder_destroy(opusDec);
    free(netBuf);
    free(fbuf);
    free(pcmBuf);

    printf("\nTerminated.\n");
    return 0;
}

static void signalHandler(int sig) {
    (void)sig;
    isRunning = false;
}

static void getLocalIPAddresses(void) {
    struct ifaddrs *ifp, *ifa;
    if (getifaddrs(&ifp) == -1) {
        perror("getifaddrs");
        strcpy(myAddress, "127.0.0.1");
        strcpy(localAddrs[localAddrCount++], "127.0.0.1");
        return;
    }
    for (ifa = ifp; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (void *)ifa->ifa_addr;
            char buf[MAX_ADDR_STR];
            inet_ntop(AF_INET, &sa->sin_addr, buf, MAX_ADDR_STR);
            if (localAddrCount < MAX_LOCAL_ADDRS) {
                strcpy(localAddrs[localAddrCount++], buf);
            }
            if (!(ifa->ifa_flags & IFF_LOOPBACK) && !myAddress[0]) {
                strcpy(myAddress, buf);
            }
        }
    }
    freeifaddrs(ifp);
    if (!myAddress[0] && localAddrCount > 0) {
        strcpy(myAddress, localAddrs[0]);
    }
    fprintf(stderr, "[INFO] Local IP: %s\n", myAddress);
}

static bool isLocalAddress(const char *addr) {
    for (int i = 0; i < localAddrCount; i++) {
        if (!strcmp(addr, localAddrs[i])) return true;
    }
    return false;
}

static int verifyCert(int pre, X509_STORE_CTX *ctx) {
    (void)pre;
    (void)ctx;
    return 1;
}
static bool setupSSL(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    netState.ctx = SSL_CTX_new(TLS_method());
    if (!netState.ctx) {
        ERR_print_errors_fp(stderr);
        return false;
    }
    SSL_CTX_set_verify(netState.ctx, SSL_VERIFY_PEER, verifyCert);
    if (access("cert.pem", F_OK) | access("key.pem", F_OK)) {
        fprintf(stderr,
                        "[WARN] missing cert.pem/key.pem\n"
                        "openssl req -x509 -newkey rsa:4096 -keyout key.pem \\ \n"
                        "-out cert.pem -days 365 -nodes\n");
        return false;
    }
    if (SSL_CTX_use_certificate_file(netState.ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(netState.ctx, "key.pem", SSL_FILETYPE_PEM) <= 0 ||
        !SSL_CTX_check_private_key(netState.ctx)) {
        ERR_print_errors_fp(stderr);
        return false;
    }
    return true;
}
static void cleanupSSL(void) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->ssl) {
            SSL_shutdown(p->ssl);
            SSL_free(p->ssl);
            p->ssl = NULL;
        }
        if (p->socketFd >= 0) {
            close(p->socketFd);
            p->socketFd = -1;
        }
    }
    if (netState.listenFd >= 0)
        close(netState.listenFd);
    if (netState.discoverySocket >= 0)
        close(netState.discoverySocket);
    if (netState.ctx)
        SSL_CTX_free(netState.ctx);
    ERR_free_strings();
    EVP_cleanup();
}

static void ma_callback(ma_device *dev, void *out_, const void *in_, ma_uint32  frameCount) {
    AudioContext *ctx = (AudioContext *)dev->pUserData;

    if (in_) {
        size_t bytesToWrite = frameCount * dev->capture.channels * sizeof(float);
        ma_uint32 wAvail = ma_rb_available_write(&ctx->captureRB);
        if (wAvail >= bytesToWrite) {
            void  *wPtr;
            size_t wSize;
            ma_rb_acquire_write(&ctx->captureRB, &wSize, &wPtr);
            if (wSize >= bytesToWrite) {
                memcpy(wPtr, in_, bytesToWrite);
                ma_rb_commit_write(&ctx->captureRB, bytesToWrite);
            } else {
                ma_rb_commit_write(&ctx->captureRB, 0);
            }
        }
    }

    if (out_) {
        float *dst = (float *)out_;
        // size_t  frameBytes   = frameCount * dev->playback.channels * sizeof(float);
        size_t  sampleBytes  = dev->playback.channels * sizeof(float);

        ma_uint32 availBytes = ma_rb_available_read(&ctx->playbackRB);
        ma_uint32 availFrames = availBytes / sampleBytes;
        ma_uint32 toCopyFrames = availFrames < frameCount ? availFrames : frameCount;
        size_t toCopyBytes  = toCopyFrames * sampleBytes;

        if (toCopyBytes > 0) {
            void  *rPtr;
            size_t rSize;
            ma_rb_acquire_read(&ctx->playbackRB, &rSize, &rPtr);
            size_t actuallyCopied = rSize < toCopyBytes ? rSize : toCopyBytes;
            memcpy(dst, rPtr, actuallyCopied);
            ma_rb_commit_read(&ctx->playbackRB, actuallyCopied);
        }

        if (toCopyFrames < frameCount) {
            float *zeroStart = dst + toCopyFrames * dev->playback.channels;
            size_t zeroBytes = (frameCount - toCopyFrames) * dev->playback.channels * sizeof(float);
            memset(zeroStart, 0, zeroBytes);
        }
    }
}

static bool initAudioDevice(void) {
    size_t captureBytes  = SAMPLE_RATE * CAPTURE_CHANNELS * sizeof(float);
    size_t playbackBytes = SAMPLE_RATE * PLAYBACK_CHANNELS * sizeof(float);
    /* enlarge ring buffers to reduce overflow under jitter */
    captureBytes  *= 2;
    playbackBytes *= 2;

    audioCtx.captureBufferData  = malloc(captureBytes);
    audioCtx.playbackBufferData = malloc(playbackBytes);
    if (!audioCtx.captureBufferData || !audioCtx.playbackBufferData) {
        fprintf(stderr, "[ERR] alloc\n");
        return false;
    }

    if (ma_rb_init(captureBytes, audioCtx.captureBufferData, NULL, &audioCtx.captureRB) != MA_SUCCESS ||
        ma_rb_init(playbackBytes, audioCtx.playbackBufferData, NULL, &audioCtx.playbackRB) != MA_SUCCESS) {
        fprintf(stderr, "[ERR] rb\n");
        return false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.sampleRate          = SAMPLE_RATE;
    cfg.periodSizeInFrames  = FRAME_SIZE;
    cfg.performanceProfile  = ma_performance_profile_low_latency;
    cfg.capture.format      = ma_format_f32;
    cfg.playback.format     = ma_format_f32;
    cfg.capture.channels    = CAPTURE_CHANNELS;   // 2
    cfg.playback.channels   = PLAYBACK_CHANNELS;  // 2
    cfg.dataCallback        = ma_callback;
    cfg.pUserData           = &audioCtx;

    if (ma_device_init(NULL, &cfg, &audioCtx.device) != MA_SUCCESS ||
        ma_device_start(&audioCtx.device) != MA_SUCCESS) {
        fprintf(stderr, "[ERR] dev\n");
        return false;
    }
    fprintf(stderr, "[INFO] Audio @ %d Hz\n", SAMPLE_RATE);

    fbuf    = malloc(FRAME_SIZE * CAPTURE_CHANNELS * sizeof(float));
    pcmBuf  = malloc(FRAME_SIZE * CAPTURE_CHANNELS * sizeof(opus_int16));
    netBuf  = malloc(MAX_PACKET_SIZE);

    double fLow  = BPF_LOW_CUTOFF;
    double fHigh = BPF_HIGH_CUTOFF;
    double fc    = sqrt(fLow * fHigh);
    double Q     = fc / (fHigh - fLow);

    ma_bpf2_config bpfConfig = ma_bpf2_config_init(ma_format_f32, CAPTURE_CHANNELS, SAMPLE_RATE, fc, Q);
    if (ma_bpf2_init(&bpfConfig, NULL, &audioCtx.bpf) != MA_SUCCESS) {
        fprintf(stderr, "[ERR] ma_bpf2_init failed\n");
        return false;
    }
    audioCtx.bpfInitialized = true;

    return true;
}

static void cleanupAudioDevice(void) {
    if (audioCtx.bpfInitialized) {
        ma_bpf2_uninit(&audioCtx.bpf, NULL);
        audioCtx.bpfInitialized = false;
    }
    ma_device_uninit(&audioCtx.device);
    ma_rb_uninit(&audioCtx.captureRB);
    ma_rb_uninit(&audioCtx.playbackRB);
    free(audioCtx.captureBufferData);
    free(audioCtx.playbackBufferData);
}

static void initializePeers(void) {
    for (int i = 0; i < MAX_PEERS; i++) {
        netState.peers[i].state = PEER_DISCONNECTED;
        netState.peers[i].socketFd = -1;
        netState.peers[i].ssl = NULL;
        netState.peers[i].counted = false;
    }
    netState.peerCount = 0;
}

static bool startServer(int port) {
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(port);
    netState.listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (netState.listenFd < 0) {
        perror("sock");
        return false;
    }
    int opt = 1;
    setsockopt(netState.listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(netState.listenFd, F_SETFL,
                fcntl(netState.listenFd, F_GETFL, 0) | O_NONBLOCK);
    if (bind(netState.listenFd, (void *)&sa, sizeof sa) < 0) {
        perror("bind");
        return false;
    }
    if (listen(netState.listenFd, MAX_PEERS) < 0) {
        perror("listen");
        return false;
    }
    fprintf(stderr, "[INFO] TCP %d\n", port);

    struct sockaddr_in da = {0};
    da.sin_family = AF_INET;
    da.sin_addr.s_addr = INADDR_ANY;
    da.sin_port = htons(PEER_DISCOVERY_PORT);
    netState.discoverySocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (netState.discoverySocket < 0) {
        perror("udp");
        return false;
    }
    setsockopt(netState.discoverySocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(netState.discoverySocket, F_SETFL, fcntl(netState.discoverySocket, F_GETFL, 0) | O_NONBLOCK);
    if (bind(netState.discoverySocket, (void *)&da, sizeof da) < 0) {
        perror("udpb");
        return false;
    }
    setsockopt(netState.discoverySocket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof opt);
    fprintf(stderr, "[INFO] UDP %d\n", PEER_DISCOVERY_PORT);
    return true;
}

static void acceptConnectionCoroutine(void *arg) {
    (void)arg;
    while (isRunning) {
        struct sockaddr_in ca;
        socklen_t al = sizeof ca;
        int cfd = accept(netState.listenFd, (void *)&ca, &al);
        if (cfd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept");
            coroutine_sleep_read(netState.listenFd);
            continue;
        }
        fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
        int idx = -1;
        for (int i = 0; i < MAX_PEERS; i++)
            if (netState.peers[i].state == PEER_DISCONNECTED) {
                idx = i;
                break;
            }
        if (idx < 0) {
            close(cfd);
            continue;
        }
        Peer *p = &netState.peers[idx];
        inet_ntop(AF_INET, &ca.sin_addr, p->address, MAX_ADDR_STR);
        p->port = ntohs(ca.sin_port);
        p->socketFd = cfd;
        p->state = PEER_CONNECTING;
        p->lastActivity = time(NULL);
        fprintf(stderr, "[IN] TCP %s:%d\n", p->address, p->port);
        p->ssl = SSL_new(netState.ctx);
        SSL_set_fd(p->ssl, cfd);
        coroutine_go(handlePeerCoroutine, p);
    }
}

static void connectToPeerCoroutine(void *arg) {
    Peer *p = (Peer *)arg;
    p->state = PEER_CONNECTING;
    p->lastActivity = time(NULL);
    p->socketFd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(p->socketFd, F_SETFL, fcntl(p->socketFd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(p->port);
    inet_pton(AF_INET, p->address, &sa.sin_addr);
    int r = connect(p->socketFd, (void *)&sa, sizeof sa);
    if (r < 0 && errno != EINPROGRESS)
        goto fail;
    if (r < 0) {
        coroutine_sleep_write(p->socketFd);
        socklen_t e, el = sizeof e;
        if (getsockopt(p->socketFd, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e)
            goto fail;
    }
    fprintf(stderr, "[OUT] TCP %s:%d ok\n", p->address, p->port);
    p->ssl = SSL_new(netState.ctx);
    SSL_set_fd(p->ssl, p->socketFd);
    do {
        r = SSL_connect(p->ssl);
        if (r <= 0) {
            int e = SSL_get_error(p->ssl, r);
            if (e == SSL_ERROR_WANT_READ) {
                coroutine_sleep_read(p->socketFd);
            } else if (e == SSL_ERROR_WANT_WRITE) {
                coroutine_sleep_write(p->socketFd);
            } else {
                goto fail;
            }
        }
    } while (r <= 0);
    unsigned char hello[3] = {PACKET_HELLO, (unsigned char)((myPort >> 8) & 0xFF), (unsigned char)(myPort & 0xFF)};
    SSL_write(p->ssl, hello, sizeof(hello));
    SSL_set_mode(p->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    p->state = PEER_CONNECTED;
    netState.peerCount++;
    coroutine_go(handlePeerCoroutine, p);
    return;
fail:
    if (p->ssl)
        SSL_free(p->ssl), p->ssl = NULL;
    if (p->socketFd >= 0)
        close(p->socketFd), p->socketFd = -1;
    p->state = PEER_DISCONNECTED;
}

static void handlePeerCoroutine(void *arg) {
    Peer *p = (Peer *)arg;
    while (isRunning) {
        if (p->state == PEER_CONNECTING) {
            int r;
            do {
                r = SSL_accept(p->ssl);
                if (r <= 0) {
                    int e = SSL_get_error(p->ssl, r);
                    if (e == SSL_ERROR_WANT_READ) {
                        coroutine_sleep_read(p->socketFd);
                    } else if (e == SSL_ERROR_WANT_WRITE) {
                        coroutine_sleep_write(p->socketFd);
                    } else {
                        goto cleanup;
                    }
                }
            } while (r <= 0);

            p->state = PEER_CONNECTED;
            netState.peerCount++;
        }
        SSL_set_mode(p->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        coroutine_sleep_read(p->socketFd);
        unsigned char *nb = netBuf;
        int n = SSL_read(p->ssl, nb, MAX_PACKET_SIZE);
        if (n >= 3 && nb[0] == PACKET_HELLO) {
            int theirPort = (nb[1] << 8) | (nb[2] & 0xFF);
            p->port = theirPort;
            unsigned char ack[3] = {PACKET_HELLO_ACK, (myPort >> 8) & 0xFF, myPort & 0xFF};
            SSL_write(p->ssl, ack, 3);
            sendPeerList(p);
            continue;
        }
        if (n <= 0) {
            int e = SSL_get_error(p->ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
                continue;
            goto cleanup;
        }
        p->lastActivity = time(NULL);
        p->bytesReceived += n;
        switch (nb[0]) {
        case PACKET_AUDIO: {
            int frames = opus_decode(opusDec, nb + 1, n - 1, pcmBuf, FRAME_SIZE, 0);
            if (frames > 0) {
                int totalSamples = frames * PLAYBACK_CHANNELS;
                for (int i = 0; i < totalSamples; i++) {
                    fbuf[i] = pcmBuf[i] / 32767.0f;
                }
                p->audioLevel = calculateRMS(fbuf, totalSamples);
        
                size_t writeBytes = totalSamples * sizeof(float);
                size_t   wAvail;
                void     *wPtr;
                if (ma_rb_acquire_write(&audioCtx.playbackRB, &wAvail, &wPtr) == MA_SUCCESS) {
                    if (wAvail >= writeBytes) {
                        memcpy(wPtr, fbuf, writeBytes);
                        ma_rb_commit_write(&audioCtx.playbackRB,
                                           writeBytes);
                    } else {
                        ma_rb_commit_write(&audioCtx.playbackRB, 0);
                    }
                }
            }
        } break;
        case PACKET_PEER_LIST:
            handlePeerList((char *)nb + 1, n - 1);
            break;
        case PACKET_HELLO: {
            unsigned char a = PACKET_HELLO_ACK;
            SSL_write(p->ssl, &a, 1);
            sendPeerList(p);
        } break;
        default:
            break;
        }
    }
cleanup:
    if (p->ssl) {
        SSL_shutdown(p->ssl);
        SSL_free(p->ssl);
        p->ssl = NULL;
    }
    if (p->socketFd >= 0) {
        close(p->socketFd);
        p->socketFd = -1;
    }
    if (p->state == PEER_CONNECTED) {
        p->state = PEER_DISCONNECTED;
        netState.peerCount--;
    }
}

static void discoveryCoroutine(void *arg) {
    (void)arg;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    while (isRunning) {
        char buf2[MAX_BUFFER_SIZE];
        int n = recvfrom(netState.discoverySocket, buf2, sizeof buf2, 0, (struct sockaddr *)&sa, &sl);
        if (n > 0) {
            buf2[n] = 0;
            if (!strncmp(buf2, "VOICECHAT:", 10)) {
                char *ipp = buf2 + 10;
                char *c = strchr(ipp, ':');
                if (c) {
                    *c = 0;
                    int pr = atoi(c + 1);
                    char sip[MAX_ADDR_STR];
                    inet_ntop(AF_INET, &sa.sin_addr, sip, MAX_ADDR_STR);
                    if (isLocalAddress(sip) && pr == myPort) {
                        continue;
                    }
                    if (!isPeerConnected(ipp, pr)) {
                        fprintf(stderr, "[DISC] %s:%d\n", ipp, pr);
                        addPeer(ipp, pr);
                    }
                }
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recvfrom");
        } else {
            coroutine_sleep_read(netState.discoverySocket);
        }
        static time_t last = 0;
        time_t now = time(NULL);
        if (now - last > 30) {
            announceSelf();
            last = now;
        }
    }
}

static void rb_read_exact(ma_rb *rb, void *dst, size_t bytesToRead) {
    size_t part;
    void  *p;
    ma_rb_acquire_read(rb, &part, &p);
    if (part > bytesToRead) part = bytesToRead;
    memcpy(dst, p, part);
    ma_rb_commit_read(rb, part);

    size_t rem = bytesToRead - part;
    if (rem > 0) {
        ma_rb_acquire_read(rb, &part, &p);
        if (part > rem) part = rem;
        memcpy((char *)dst + part, p, rem);
        ma_rb_commit_read(rb, rem);
    }
}

static void processAudioIO(void) {
    size_t   frameBytes = FRAME_SIZE * CAPTURE_CHANNELS * sizeof(float);
    ma_uint32 availBytes = ma_rb_available_read(&audioCtx.captureRB);
    if (availBytes < frameBytes) {
        return;
    }

    rb_read_exact(&audioCtx.captureRB, fbuf, frameBytes);

    if (audioCtx.bpfInitialized) {
        //out+in (in-place)
        ma_bpf2_process_pcm_frames(&audioCtx.bpf, fbuf, fbuf, FRAME_SIZE);
    }

    size_t   wAvail;
    void     *wPtr;
    ma_rb_acquire_write(&audioCtx.playbackRB, &wAvail, &wPtr);
    if (wAvail >= frameBytes) {
        memcpy(wPtr, fbuf, frameBytes);
        ma_rb_commit_write(&audioCtx.playbackRB, frameBytes);
    } else {
        ma_rb_commit_write(&audioCtx.playbackRB, 0);
    }

    int totalSamples = FRAME_SIZE * CAPTURE_CHANNELS;
    for (int i = 0; i < totalSamples; i++) {
        pcmBuf[i] = (opus_int16)(fbuf[i] * 32767.0f);
    }

    int nb = opus_encode(opusEnc, pcmBuf, FRAME_SIZE, netBuf + 1, MAX_PACKET_SIZE - 1);
    if (nb > 0) {
        netBuf[0] = PACKET_AUDIO;
        broadcastAudioToPeers(netBuf, nb + 1);
    }
}

static void broadcastAudioToPeers(const unsigned char *data, int sz) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->state == PEER_CONNECTED && p->ssl) {
            int w = SSL_write(p->ssl, data, sz);
            if (w > 0) {
                p->bytesSent += w;
            } else {
                int err = SSL_get_error(p->ssl, w);
                fprintf(stderr, "[E] SSL_write to %s:%d -> %d (err %d)\n", p->address, p->port, w, err);
            }
        }
    }
}

static void updateAudioLevels(void) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->state == PEER_CONNECTED) {
            p->audioLevel *= 0.9f;
            if (time(NULL) - p->lastActivity > 60) {
                fprintf(stderr, "[TIMEOUT] %s:%d\n", p->address, p->port);
                if (p->ssl)
                    SSL_shutdown(p->ssl), SSL_free(p->ssl), p->ssl = NULL;
                if (p->socketFd >= 0)
                    close(p->socketFd), p->socketFd = -1;
                p->state = PEER_DISCONNECTED;
                netState.peerCount--;
            }
        }
    }
}

static void drawVisualization(void) {
    printf("\033[2J\033[H");
    printf("=== Voice Chat ===\n\n");
    ma_uint32 avail = ma_rb_available_read(&audioCtx.captureRB);
    printf("Mic ring: %4lu / %4u\n", avail / sizeof(float), SAMPLE_RATE);
    printf("\nPeers: %d\n", netState.peerCount);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->state == PEER_CONNECTED) {
            printf("%-15s ", p->address);
            int bar = 15, fill = p->audioLevel * bar * 5;
            if (fill > bar)
                fill = bar;
            printf("[");
            for (int j = 0; j < bar; j++)
                putchar(j < fill ? '#' : ' ');
            printf("] ↑%.1f↓%.1f\n", p->bytesSent / 1024.0f, p->bytesReceived / 1024.0f);
        }
    }
    printf("\nCtrl+C to exit\n");
    fflush(stdout);
}

static float calculateRMS(const float *b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++)
        s += b[i] * b[i];
    return sqrt(s / n);
}

static bool isPeerConnected(const char *addr, int port) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if ((p->state == PEER_CONNECTED || p->state == PEER_CONNECTING) &&
            !strcmp(p->address, addr) && p->port == port)
            return true;
    }
    return false;
}

static void addPeer(const char *addr, int port) {
    if (isPeerConnected(addr, port))
        return;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->state == PEER_DISCONNECTED) {
            strcpy(p->address, addr);
            p->port = port;
            coroutine_go(connectToPeerCoroutine, p);
            return;
        }
    }
    fprintf(stderr, "[WARN] no slot %s:%d\n", addr, port);
}

static void handlePeerList(const char *data, int len) {
    char *cp = malloc(len + 1);
    memcpy(cp, data, len);
    cp[len] = 0;
    char *tok, *save;
    for (tok = strtok_r(cp, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *c = strchr(tok, ':');
        if (!c)
            continue;
        *c = 0;
        int pr = atoi(c + 1);
        if (isLocalAddress(tok) && pr == myPort) {
            continue;
        }
        if (!isPeerConnected(tok, pr))
            addPeer(tok, pr);
    }
    free(cp);
}

static void sendPeerList(Peer *p) {
    char buf2[MAX_BUFFER_SIZE];
    int off = 0;
    buf2[off++] = PACKET_PEER_LIST;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *q = &netState.peers[i];
        if (q->state == PEER_CONNECTED) {
            off += snprintf(buf2 + off, MAX_BUFFER_SIZE - off, "%s:%d;", q->address, q->port);
        }
    }
    off += snprintf(buf2 + off, MAX_BUFFER_SIZE - off, "%s:%d;", myAddress, myPort);
    SSL_write(p->ssl, buf2, off);
}

static void announceSelf(void) {
    char msg[128];
    snprintf(msg, sizeof msg, "VOICECHAT:%s:%d", myAddress, myPort);
    struct sockaddr_in ba = {0};
    ba.sin_family = AF_INET;
    ba.sin_port = htons(PEER_DISCOVERY_PORT);
    ba.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(netState.discoverySocket, msg, strlen(msg), 0, (void *)&ba, sizeof ba);
    fprintf(stderr, "[DISC] self\n");
}
