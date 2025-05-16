#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <opus/opus.h>
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

#define SAMPLE_RATE          48000
#define CAPTURE_CHANNELS     2
#define PLAYBACK_CHANNELS    2
#define FRAME_SIZE           960
#define MAX_PACKET_SIZE      4000    /* max opus payload */
#define MAX_DTLS_PACKET_SIZE (MAX_PACKET_SIZE + 6)
#define DEFAULT_PORT         14400
#define PEER_DISCOVERY_PORT  14401
#define MAX_BUFFER_SIZE      8192
#define MAX_PEERS            16
#define MAX_ADDR_STR         64
#define JITTER_SIZE          32
#define BPF_LOW_CUTOFF       300.0
#define BPF_HIGH_CUTOFF      3400.0

typedef enum {
    PACKET_PEER_LIST = 1,
    PACKET_HELLO     = 2,
    PACKET_HELLO_ACK = 3
} PacketType;

typedef enum {
    PEER_DISCONNECTED = 0,
    PEER_CONNECTING,
    PEER_CONNECTED
} PeerState;

/* per‐peer jitter buffer */
typedef struct {
    float    pcm[JITTER_SIZE][FRAME_SIZE * PLAYBACK_CHANNELS];
    uint32_t ts[JITTER_SIZE];
    uint16_t seq[JITTER_SIZE];
    int      start;
    int      count;
    uint32_t expectedTs;
    bool     initialized;
} JitterBuffer;

typedef struct {
    /* TCP control channel */
    char            address[MAX_ADDR_STR];
    int             port;
    int             socketFd;
    SSL            *ssl;
    PeerState       state;
    time_t          lastActivity;
    float           audioLevel;
    uint64_t        bytesSent, bytesReceived;

    /* --- new UDP/DTLS fields --- */
    struct sockaddr_in udpAddr;   /* peer UDP/DTLS address */
    socklen_t          udpAddrLen;
    bool               initiator; /* true if we initiated TCP */
    SSL               *dtls;      /* DTLS SSL object */
    bool               dtlsStarted;
    bool               dtlsConnected;

    JitterBuffer       jitter;
} Peer;

typedef struct {
    int      listenFd;
    int      discoverySocket;
    int      audioFd;
    SSL_CTX *tlsCtx;
    SSL_CTX *dtlsCtx;
    Peer     peers[MAX_PEERS];
    int      peerCount;
} NetworkState;

static NetworkState netState = {0};

typedef struct {
    ma_device device;
    ma_rb     captureRB;
    ma_rb     playbackRB;
    void     *captureBufferData;
    void     *playbackBufferData;
    ma_bpf2   bpf;
    bool      bpfInitialized;
} AudioContext;

static AudioContext    audioCtx = {0};
static OpusEncoder    *opusEnc  = NULL;
static OpusDecoder    *opusDec  = NULL;

static char myAddress[MAX_ADDR_STR] = {0};
static int  myPort                   = DEFAULT_PORT;
#define MAX_LOCAL_ADDRS 16
static char localAddrs[MAX_LOCAL_ADDRS][MAX_ADDR_STR];
static int  localAddrCount = 0;

static bool isRunning = true;

static void signalHandler(int sig);
static void getLocalIPAddresses(void);
static bool isLocalAddress(const char *addr);
static int  verifyCert(int pre, X509_STORE_CTX *ctx);
static bool setupSSL(void);
static void cleanupSSL(void);

static bool initAudioDevice(void);
static void cleanupAudioDevice(void);
static void ma_callback(ma_device *dev, void *out_, const void *in_, ma_uint32 frameCount);

static void initializePeers(void);
static bool startServer(int port);

static void acceptConnectionCoroutine(void *arg);
static void connectToPeerCoroutine(void *arg);
static void handlePeerCoroutine(void *arg);
static void discoveryCoroutine(void *arg);
static void dtlsCoroutine(void *arg);

static void processAudioIO(void);
static void broadcastAudioDTLS(const unsigned char *opus, int opusLen);
static void updateAudioLevels(void);
static void drawVisualization(void);
static float calculateRMS(const float *buf, int n);

static bool isPeerConnected(const char *addr, int port);
static void addPeer(const char *addr, int port);
static void handlePeerList(const char *data, int len);
static void sendPeerList(Peer *p);
static void announceSelf(void);

static void flushDTLS(Peer *p);
static void jitterInsert(Peer *p, float *pcm, int samples, uint32_t ts, uint16_t seq);
static void jitterFlush(Peer *p);

/* temporary buffers */
static unsigned char *netBuf = NULL;
static float         *fbuf   = NULL;
static opus_int16    *pcmBuf = NULL;

/* main */
int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    initializePeers();
    coroutine_init();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port   = atoi(argv[++i]);
            myPort = port;
        } else if (!strcmp(argv[i], "-h")) {
            printf("Usage: %s [-p port] [peer[:port]]...\n", argv[0]);
            return 0;
        } else {
            char *cp   = strdup(argv[i]);
            char *addr = cp;
            int   prt  = DEFAULT_PORT;
            char *c    = strchr(cp, ':');
            if (c) {
                *c   = 0;
                prt  = atoi(c + 1);
            }
            addPeer(addr, prt);
            free(cp);
        }
    }

    signal(SIGINT, signalHandler);
    getLocalIPAddresses();

    if (!setupSSL())       return 1;
    if (!initAudioDevice()) return 1;

    int err;
    opusEnc = opus_encoder_create(SAMPLE_RATE, CAPTURE_CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        fprintf(stderr,
                "Opus enc init failed: %s\n",
                opus_strerror(err));
    }

    opusDec = opus_decoder_create(SAMPLE_RATE, PLAYBACK_CHANNELS, &err);
    if (err != OPUS_OK) {
        fprintf(stderr,
                "Opus dec init failed: %s\n",
                opus_strerror(err));
    }

    opus_encoder_ctl(opusEnc, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(opusEnc, OPUS_SET_VBR(1));
    opus_encoder_ctl(opusEnc, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(opusEnc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(opusEnc, OPUS_SET_PACKET_LOSS_PERC(5));

    if (!startServer(port))
        return 1;

    coroutine_go(acceptConnectionCoroutine, NULL);
    coroutine_go(discoveryCoroutine, NULL);
    announceSelf();
    coroutine_go(dtlsCoroutine, NULL);

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
            struct sockaddr_in *sa =
                (struct sockaddr_in*)ifa->ifa_addr;
            char buf[MAX_ADDR_STR];
            inet_ntop(AF_INET, &sa->sin_addr, buf, MAX_ADDR_STR);
            if (localAddrCount < MAX_LOCAL_ADDRS) {
                strcpy(localAddrs[localAddrCount++], buf);
            }
            if (!(ifa->ifa_flags & IFF_LOOPBACK) &&
                !myAddress[0]) {
                strcpy(myAddress, buf);
            }
        }
    }
    freeifaddrs(ifp);
    if (!myAddress[0] && localAddrCount>0) {
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
    (void)pre; (void)ctx;
    return 1;
}

static bool setupSSL(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    netState.tlsCtx  = SSL_CTX_new(TLS_method());
    netState.dtlsCtx = SSL_CTX_new(DTLS_method());
    if (!netState.tlsCtx || !netState.dtlsCtx) {
        ERR_print_errors_fp(stderr);
        return false;
    }

    SSL_CTX_set_verify(netState.tlsCtx, SSL_VERIFY_PEER, verifyCert);
    SSL_CTX_set_verify(netState.dtlsCtx, SSL_VERIFY_PEER, verifyCert);

    if (access("cert.pem", F_OK) ||
        access("key.pem",  F_OK)) {
        fprintf(stderr,
            "[WARN] missing cert.pem/key.pem\n"
            "openssl req -x509 -newkey rsa:4096 -keyout key.pem \\\n"
            "  -out cert.pem -days 365 -nodes\n");
        return false;
    }

    if (SSL_CTX_use_certificate_file(netState.tlsCtx,
                   "cert.pem", SSL_FILETYPE_PEM)  <= 0 ||
        SSL_CTX_use_PrivateKey_file(netState.tlsCtx,
                   "key.pem",  SSL_FILETYPE_PEM)  <= 0 ||
        !SSL_CTX_check_private_key(netState.tlsCtx)) {
        ERR_print_errors_fp(stderr);
        return false;
    }
    if (SSL_CTX_use_certificate_file(netState.dtlsCtx,
                   "cert.pem", SSL_FILETYPE_PEM)  <= 0 ||
        SSL_CTX_use_PrivateKey_file(netState.dtlsCtx,
                   "key.pem",  SSL_FILETYPE_PEM)  <= 0 ||
        !SSL_CTX_check_private_key(netState.dtlsCtx)) {
        ERR_print_errors_fp(stderr);
        return false;
    }

    return true;
}

/* clean up all SSL, sockets */
static void cleanupSSL(void) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->ssl) {
            SSL_shutdown(p->ssl);
            SSL_free(p->ssl);
        }
        if (p->dtls) {
            SSL_shutdown(p->dtls);
            SSL_free(p->dtls);
        }
        if (p->socketFd >= 0)
            close(p->socketFd);
    }
    if (netState.audioFd       >= 0) close(netState.audioFd);
    if (netState.listenFd      >= 0) close(netState.listenFd);
    if (netState.discoverySocket>=0) close(netState.discoverySocket);
    if (netState.tlsCtx)  SSL_CTX_free(netState.tlsCtx);
    if (netState.dtlsCtx) SSL_CTX_free(netState.dtlsCtx);
    ERR_free_strings();
    EVP_cleanup();
}

/* audio callback into ringbuffers */
static void ma_callback(ma_device *dev, void *out_, const void *in_, ma_uint32 frameCount) {
    AudioContext *ctx = (AudioContext*)dev->pUserData;

    if (in_) {
        size_t bytesToWrite =
            frameCount * dev->capture.channels * sizeof(float);
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
        float *dst = (float*)out_;
        size_t frameBytes = frameCount * dev->playback.channels * sizeof(float);
        ma_uint32 availBytes= ma_rb_available_read(&ctx->playbackRB);

        if (availBytes < frameBytes) return;

        size_t sampleBytes = dev->playback.channels * sizeof(float);
        ma_uint32 availFrames = availBytes / sampleBytes;
        ma_uint32 toCopyFrames = availFrames < frameCount ? availFrames : frameCount;
        size_t toCopyBytes = toCopyFrames * sampleBytes;

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

/* init miniaudio, ringbuffers, BPF etc */
static bool initAudioDevice(void) {
    size_t captureBytes  = SAMPLE_RATE * CAPTURE_CHANNELS * sizeof(float) * 2;
    size_t playbackBytes = SAMPLE_RATE * PLAYBACK_CHANNELS * sizeof(float) * 2;

    audioCtx.captureBufferData = malloc(captureBytes);
    audioCtx.playbackBufferData = malloc(playbackBytes);
    if (!audioCtx.captureBufferData || !audioCtx.playbackBufferData) {
        fprintf(stderr, "[ERR] alloc\n");
        return false;
    }
    if (ma_rb_init(captureBytes, audioCtx.captureBufferData, NULL, &audioCtx.captureRB) != MA_SUCCESS 
        || ma_rb_init(playbackBytes, audioCtx.playbackBufferData, NULL, &audioCtx.playbackRB) != MA_SUCCESS) {
        fprintf(stderr, "[ERR] rb init\n");
        return false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.sampleRate         = SAMPLE_RATE;
    cfg.periodSizeInFrames = FRAME_SIZE;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.capture.format     = ma_format_f32;
    cfg.playback.format    = ma_format_f32;
    cfg.capture.channels   = CAPTURE_CHANNELS;
    cfg.playback.channels  = PLAYBACK_CHANNELS;
    cfg.dataCallback       = ma_callback;
    cfg.pUserData          = &audioCtx;

    if (ma_device_init(NULL, &cfg, &audioCtx.device) != MA_SUCCESS ||
        ma_device_start(&audioCtx.device) != MA_SUCCESS) {
        fprintf(stderr, "[ERR] audio dev\n");
        return false;
    }
    fprintf(stderr, "[INFO] Audio @ %d Hz\n", SAMPLE_RATE);

    fbuf   = malloc(FRAME_SIZE * CAPTURE_CHANNELS * sizeof(float));
    pcmBuf = malloc(FRAME_SIZE * CAPTURE_CHANNELS * sizeof(opus_int16));
    netBuf = malloc(MAX_PACKET_SIZE);

    double fLow  = BPF_LOW_CUTOFF;
    double fHigh = BPF_HIGH_CUTOFF;
    double fc    = sqrt(fLow * fHigh);
    double Q     = fc / (fHigh - fLow);

    ma_bpf2_config bpfConfig = ma_bpf2_config_init(ma_format_f32, CAPTURE_CHANNELS, SAMPLE_RATE, fc, Q);
    if (ma_bpf2_init(&bpfConfig, NULL, &audioCtx.bpf) != MA_SUCCESS) {
        fprintf(stderr, "[ERR] bpf init\n");
        return false;
    }
    audioCtx.bpfInitialized = true;
    return true;
}

static void cleanupAudioDevice(void) {
    if (audioCtx.bpfInitialized) {
        ma_bpf2_uninit(&audioCtx.bpf, NULL);
    }
    ma_device_uninit(&audioCtx.device);
    ma_rb_uninit(&audioCtx.captureRB);
    ma_rb_uninit(&audioCtx.playbackRB);
    free(audioCtx.captureBufferData);
    free(audioCtx.playbackBufferData);
}

static void initializePeers(void) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        p->state         = PEER_DISCONNECTED;
        p->socketFd      = -1;
        p->ssl           = NULL;
        p->bytesSent     = p->bytesReceived = 0;
        p->udpAddrLen    = 0;
        p->initiator     = false;
        p->dtls          = NULL;
        p->dtlsStarted   = false;
        p->dtlsConnected = false;
        p->jitter.start = 0;
        p->jitter.count = 0;
        p->jitter.initialized = false;
    }
    netState.peerCount = 0;
}

/* TCP listen + discovery UDP + audio UDP */
static bool startServer(int port) {
    struct sockaddr_in sa = {0};
    int opt = 1;

    /* TCP control */
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port        = htons(port);
    netState.listenFd  = socket(AF_INET,
                                SOCK_STREAM, 0);
    if (netState.listenFd < 0) {
        perror("sock");
        return false;
    }
    setsockopt(netState.listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(netState.listenFd, F_SETFL, fcntl(netState.listenFd, F_GETFL, 0) | O_NONBLOCK);
    if (bind(netState.listenFd, (void*)&sa, sizeof sa) < 0) {
        perror("bind");
        return false;
    }
    if (listen(netState.listenFd, MAX_PEERS) < 0) {
        perror("listen");
        return false;
    }
    fprintf(stderr, "[INFO] TCP %d\n", port);

    /* peer discovery */
    struct sockaddr_in da = {0};
    da.sin_family      = AF_INET;
    da.sin_addr.s_addr = INADDR_ANY;
    da.sin_port        = htons(PEER_DISCOVERY_PORT);
    netState.discoverySocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (netState.discoverySocket < 0) {
        perror("udp");
        return false;
    }
    setsockopt(netState.discoverySocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(netState.discoverySocket, F_SETFL, fcntl(netState.discoverySocket, F_GETFL, 0) | O_NONBLOCK);
    if (bind(netState.discoverySocket, (void*)&da, sizeof da) < 0) {
        perror("udpb");
        return false;
    }
    setsockopt(netState.discoverySocket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof opt);
    fprintf(stderr, "[INFO] UDP %d\n", PEER_DISCOVERY_PORT);

    /* audio DTLS */
    netState.audioFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (netState.audioFd < 0) {
        perror("audio udp socket");
        return false;
    }
    setsockopt(netState.audioFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(netState.audioFd, F_SETFL, fcntl(netState.audioFd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in aua = {0};
    aua.sin_family      = AF_INET;
    aua.sin_addr.s_addr = INADDR_ANY;
    aua.sin_port        = htons(port);
    if (bind(netState.audioFd, (void*)&aua, sizeof aua) < 0) {
        perror("audio udpb");
        return false;
    }
    fprintf(stderr, "[INFO] DTLS/UDP %d\n", port);

    return true;
}

/* new TCP incoming */
static void acceptConnectionCoroutine(void *arg) {
    (void)arg;
    while (isRunning) {
        struct sockaddr_in ca;
        socklen_t al = sizeof(ca);
        int cfd = accept(netState.listenFd, (void*)&ca, &al);
        if (cfd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept");
            coroutine_sleep_read( netState.listenFd);
            continue;
        }
        fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL,0) | O_NONBLOCK);
        int idx = -1;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (netState.peers[i].state == PEER_DISCONNECTED) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            close(cfd);
            continue;
        }
        Peer *p = &netState.peers[idx];
        inet_ntop(AF_INET, &ca.sin_addr, p->address, MAX_ADDR_STR);
        p->port            = ntohs(ca.sin_port);
        p->udpAddr         = ca;
        p->udpAddrLen      = sizeof p->udpAddr;
        p->initiator       = false;
        p->socketFd        = cfd;
        p->state           = PEER_CONNECTING;
        p->lastActivity    = time(NULL);
        fprintf(stderr, "[IN] TCP %s:%d\n", p->address, p->port);
        p->ssl = SSL_new(netState.tlsCtx);
        SSL_set_fd(p->ssl, cfd);
        coroutine_go(handlePeerCoroutine, p);
    }
}

/* outgoing TCP connect */
static void connectToPeerCoroutine(void *arg) {
    Peer *p = (Peer*)arg;
    p->state        = PEER_CONNECTING;
    p->lastActivity = time(NULL);
    p->socketFd     = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(p->socketFd, F_SETFL, fcntl(p->socketFd, F_GETFL,0)|O_NONBLOCK);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(p->port);
    inet_pton(AF_INET, p->address, &sa.sin_addr);
    int r = connect(p->socketFd, (void*)&sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS)
        goto fail;
    if (r < 0) {
        coroutine_sleep_write( p->socketFd);
        socklen_t e, el = sizeof(e);
        if (getsockopt(p->socketFd, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e)
            goto fail;
    }
    fprintf(stderr, "[OUT] TCP %s:%d ok\n", p->address, p->port);
    p->ssl = SSL_new(netState.tlsCtx);
    SSL_set_fd(p->ssl, p->socketFd);
    do {
        r = SSL_connect(p->ssl);
        if (r <= 0) {
            int e = SSL_get_error(p->ssl, r);
            if (e == SSL_ERROR_WANT_READ) {
                coroutine_sleep_read( p->socketFd);
            } else if (e == SSL_ERROR_WANT_WRITE) {
                coroutine_sleep_write( p->socketFd);
            } else {
                goto fail;
            }
        }
    } while (r <= 0);

    /* send HELLO */
    unsigned char hello[3] = {
        PACKET_HELLO,
        (unsigned char)((myPort>>8)&0xFF),
        (unsigned char)(myPort &0xFF)
    };
    SSL_write(p->ssl, hello, sizeof hello);
    SSL_set_mode(p->ssl,
     SSL_MODE_ENABLE_PARTIAL_WRITE |
     SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    p->state = PEER_CONNECTED;
    netState.peerCount++;
    coroutine_go(handlePeerCoroutine, p);
    return;

fail:
    if (p->ssl)   SSL_free(p->ssl);
    if (p->socketFd>=0) close(p->socketFd);
    p->ssl       = NULL;
    p->socketFd  = -1;
    p->state     = PEER_DISCONNECTED;
}

/* handle TCP control messages */
static void handlePeerCoroutine(void *arg) {
    Peer *p = (Peer*)arg;
    while (isRunning) {
        if (p->state == PEER_CONNECTING) {
            int r;
            do {
                r = SSL_accept(p->ssl);
                if (r <= 0) {
                    int e = SSL_get_error(p->ssl,r);
                    if (e == SSL_ERROR_WANT_READ) {
                        coroutine_sleep_read( p->socketFd);
                    } else if (e == SSL_ERROR_WANT_WRITE) {
                        coroutine_sleep_write( p->socketFd);
                    } else {
                        goto cleanup;
                    }
                }
            } while (r <= 0);
            p->state = PEER_CONNECTED;
            netState.peerCount++;
        }

        coroutine_sleep_read(p->socketFd);
        unsigned char buf[MAX_BUFFER_SIZE];
        int n = SSL_read(p->ssl, buf, sizeof(buf));
        if (n <= 0) {
            int e = SSL_get_error(p->ssl, n);
            if (e==SSL_ERROR_WANT_READ || e==SSL_ERROR_WANT_WRITE)
                continue;
            goto cleanup;
        }
        p->lastActivity  = time(NULL);
        p->bytesReceived+= n;

        /* control only: HELLO, HELLO_ACK, PEER_LIST */
        if (n >= 3 && buf[0]==PACKET_HELLO) {
            int theirPort = (buf[1]<<8)|(buf[2]&0xFF);
            p->port = theirPort;
            unsigned char ack[3] = {
              PACKET_HELLO_ACK,
              (myPort>>8)&0xFF,
              myPort&0xFF
            };
            SSL_write(p->ssl, ack, 3);
            sendPeerList(p);
        } else if (buf[0]==PACKET_PEER_LIST) {
            handlePeerList((char*)buf+1, n-1);
        }
    }
cleanup:
    if (p->ssl)   { SSL_shutdown(p->ssl); SSL_free(p->ssl); }
    if (p->socketFd>=0) close(p->socketFd);
    if (p->state==PEER_CONNECTED) {
        p->state      = PEER_DISCONNECTED;
        netState.peerCount--;
    }
}

/* broadcast our presence via UDP */
static void discoveryCoroutine(void *arg) {
    (void)arg;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    while (isRunning) {
        char buf[MAX_BUFFER_SIZE];
        int n = recvfrom(netState.discoverySocket, buf, sizeof buf, 0, (struct sockaddr*)&sa, &sl);
        if (n > 0) {
            buf[n] = 0;
            if (!strncmp(buf, "VOICECHAT:",10)) {
                char *ipp = buf+10;
                char *c   = strchr(ipp, ':');
                if (c) {
                    *c=0;
                    int pr = atoi(c+1);
                    char sip[MAX_ADDR_STR];
                    inet_ntop(AF_INET,&sa.sin_addr, sip,MAX_ADDR_STR);
                    if (isLocalAddress(sip) && pr==myPort)
                        continue;
                    if (!isPeerConnected(ipp, pr)) {
                        addPeer(ipp, pr);
                    }
                }
            }
        } else if (errno!=EAGAIN && errno!=EWOULDBLOCK) {
            perror("recvfrom");
        } else {
            coroutine_sleep_read( netState.discoverySocket);
        }
        static time_t last=0;
        time_t now=time(NULL);
        if (now-last>30) {
            announceSelf();
            last = now;
        }
    }
}

/* read exactly bytes from ringbuffer */
static void rb_read_exact(ma_rb *rb, void *dst, size_t bytesToRead) {
    size_t part;
    void  *p;
    ma_rb_acquire_read(rb, &part, &p);
    if (part > bytesToRead) part = bytesToRead;
    memcpy(dst, p, part);
    ma_rb_commit_read(rb, part);

    size_t rem = bytesToRead - part;
    if (rem>0) {
        ma_rb_acquire_read(rb, &part, &p);
        if (part>rem) part=rem;
        memcpy((char*)dst+part, p, rem);
        ma_rb_commit_read(rb, rem);
    }
}

/* capture -> encode -> DTLS send */
static void processAudioIO(void) {
    size_t frameBytes = FRAME_SIZE * CAPTURE_CHANNELS * sizeof(float);
    ma_uint32 availBytes = ma_rb_available_read(&audioCtx.captureRB);
    if (availBytes < frameBytes) return;

    rb_read_exact(&audioCtx.captureRB, fbuf, frameBytes);

    if (audioCtx.bpfInitialized) {
        ma_bpf2_process_pcm_frames(&audioCtx.bpf, fbuf, fbuf, FRAME_SIZE);
    }

    /* loopback to own playback */
    {
        size_t   wAvail;
        void    *wPtr;
        ma_rb_acquire_write(&audioCtx.playbackRB, &wAvail, &wPtr);
        if (wAvail >= frameBytes) {
            memcpy(wPtr, fbuf, frameBytes);
            ma_rb_commit_write( &audioCtx.playbackRB, frameBytes);
        } else {
            ma_rb_commit_write( &audioCtx.playbackRB, 0);
        }
    }

    /* float -> int16 pcm */
    int totalSamples = FRAME_SIZE * CAPTURE_CHANNELS;
    for (int i = 0; i < totalSamples; i++) {
        pcmBuf[i] = (opus_int16) (fbuf[i] * 32767.0f);
    }

    int nb = opus_encode(opusEnc, pcmBuf, FRAME_SIZE, netBuf, MAX_PACKET_SIZE);
    if (nb > 0) {
        broadcastAudioDTLS(netBuf, nb);
    }
}

static uint32_t txTs  = 0;
static uint16_t txSeq = 0;

/* prepend TS -> SEQ and DTLS‐send */
static void broadcastAudioDTLS(const unsigned char *opus, int opusLen) {
    unsigned char pkt[MAX_DTLS_PACKET_SIZE];
    uint32_t be_ts  = htonl(txTs);
    uint16_t be_seq = htons(txSeq);
    memcpy(pkt + 0, &be_ts,  4);
    memcpy(pkt + 4, &be_seq, 2);
    memcpy(pkt + 6, opus,    opusLen);
    int pktSz = 6 + opusLen;
    txTs  += FRAME_SIZE;
    txSeq += 1;

    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->dtlsConnected) {
            int w = SSL_write(p->dtls, pkt, pktSz);
            if (w > 0) {
                p->bytesSent += w;
                flushDTLS(p);
            }
        }
    }
}

/* fade out levels, detect TCP timeout */
static void updateAudioLevels(void) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->state == PEER_CONNECTED) {
            p->audioLevel *= 0.9f;
            if (time(NULL) - p->lastActivity>60) {
                fprintf(stderr,
                  "[TIMEOUT] %s:%d\n",
                  p->address, p->port);
                if (p->ssl) {
                    SSL_shutdown(p->ssl);
                    SSL_free(p->ssl);
                    p->ssl = NULL;
                }
                if (p->socketFd>=0)
                    close(p->socketFd);
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
    printf("Mic ring: %4lu / %4u\n", avail/sizeof(float), SAMPLE_RATE);
    printf("\nPeers: %d\n", netState.peerCount);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->state == PEER_CONNECTED) {
            printf("%-15s ", p->address);
            int bar  = 15;
            int fill = p->audioLevel * bar * 5;
            if (fill > bar) fill=bar;
            printf("[");
            for (int j = 0; j < bar; j++)
                putchar(j < fill ? '#' : ' ');
            printf("] ↑%.1f↓%.1f\n", p->bytesSent/1024.0f, p->bytesReceived/1024.0f);
        }
    }
    printf("\nCtrl+C to exit\n");
    fflush(stdout);
}

/* RMS */
static float calculateRMS(const float *b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++)
        s += b[i]*b[i];
    return sqrt(s/n);
}

/* peers by addr:port */
static bool isPeerConnected(const char *addr, int port) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if ((p->state==PEER_CONNECTED||
             p->state==PEER_CONNECTING) &&
            !strcmp(p->address, addr) &&
            p->port == port)
            return true;
    }
    return false;
}

/* spawn new TCP + remember UDP addr */
static void addPeer(const char *addr, int port) {
    if (isPeerConnected(addr, port)) return;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &netState.peers[i];
        if (p->state == PEER_DISCONNECTED) {
            strcpy(p->address, addr);
            p->port = port;
            memset(&p->udpAddr,0,sizeof p->udpAddr);
            p->udpAddr.sin_family = AF_INET;
            p->udpAddr.sin_port = htons(port);
            inet_pton(AF_INET, addr, &p->udpAddr.sin_addr);
            p->udpAddrLen  = sizeof(p->udpAddr);
            p->initiator = true;
            coroutine_go(connectToPeerCoroutine, p);
            return;
        }
    }
    fprintf(stderr, "[WARN] no slot %s:%d\n", addr, port);
}

static void handlePeerList(const char *data, int len) {
    char *cp = malloc(len+1);
    memcpy(cp, data, len);
    cp[len]=0;
    char *tok, *save;
    for (tok=strtok_r(cp, ";",&save); tok; tok=strtok_r(NULL,";",&save)) {
        char *c = strchr(tok,':');
        if (!c) continue;
        *c=0;
        int pr = atoi(c+1);
        if (isLocalAddress(tok) && pr==myPort) continue;
        if (!isPeerConnected(tok,pr))
            addPeer(tok,pr);
    }
    free(cp);
}

static void sendPeerList(Peer *p) {
    char buf[MAX_BUFFER_SIZE];
    int  off = 0;
    buf[off++] = PACKET_PEER_LIST;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *q = &netState.peers[i];
        if (q->state==PEER_CONNECTED) {
            off += snprintf(buf+off, MAX_BUFFER_SIZE-off, "%s:%d;", q->address, q->port);
        }
    }
    off += snprintf(buf+off, MAX_BUFFER_SIZE-off, "%s:%d;", myAddress, myPort);
    SSL_write(p->ssl, buf, off);
}

static void announceSelf(void) {
    char msg[128];
    snprintf(msg, sizeof msg, "VOICECHAT:%s:%d", myAddress, myPort);
    struct sockaddr_in ba = {0};
    ba.sin_family      = AF_INET;
    ba.sin_port        = htons(PEER_DISCOVERY_PORT);
    ba.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(netState.discoverySocket, msg, strlen(msg), 0, (void*)&ba, sizeof ba);
    fprintf(stderr, "[DISC] self\n");
}

static void flushDTLS(Peer *p) {
    BIO *wbio = SSL_get_wbio(p->dtls);
    unsigned char buf[1500];
    int len;
    while ((len = BIO_read(wbio, buf, sizeof buf))>0) {
        sendto(netState.audioFd, buf, len, 0, (struct sockaddr*)&p->udpAddr, p->udpAddrLen);
    }
}

static void jitterInsert(Peer *p, float *pcm, int samples, uint32_t ts, uint16_t seq) {
    JitterBuffer *jb = &p->jitter;
    if (!jb->initialized) {
        jb->initialized = true;
        jb->expectedTs  = ts;
    }
    int offset = (int)((ts - jb->expectedTs) / FRAME_SIZE);
    if (offset < 0 || offset >= JITTER_SIZE)
        return;

    int idx = (jb->start + offset) % JITTER_SIZE;
    memcpy(jb->pcm[idx], pcm, samples * sizeof *pcm);
    jb->ts[idx]  = ts;
    jb->seq[idx] = seq;
    int want = offset+1;
    if (jb->count < want) jb->count = want;
}

static void jitterFlush(Peer *p) {
    JitterBuffer *jb = &p->jitter;
    while (jb->count > 0) {
        bool have = jb->ts[jb->start] == jb->expectedTs;
        size_t bytes = FRAME_SIZE * PLAYBACK_CHANNELS * sizeof(float);
        void *wPtr;
        size_t wAvail;

        if (ma_rb_acquire_write( &audioCtx.playbackRB, &wAvail, &wPtr) != MA_SUCCESS)
            break;

        if (wAvail < bytes) {
            ma_rb_commit_write(&audioCtx.playbackRB, 0);
            break;
        }

        if (have) {
            memcpy(wPtr, jb->pcm[jb->start], bytes);
        } else {
            memset(wPtr, 0, bytes);
        }

        ma_rb_commit_write(&audioCtx.playbackRB, bytes);
        jb->start = (jb->start + 1) % JITTER_SIZE;
        jb->count--;
        jb->expectedTs += FRAME_SIZE;
    }
}

/* single loop: start DTLS + recv/send */
static void dtlsCoroutine(void *arg) {
    (void)arg;
    unsigned char inbuf[MAX_PACKET_SIZE];
    unsigned char appbuf[MAX_PACKET_SIZE];
    while (isRunning) {
        /* init any new p->dtls */
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = &netState.peers[i];
            if (p->ssl && p->state == PEER_CONNECTED && !p->dtls) {
                p->dtls = SSL_new(netState.dtlsCtx);
                SSL_set_mtu(p->dtls, 1500);

                SSL_use_certificate_file( p->dtls, "cert.pem", SSL_FILETYPE_PEM);
                SSL_use_PrivateKey_file( p->dtls, "key.pem", SSL_FILETYPE_PEM);

                BIO *rbio = BIO_new(BIO_s_mem());
                BIO *wbio = BIO_new(BIO_s_mem());
                SSL_set_bio(p->dtls, rbio, wbio);

                if (p->initiator) {
                    SSL_set_connect_state(p->dtls);
                } else {
                    SSL_set_accept_state(p->dtls);
                }

                p->dtlsStarted = true;
                if (p->initiator) {
                    SSL_do_handshake(p->dtls);
                    flushDTLS(p);
                }
            }
        }

        /* wait for UDP data */
        coroutine_sleep_read(netState.audioFd);

        /* recv all datagrams */
        for (;;) {
            struct sockaddr_in sa;
            socklen_t al = sizeof sa;
            int n = recvfrom(netState.audioFd, inbuf, sizeof inbuf, 0, (void*)&sa, &al);
            if (n < 0) {
                if (errno==EAGAIN|| errno==EWOULDBLOCK) break;
                perror("dtls recvfrom");
                break;
            }
            /* find peer by UDP src */
            Peer *p = NULL;
            for (int i = 0; i < MAX_PEERS; i++) {
                Peer *q = &netState.peers[i];
                if (q->dtls && al == q->udpAddrLen &&
                    !memcmp(&q->udpAddr.sin_addr, &sa.sin_addr, sizeof sa.sin_addr) &&
                    q->udpAddr.sin_port == sa.sin_port) {
                    p = q;
                    break;
                }
            }
            if (!p) continue;

            /* feed to dtls */
            BIO_write(SSL_get_rbio(p->dtls), inbuf, n);

            /* handshake? */
            if (!p->dtlsConnected) {
                int r, err;
                do {
                    r = SSL_do_handshake(p->dtls);
                    flushDTLS(p);
                    if (r == 1) {
                        p->dtlsConnected = true;
                        break;
                    }
                    err = SSL_get_error(p->dtls,r);
                } while (err==SSL_ERROR_WANT_READ ||
                         err==SSL_ERROR_WANT_WRITE);

                if (!p->dtlsConnected &&
                    err!=SSL_ERROR_WANT_READ &&
                    err!=SSL_ERROR_WANT_WRITE)
                {
                    SSL_free(p->dtls);
                    p->dtls = NULL;
                }
                continue;
            }

            /* application data */
            int r;
            while ((r = SSL_read(p->dtls, appbuf, sizeof appbuf))>0) {
                uint32_t ts = ntohl(*(uint32_t*)(appbuf+0));
                uint16_t seq = ntohs(*(uint16_t*)(appbuf+4));
                unsigned char *opusData = appbuf+6;
                int opusLen = r - 6;
                /* decode */
                int frames = opus_decode(opusDec, opusData, opusLen, pcmBuf, FRAME_SIZE, 0);
                if (frames > 0) {
                    int total = frames * PLAYBACK_CHANNELS;
                    for (int j = 0; j < total; j++)
                        fbuf[j] = pcmBuf[j]/32767.0f;
                    p->audioLevel = calculateRMS(fbuf, total);
                    jitterInsert(p, fbuf, total, ts, seq);
                    jitterFlush(p);
                }
            }
        }
    }
}
