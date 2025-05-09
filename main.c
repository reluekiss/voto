// voicechat.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <opus/opus.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "coroutine.h"

// configuration
#define SAMPLE_RATE         48000
#define CAPTURE_CHANNELS    2
#define PLAYBACK_CHANNELS   1
#define FRAME_SIZE          960
#define MAX_PACKET_SIZE     4000
#define DEFAULT_PORT        14400
#define PEER_DISCOVERY_PORT 14401
#define MAX_BUFFER_SIZE     8192
#define MAX_PEERS           16
#define MAX_ADDR_STR        64

// packet types
typedef enum {
    PACKET_AUDIO     = 0,
    PACKET_PEER_LIST = 1,
    PACKET_HELLO     = 2,
    PACKET_HELLO_ACK = 3
} PacketType;

// peer state
typedef enum { PEER_DISCONNECTED=0, PEER_CONNECTING, PEER_CONNECTED } PeerState;

// peer struct
typedef struct {
    char        address[MAX_ADDR_STR];
    int         port;
    int         socketFd;
    SSL        *ssl;
    PeerState   state;
    time_t      lastActivity;
    float       audioLevel;
    uint64_t    bytesSent, bytesReceived;
    bool        audioDetected;
} Peer;

// network state
typedef struct {
    int       listenFd;
    int       discoverySocket;
    SSL_CTX  *ctx;
    Peer      peers[MAX_PEERS];
    int       peerCount;
} NetworkState;

static NetworkState netState = {0};

// audio context
typedef struct {
    ma_device device;
    ma_rb     captureRB;
    ma_rb     playbackRB;
    void     *captureBufferData;
    void     *playbackBufferData;
} AudioContext;

static AudioContext audioCtx = {0};

// Opus
static OpusEncoder *opusEnc = NULL;
static OpusDecoder *opusDec = NULL;

// local
static char  myAddress[MAX_ADDR_STR] = {0};
static int   myPort = DEFAULT_PORT;
static bool  isRunning = true;

// forward
static void signalHandler(int sig);
static void getOwnIPAddress(void);
static int  verifyCert(int pre, X509_STORE_CTX *ctx);
static bool setupSSL(void);
static void cleanupSSL(void);

static bool initAudioDevice(void);
static void cleanupAudioDevice(void);
static void ma_callback(ma_device *d, void *o, const void *i, ma_uint32 f);

static void initializePeers(void);
static bool startServer(int port);

static void acceptConnectionCoroutine(void *arg);
static void connectToPeerCoroutine(void *arg);
static void handlePeerCoroutine(void *arg);
static void discoveryCoroutine(void *arg);

static void processAudioIO(void);
static void broadcastAudioToPeers(const unsigned char *data,int sz);
static void updateAudioLevels(void);
static void drawVisualization(void);
static float calculateRMS(const float *buf,int n);

static bool isPeerConnected(const char *addr,int port);
static void addPeer(const char *addr,int port);
static void handlePeerList(Peer *p,const char *data,int len);
static void sendPeerList(Peer *p);
static void announceSelf(void);

//——————————————————————————————————————————————————————————————————————————————
// main
//——————————————————————————————————————————————————————————————————————————————
int main(int argc,char **argv){
    int port=DEFAULT_PORT;
    // parse
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-p")&&i+1<argc){
            port=atoi(argv[++i]);
            myPort=port;
        } else if(!strcmp(argv[i],"-h")){
            printf("Usage: %s [-p port] [peer[:port]]...\n",argv[0]);
            return 0;
        } else {
            char *cp=strdup(argv[i]);
            char *addr=cp;
            int prt=DEFAULT_PORT;
            char *c=strchr(cp,':');
            if(c){*c=0;prt=atoi(c+1);}
            addPeer(addr,prt);
            free(cp);
        }
    }
    // signal
    signal(SIGINT,signalHandler);
    getOwnIPAddress();
    if(!setupSSL())       return 1;
    if(!initAudioDevice())return 1;
    // Opus
    {
        int err;
        opusEnc=opus_encoder_create(SAMPLE_RATE,PLAYBACK_CHANNELS,OPUS_APPLICATION_VOIP,&err);
        opusDec=opus_decoder_create(SAMPLE_RATE,PLAYBACK_CHANNELS,&err);
        opus_encoder_ctl(opusEnc,OPUS_SET_BITRATE(64000));
    }
    initializePeers();
    coroutine_init();
    if(!startServer(port))return 1;
    coroutine_go(acceptConnectionCoroutine,NULL);
    coroutine_go(discoveryCoroutine,     NULL);
    announceSelf();
    // loop
    while(isRunning){
        processAudioIO();
        updateAudioLevels();
        coroutine_yield();
        static time_t last=0;
        time_t now=time(NULL);
        if(now-last>=1){
            drawVisualization();
            last=now;
        }
        usleep(10000);
    }
    cleanupSSL();
    cleanupAudioDevice();
    opus_encoder_destroy(opusEnc);
    opus_decoder_destroy(opusDec);
    printf("\nTerminated.\n");
    return 0;
}

//——————————————————————————————————————————————————————————————————————————————
// signal
//——————————————————————————————————————————————————————————————————————————————
static void signalHandler(int sig){(void)sig;isRunning=false;}

//——————————————————————————————————————————————————————————————————————————————
// own IP
//——————————————————————————————————————————————————————————————————————————————
static void getOwnIPAddress(void){
    struct ifaddrs *ifp,*ifa;
    if(getifaddrs(&ifp)==-1){perror("getifaddrs");strcpy(myAddress,"127.0.0.1");return;}
    for(ifa=ifp;ifa;ifa=ifa->ifa_next){
        if(!ifa->ifa_addr)continue;
        if(ifa->ifa_addr->sa_family==AF_INET &&
           !(ifa->ifa_flags&IFF_LOOPBACK)){
            struct sockaddr_in *sa=(void*)ifa->ifa_addr;
            inet_ntop(AF_INET,&sa->sin_addr,myAddress,MAX_ADDR_STR);
            break;
        }
    }
    freeifaddrs(ifp);
    if(!myAddress[0])strcpy(myAddress,"127.0.0.1");
    fprintf(stderr,"[INFO] IP: %s\n",myAddress);
}

//——————————————————————————————————————————————————————————————————————————————
// SSL
//——————————————————————————————————————————————————————————————————————————————
static int verifyCert(int pre, X509_STORE_CTX *ctx){
    (void)pre;(void)ctx;return 1;
}
static bool setupSSL(void){
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    netState.ctx=SSL_CTX_new(TLS_method());
    if(!netState.ctx){ERR_print_errors_fp(stderr);return false;}
    SSL_CTX_set_verify(netState.ctx,SSL_VERIFY_PEER,verifyCert);
    if(access("cert.pem",F_OK)|access("key.pem",F_OK)){
        fprintf(stderr,
          "[WARN] cert.pem/key.pem missing\n"
          "openssl req -x509 -newkey rsa:4096 -keyout key.pem "
          "-out cert.pem -days 365 -nodes\n");
        return false;
    }
    if(SSL_CTX_use_certificate_file(netState.ctx,"cert.pem",SSL_FILETYPE_PEM)<=0||
       SSL_CTX_use_PrivateKey_file  (netState.ctx,"key.pem",SSL_FILETYPE_PEM)<=0||
       !SSL_CTX_check_private_key(netState.ctx)){
        ERR_print_errors_fp(stderr);return false;
    }
    return true;
}
static void cleanupSSL(void){
    for(int i=0;i<MAX_PEERS;i++){
        Peer *p=&netState.peers[i];
        if(p->ssl){SSL_shutdown(p->ssl);SSL_free(p->ssl);p->ssl=NULL;}
        if(p->socketFd>=0){close(p->socketFd);p->socketFd=-1;}
    }
    if(netState.listenFd>=0)close(netState.listenFd);
    if(netState.discoverySocket>=0)close(netState.discoverySocket);
    if(netState.ctx)SSL_CTX_free(netState.ctx);
    ERR_free_strings();EVP_cleanup();
}

//——————————————————————————————————————————————————————————————————————————————
// miniaudio callback
//——————————————————————————————————————————————————————————————————————————————
static void ma_callback(ma_device *dev, void *out_, const void *in_, ma_uint32 fc){
    AudioContext *ctx=dev->pUserData;
    const float *in=(const float*)in_;
    float mono[FRAME_SIZE];
    if(dev->capture.channels==2){
        for(ma_uint32 i=0;i<fc;i++)
            mono[i]=0.5f*(in[2*i]+in[2*i+1]);
        in=mono;
    }
    // capture→ring
    size_t wA=ma_rb_available_write(&ctx->captureRB);
    size_t wB=fc*sizeof(float);
    if(wA>=wB){
        void *wp;size_t ws;
        ma_rb_acquire_write(&ctx->captureRB,&ws,&wp);
        memcpy(wp,in,wB);
        ma_rb_commit_write(&ctx->captureRB,wB);
    }
    // ring→playback
    size_t rA=ma_rb_available_read(&ctx->playbackRB);
    size_t rB=fc*sizeof(float);
    if(rA>=rB){
        void *rp;size_t rs;
        ma_rb_acquire_read(&ctx->playbackRB,&rs,&rp);
        memcpy(out_,rp,rB);
        ma_rb_commit_read(&ctx->playbackRB,rB);
    } else {
        memset(out_,0,rB);
    }
}

//——————————————————————————————————————————————————————————————————————————————
// init audio device
//——————————————————————————————————————————————————————————————————————————————
static bool initAudioDevice(void){
    ma_uint32 cap=SAMPLE_RATE;
    size_t    bytes=cap*sizeof(float);
    audioCtx.captureBufferData=malloc(bytes);
    audioCtx.playbackBufferData=malloc(bytes);
    if(!audioCtx.captureBufferData||!audioCtx.playbackBufferData){
        fprintf(stderr,"[ERR] alloc RB\n");return false;
    }
    if(ma_rb_init(bytes,audioCtx.captureBufferData,NULL,&audioCtx.captureRB)!=MA_SUCCESS||
       ma_rb_init(bytes,audioCtx.playbackBufferData,NULL,&audioCtx.playbackRB)!=MA_SUCCESS){
        fprintf(stderr,"[ERR] rb_init\n");return false;
    }
    ma_device_config cfg=ma_device_config_init(ma_device_type_duplex);
    cfg.sampleRate=SAMPLE_RATE;
    cfg.capture.format=ma_format_f32;cfg.capture.channels=CAPTURE_CHANNELS;
    cfg.playback.format=ma_format_f32;cfg.playback.channels=PLAYBACK_CHANNELS;
    cfg.dataCallback=ma_callback;cfg.pUserData=&audioCtx;
    if(ma_device_init(NULL,&cfg,&audioCtx.device)!=MA_SUCCESS||
       ma_device_start(&audioCtx.device)!=MA_SUCCESS){
        fprintf(stderr,"[ERR] dev_init\n");return false;
    }
    fprintf(stderr,"[INFO] Audio opened\n");
    return true;
}
static void cleanupAudioDevice(void){
    ma_device_uninit(&audioCtx.device);
    ma_rb_uninit(&audioCtx.captureRB);
    ma_rb_uninit(&audioCtx.playbackRB);
    free(audioCtx.captureBufferData);
    free(audioCtx.playbackBufferData);
}

//——————————————————————————————————————————————————————————————————————————————
// initialize peers
//——————————————————————————————————————————————————————————————————————————————
static void initializePeers(void){
    for(int i=0;i<MAX_PEERS;i++){
        netState.peers[i].state=PEER_DISCONNECTED;
        netState.peers[i].socketFd=-1;
        netState.peers[i].ssl=NULL;
    }
    netState.peerCount=0;
}

//——————————————————————————————————————————————————————————————————————————————
// start server + discovery
//——————————————————————————————————————————————————————————————————————————————
static bool startServer(int port){
    struct sockaddr_in sa={0};
    sa.sin_family=AF_INET;sa.sin_addr.s_addr=INADDR_ANY;sa.sin_port=htons(port);
    netState.listenFd=socket(AF_INET,SOCK_STREAM,0);
    if(netState.listenFd<0){perror("sock");return false;}
    int opt=1;
    setsockopt(netState.listenFd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof opt);
    fcntl(netState.listenFd,F_SETFL,fcntl(netState.listenFd,F_GETFL,0)|O_NONBLOCK);
    if(bind(netState.listenFd,(void*)&sa,sizeof sa)<0){perror("bind");return false;}
    if(listen(netState.listenFd,MAX_PEERS)<0){perror("listen");return false;}
    fprintf(stderr,"[INFO] TCP port %d\n",port);

    struct sockaddr_in da={0};
    da.sin_family=AF_INET;da.sin_addr.s_addr=INADDR_ANY;
    da.sin_port=htons(PEER_DISCOVERY_PORT);
    netState.discoverySocket=socket(AF_INET,SOCK_DGRAM,0);
    if(netState.discoverySocket<0){perror("udp");return false;}
    setsockopt(netState.discoverySocket,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof opt);
    fcntl(netState.discoverySocket,F_SETFL,fcntl(netState.discoverySocket,F_GETFL,0)|O_NONBLOCK);
    if(bind(netState.discoverySocket,(void*)&da,sizeof da)<0){perror("udpb");return false;}
    setsockopt(netState.discoverySocket,SOL_SOCKET,SO_BROADCAST,&opt,sizeof opt);
    fprintf(stderr,"[INFO] UDP port %d\n",PEER_DISCOVERY_PORT);
    return true;
}

//——————————————————————————————————————————————————————————————————————————————
// accept incoming TCP
//——————————————————————————————————————————————————————————————————————————————
static void acceptConnectionCoroutine(void *arg){
    (void)arg;
    while(isRunning){
        struct sockaddr_in ca; socklen_t al=sizeof ca;
        int cfd=accept(netState.listenFd,(void*)&ca,&al);
        if(cfd<0){
            if(errno!=EAGAIN&&errno!=EWOULDBLOCK)perror("accept");
            coroutine_sleep_read(netState.listenFd);
            continue;
        }
        fcntl(cfd,F_SETFL,fcntl(cfd,F_GETFL,0)|O_NONBLOCK);
        int idx=-1;
        for(int i=0;i<MAX_PEERS;i++)
            if(netState.peers[i].state==PEER_DISCONNECTED){idx=i;break;}
        if(idx<0){close(cfd);continue;}
        Peer *p=&netState.peers[idx];
        inet_ntop(AF_INET,&ca.sin_addr,p->address,MAX_ADDR_STR);
        p->port=ntohs(ca.sin_port);
        p->socketFd=cfd;
        p->state=PEER_CONNECTING;
        p->lastActivity=time(NULL);
        fprintf(stderr,"[IN] TCP %s:%d\n",p->address,p->port);
        p->ssl=SSL_new(netState.ctx); SSL_set_fd(p->ssl,cfd);
        coroutine_go(handlePeerCoroutine,p);
    }
}

//——————————————————————————————————————————————————————————————————————————————
// connect out
//——————————————————————————————————————————————————————————————————————————————
static void connectToPeerCoroutine(void *arg){
    Peer *p=(Peer*)arg;
    p->state=PEER_CONNECTING; p->lastActivity=time(NULL);
    p->socketFd=socket(AF_INET,SOCK_STREAM,0);
    fcntl(p->socketFd,F_SETFL,fcntl(p->socketFd,F_GETFL,0)|O_NONBLOCK);
    struct sockaddr_in sa={0};
    sa.sin_family=AF_INET;sa.sin_port=htons(p->port);
    inet_pton(AF_INET,p->address,&sa.sin_addr);
    int r=connect(p->socketFd,(void*)&sa,sizeof sa);
    if(r<0&&errno!=EINPROGRESS)goto fail;
    if(r<0){
        coroutine_sleep_write(p->socketFd);
        int e,el=sizeof e;
        if(getsockopt(p->socketFd,SOL_SOCKET,SO_ERROR,&e,&el)<0||e)goto fail;
    }
    fprintf(stderr,"[OUT] TCP %s:%d ok\n",p->address,p->port);
    p->ssl=SSL_new(netState.ctx);SSL_set_fd(p->ssl,p->socketFd);
    do{
        r=SSL_connect(p->ssl);
        if(r<=0){
            int e=SSL_get_error(p->ssl,r);
            if(e==SSL_ERROR_WANT_READ)coroutine_sleep_read(p->socketFd);
            else if(e==SSL_ERROR_WANT_WRITE)coroutine_sleep_write(p->socketFd);
            else goto fail;
        }
    }while(r<=0);
    fprintf(stderr,"[OUT] SSL %s:%d ok\n",p->address,p->port);
    // mark connected
    if(p->state!=PEER_CONNECTED){
        p->state=PEER_CONNECTED;
        netState.peerCount++;
    }
    unsigned char h=PACKET_HELLO;
    SSL_write(p->ssl,&h,1);
    coroutine_go(handlePeerCoroutine,p);
    return;
fail:
    if(p->ssl)SSL_free(p->ssl),p->ssl=NULL;
    if(p->socketFd>=0)close(p->socketFd),p->socketFd=-1;
    p->state=PEER_DISCONNECTED;
}

//--------------------------------------------------------------------------------------
// handle handshake + I/O
//--------------------------------------------------------------------------------------
static void handlePeerCoroutine(void *arg){
    Peer *p=(Peer*)arg;
    unsigned char buf[MAX_PACKET_SIZE];
    if(p->state==PEER_CONNECTING && p->ssl){
        int r;
        do{
            r=SSL_accept(p->ssl);
            if(r<=0){
                int e=SSL_get_error(p->ssl,r);
                if(e==SSL_ERROR_WANT_READ)coroutine_sleep_read(p->socketFd);
                else if(e==SSL_ERROR_WANT_WRITE)coroutine_sleep_write(p->socketFd);
                else goto cleanup;
            }
        }while(r<=0);
        fprintf(stderr,"[IN] SSL %s:%d ok\n",p->address,p->port);
        if(p->state!=PEER_CONNECTED){
            p->state=PEER_CONNECTED;
            netState.peerCount++;
        }
        unsigned char a=PACKET_HELLO_ACK;
        SSL_write(p->ssl,&a,1);
        sendPeerList(p);
    }
    while(isRunning && p->state==PEER_CONNECTED){
        coroutine_sleep_read(p->socketFd);
        int n=SSL_read(p->ssl,buf,sizeof buf);
        if(n<=0){
            int e=SSL_get_error(p->ssl,n);
            if(e==SSL_ERROR_WANT_READ||e==SSL_ERROR_WANT_WRITE)continue;
            goto cleanup;
        }
        p->lastActivity=time(NULL);
        p->bytesReceived+=n;
        switch(buf[0]){
        case PACKET_AUDIO:{
            opus_int16 pcm[FRAME_SIZE];
            int frames=opus_decode(opusDec,buf+1,n-1,pcm,FRAME_SIZE,0);
            fprintf(stderr,"[AUDIO] decoded %d\n",frames);
            if(frames>0){
                float fbuf[FRAME_SIZE];
                for(int i=0;i<frames;i++)fbuf[i]=pcm[i]/32767.0f;
                p->audioLevel=calculateRMS(fbuf,frames);
                p->audioDetected=p->audioLevel>0.01f;
                // write to playbackRB
                size_t wS;void*wP;
                if(ma_rb_acquire_write(&audioCtx.playbackRB,&wS,&wP)==MA_SUCCESS){
                    size_t b=frames*sizeof(float);
                    if(wS>=b){
                        memcpy(wP,fbuf,b);
                        ma_rb_commit_write(&audioCtx.playbackRB,b);
                    }else ma_rb_commit_write(&audioCtx.playbackRB,0);
                }
            }
        }break;
        case PACKET_PEER_LIST:
            handlePeerList(p,(char*)buf+1,n-1);
            break;
        case PACKET_HELLO:{
            unsigned char a=PACKET_HELLO_ACK;
            SSL_write(p->ssl,&a,1);
            sendPeerList(p);
        }break;
        }
    }
cleanup:
    if(p->ssl)SSL_shutdown(p->ssl),SSL_free(p->ssl),p->ssl=NULL;
    if(p->socketFd>=0)close(p->socketFd),p->socketFd=-1;
    if(p->state==PEER_CONNECTED){
        netState.peerCount--;
        p->state=PEER_DISCONNECTED;
    }
}

//--------------------------------------------------------------------------------------
// discovery (UDP)
//--------------------------------------------------------------------------------------
static void discoveryCoroutine(void *arg){
    (void)arg;
    char buf[MAX_BUFFER_SIZE];
    struct sockaddr_in sa;
    socklen_t sl=sizeof sa;
    while(isRunning){
        int n=recvfrom(netState.discoverySocket,buf,sizeof buf,0,(struct sockaddr*)&sa,&sl);
        if(n>0){
            buf[n]=0;
            if(!strncmp(buf,"VOICECHAT:",10)){
                char*ipp=buf+10;
                char*c=strchr(ipp,':');
                if(c){
                    *c=0;int pr=atoi(c+1);
                    char sip[MAX_ADDR_STR];
                    inet_ntop(AF_INET,&sa.sin_addr,sip,MAX_ADDR_STR);
                    if(strcmp(sip,myAddress)||pr!=myPort){
                        if(!isPeerConnected(ipp,pr)){
                            fprintf(stderr,"[DISC] %s:%d\n",ipp,pr);
                            addPeer(ipp,pr);
                        }
                    }
                }
            }
        } else if(errno!=EAGAIN&&errno!=EWOULDBLOCK){
            perror("recvfrom");
        } else {
            coroutine_sleep_read(netState.discoverySocket);
        }
        static time_t last=0;time_t now=time(NULL);
        if(now-last>30){announceSelf();last=now;}
    }
}

//--------------------------------------------------------------------------------------
// process audio: read ring→encode→broadcast, also loopback to playback
//--------------------------------------------------------------------------------------
static void processAudioIO(void){
    ma_uint32 avail=ma_rb_available_read(&audioCtx.captureRB);
    if(avail<FRAME_SIZE*sizeof(float)){
        static int once=0;
        if(!once++)fprintf(stderr,"[AUDIO] ring empty: %u\n",avail);
        return;
    }
    fprintf(stderr,"[AUDIO] ring ok: %u\n",avail);

    float fbuf[FRAME_SIZE];
    size_t rS;void*rP;
    if(ma_rb_acquire_read(&audioCtx.captureRB,&rS,&rP)==MA_SUCCESS){
        size_t b=FRAME_SIZE*sizeof(float);
        if(rS>=b){
            memcpy(fbuf,rP,b);
            ma_rb_commit_read(&audioCtx.captureRB,b);
        } else {
            ma_rb_commit_read(&audioCtx.captureRB,0);
            return;
        }
    }

    // loopback
    size_t wS;void*wP;
    if(ma_rb_acquire_write(&audioCtx.playbackRB,&wS,&wP)==MA_SUCCESS){
        size_t b=FRAME_SIZE*sizeof(float);
        if(wS>=b){
            memcpy(wP,fbuf,b);
            ma_rb_commit_write(&audioCtx.playbackRB,b);
        } else ma_rb_commit_write(&audioCtx.playbackRB,0);
    }

    opus_int16 pcm[FRAME_SIZE];
    for(int i=0;i<FRAME_SIZE;i++)pcm[i]=fbuf[i]*32767.0f;

    unsigned char enc[MAX_PACKET_SIZE];
    enc[0]=PACKET_AUDIO;
    int nb=opus_encode(opusEnc,pcm,FRAME_SIZE,enc+1,MAX_PACKET_SIZE-1);
    if(nb>0)broadcastAudioToPeers(enc,nb+1);
}

//--------------------------------------------------------------------------------------
// broadcast
//--------------------------------------------------------------------------------------
static void broadcastAudioToPeers(const unsigned char *data,int sz){
    for(int i=0;i<MAX_PEERS;i++){
        Peer*p=&netState.peers[i];
        if(p->state==PEER_CONNECTED&&p->ssl){
            int w=SSL_write(p->ssl,data,sz);
            if(w>0)p->bytesSent+=w;
        }
    }
}

//--------------------------------------------------------------------------------------
// decay & timeout
//--------------------------------------------------------------------------------------
static void updateAudioLevels(void){
    for(int i=0;i<MAX_PEERS;i++){
        Peer*p=&netState.peers[i];
        if(p->state==PEER_CONNECTED){
            p->audioLevel*=0.9f;
            if(time(NULL)-p->lastActivity>60){
                fprintf(stderr,"[TIMEOUT] %s:%d\n",p->address,p->port);
                if(p->ssl)SSL_shutdown(p->ssl),SSL_free(p->ssl),p->ssl=NULL;
                if(p->socketFd>=0)close(p->socketFd),p->socketFd=-1;
                p->state=PEER_DISCONNECTED;
                netState.peerCount--;
            }
        }
    }
}

//--------------------------------------------------------------------------------------
// draw bars
//--------------------------------------------------------------------------------------
static void drawVisualization(void){
    printf("\033[2J\033[H");
    printf("=== Voice Chat ===\n\n");
    ma_uint32 avail=ma_rb_available_read(&audioCtx.captureRB);
    printf("Mic ring: %4u/%4u\n",avail/sizeof(float),SAMPLE_RATE);
    printf("\nPeers: %d\n",netState.peerCount);
    for(int i=0;i<MAX_PEERS;i++){
        Peer*p=&netState.peers[i];
        if(p->state==PEER_CONNECTED){
            printf("%-15s ",p->address);
            int bar=15,fill=p->audioLevel*bar*5;
            if(fill>bar)fill=bar;
            printf("[");
            for(int j=0;j<bar;j++)putchar(j<fill?'#':' ');
            printf("] ↑%.1f↓%.1f\n",p->bytesSent/1024.0f,p->bytesReceived/1024.0f);
        }
    }
    printf("\nCtrl+C to exit\n");
    fflush(stdout);
}

//--------------------------------------------------------------------------------------
// RMS
//--------------------------------------------------------------------------------------
static float calculateRMS(const float*b,int n){
    double s=0;
    for(int i=0;i<n;i++)s+=b[i]*b[i];
    return sqrt(s/n);
}

//--------------------------------------------------------------------------------------
// peerExists
//--------------------------------------------------------------------------------------
static bool isPeerConnected(const char*addr,int port){
    for(int i=0;i<MAX_PEERS;i++){
        Peer*p=&netState.peers[i];
        if((p->state==PEER_CONNECTED||p->state==PEER_CONNECTING)&&
           !strcmp(p->address,addr)&&p->port==port) return true;
    }
    return false;
}

//--------------------------------------------------------------------------------------
// add peer
//--------------------------------------------------------------------------------------
static void addPeer(const char*addr,int port){
    if(isPeerConnected(addr,port))return;
    for(int i=0;i<MAX_PEERS;i++){
        Peer*p=&netState.peers[i];
        if(p->state==PEER_DISCONNECTED){
            strcpy(p->address,addr);
            p->port=port;
            coroutine_go(connectToPeerCoroutine,p);
            return;
        }
    }
    fprintf(stderr,"[WARN] no slot for %s:%d\n",addr,port);
}

//--------------------------------------------------------------------------------------
// handle peer-list
//--------------------------------------------------------------------------------------
static void handlePeerList(Peer*p,const char*data,int len){
    char*cp=malloc(len+1);
    memcpy(cp,data,len);cp[len]=0;
    char*tok,*save;
    for(tok=strtok_r(cp,";",&save);tok;tok=strtok_r(NULL,";",&save)){
        char*c=strchr(tok,':');if(!c)continue;*c=0;
        int pr=atoi(c+1);
        if(!isPeerConnected(tok,pr))addPeer(tok,pr);
    }
    free(cp);
}

//--------------------------------------------------------------------------------------
// sendPeerList
//--------------------------------------------------------------------------------------
static void sendPeerList(Peer*p){
    char buf[MAX_BUFFER_SIZE];int off=0;
    buf[off++]=PACKET_PEER_LIST;
    for(int i=0;i<MAX_PEERS;i++){
        Peer*q=&netState.peers[i];
        if(q->state==PEER_CONNECTED){
            off+=snprintf(buf+off,MAX_BUFFER_SIZE-off,
                          "%s:%d;",q->address,q->port);
        }
    }
    off+=snprintf(buf+off,MAX_BUFFER_SIZE-off,
                  "%s:%d;",myAddress,myPort);
    SSL_write(p->ssl,buf,off);
}

//--------------------------------------------------------------------------------------
// announceSelf
//--------------------------------------------------------------------------------------
static void announceSelf(void){
    char msg[128];
    snprintf(msg,sizeof msg,"VOICECHAT:%s:%d",myAddress,myPort);
    struct sockaddr_in ba={0};
    ba.sin_family=AF_INET;
    ba.sin_port=htons(PEER_DISCOVERY_PORT);
    ba.sin_addr.s_addr=INADDR_BROADCAST;
    sendto(netState.discoverySocket,msg,strlen(msg),0,
           (struct sockaddr*)&ba,sizeof ba);
    fprintf(stderr,"[DISC] self\n");
}
