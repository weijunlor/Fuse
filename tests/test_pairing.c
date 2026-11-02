/* Security boundary tests include the transport in a dedicated test binary.
   Adversarial peers exist only here, never in the production executable. */
#define FUSE_PAIRING_TOOL 1
#define main pairing_program_main
#include "../common/pairing_tool.c"
#undef main
#include <assert.h>

static void *race_consume(void *arg)
{
    bool *result = arg; *result = consume_code(17); return NULL;
}

static int adversarial_host(const char *mode, const char *address, int port)
{
    struct sockaddr_in local = {.sin_family=AF_INET, .sin_port=htons((uint16_t)port)};
    assert(inet_pton(AF_INET,address,&local.sin_addr)==1);
    int fd = socket(AF_INET,SOCK_STREAM,0), one=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    assert(bind(fd,(struct sockaddr*)&local,sizeof local)==0 && listen(fd,1)==0);
    p_server_ctx = tls_context(true); assert(p_server_ctx); new_code();
    int peer = accept(fd,NULL,NULL); assert(peer>=0); fcntl(peer,F_SETFL,O_NONBLOCK);
    Connection connection={.int_fd=peer};
    SSL *ssl=SSL_new(p_server_ctx); SSL_set_fd(ssl,peer); SSL_set_app_data(ssl,&connection);
    assert(handshake(ssl,true));
    unsigned char type,*request=NULL; size_t size=0;
    assert(receive_frame(ssl,&type,&request,&size,now_ns()+10000000000ull));
    if (!strcmp(mode,"invalid-bundle")) {
        assert(send_frame(ssl,0,"invalid credential archive",26,now_ns()+10000000000ull));
    } else if (!strcmp(mode,"oversize")) {
        unsigned char header[5]={0}; uint32_t length=htonl(PAIR_MAX+1); memcpy(header+1,&length,4);
        tls_bytes(ssl,header,5,true,now_ns()+10000000000ull);
    } else {
        while (!b_stop) { struct timespec pause={.tv_nsec=10000000}; nanosleep(&pause,NULL); }
    }
    free(request); SSL_free(ssl); SSL_CTX_free(p_server_ctx); close(peer); close(fd); return 0;
}

static int bad_proof(const char *address,int port,bool replay)
{
    char code[64]; assert(fgets(code,sizeof code,stdin));
    unsigned char encoded[45],decoded[33];
    memcpy(encoded,code+5,43); encoded[43]='='; encoded[44]=0;
    for(int i=0;i<43;i++){if(encoded[i]=='-')encoded[i]='+'; if(encoded[i]=='_')encoded[i]='/';}
    assert(EVP_DecodeBlock(decoded,encoded,44)==33);
    Connection connection={0}; memcpy(connection.arr_psk,decoded,32);
    int fd=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in remote={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    assert(inet_pton(AF_INET,address,&remote.sin_addr)==1 && connect(fd,(struct sockaddr*)&remote,sizeof remote)==0);
    fcntl(fd,F_SETFL,O_NONBLOCK);
    SSL_CTX *ctx=tls_context(false); SSL *ssl=SSL_new(ctx);
    SSL_set_fd(ssl,fd); SSL_set_app_data(ssl,&connection); assert(handshake(ssl,false));
    unsigned char proof[96]={0};
    EVP_PKEY *key=EVP_PKEY_Q_keygen(NULL,NULL,"ED25519"); size_t size=32;
    assert(key && EVP_PKEY_get_raw_public_key(key,proof,&size)==1);
    if(replay) {
        assert(device_proof(ssl,key,proof,false));
        SSL_free(ssl); close(fd);
        fd=socket(AF_INET,SOCK_STREAM,0);
        assert(connect(fd,(struct sockaddr*)&remote,sizeof remote)==0); fcntl(fd,F_SETFL,O_NONBLOCK);
        ssl=SSL_new(ctx); SSL_set_fd(ssl,fd); SSL_set_app_data(ssl,&connection);
        assert(handshake(ssl,false));
    }
    // A valid key with a forged signature must never reserve an identity.
    assert(send_frame(ssl,1,proof,sizeof proof,now_ns()+10000000000ull));
    unsigned char type,*reply=NULL; size_t length=0;
    bool received=receive_frame(ssl,&type,&reply,&length,now_ns()+3000000000ull);
    free(reply); EVP_PKEY_free(key); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    return received ? 1 : 0;
}

int main(int argc,char **argv)
{
    signal(SIGPIPE,SIG_IGN); signal(SIGTERM,stop_signal);
    if(argc==4) {
        if(!strcmp(argv[1],"bad-proof") || !strcmp(argv[1],"replay-proof"))
            return bad_proof(argv[2],atoi(argv[3]),!strcmp(argv[1],"replay-proof"));
        return adversarial_host(argv[1],argv[2],atoi(argv[3]));
    }
    SSL_CTX *ctx=tls_context(false); assert(ctx);
    assert(SSL_CTX_get_min_proto_version(ctx)==TLS1_3_VERSION);
    assert(SSL_CTX_get_max_proto_version(ctx)==TLS1_3_VERSION);
    assert(!(SSL_CTX_get_options(ctx)&SSL_OP_ALLOW_NO_DHE_KEX));
    assert(SSL_CTX_get_max_early_data(ctx)==0 && SSL_CTX_get_num_tickets(ctx)==0);
    SSL_CTX_free(ctx);
    b_code_active=true; u_generation=17; u_expiry=now_ns()+1000000000ull;
    bool a=false,b=false; pthread_t first,second;
    assert(!pthread_create(&first,NULL,race_consume,&a));
    assert(!pthread_create(&second,NULL,race_consume,&b));
    pthread_join(first,NULL); pthread_join(second,NULL); assert(a!=b);
    b_code_active=true; u_expiry=now_ns()-1; assert(!consume_code(17));
    SSL_CTX *server=tls_context(true); SSL *ssl=SSL_new(server); Connection connection={0};
    SSL_set_app_data(ssl,&connection); SSL_SESSION *session=NULL;
    assert(server_psk(ssl,psk_identity,sizeof psk_identity-1,&session)==1 && session==NULL);
    memset(arr_code,123,32); cancel_code();
    for(int i=0;i<32;i++)assert(arr_code[i]==0);
    SSL_free(ssl); SSL_CTX_free(server);
    puts("PASS TLS policy, atomic one-time consumption, expired handshake rejection and secret erasure");
    return 0;
}
