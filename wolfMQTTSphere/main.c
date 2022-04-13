
#include <wolfmqtt/mqtt_client.h>
#include <wolfssl/openssl/ssl.h>
#include <applibs/log.h>
#include <applibs/networking.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <netdb.h>
#include <applibs/storage.h>
#include <errno.h>

#include <applibs/application.h>
#include <tlsutils/deviceauth.h>


#define MQTT_QOS             MQTT_QOS_0
#define MQTT_USE_TLS         1
#define MQTT_PORT            1883
#define MQTT_TLS_PORT        8883
#define MQTT_KEEP_ALIVE_SEC  60
#define MQTT_CMD_TIMEOUT_MS  30000
#define MQTT_CON_TIMEOUT_MS  3000
#define MQTT_CLIENT_ID       "enter id here"
#define MQTT_USERNAME        "enter username here"
#define MQTT_PASSWORD        "enter password here"
#define MQTT_MAX_PACKET_SZ   1024
#define INVALID_SOCKET_FD    -1
#define MQTT_BROKER_IP_ADDRESS "000.000.0.00" /*enter your mqtt broker IP here
                                                and in app_manifest.json allowed
                                                connections section*/

static byte mqtt_send_buf[MQTT_MAX_PACKET_SZ];
static byte mqtt_read_buf[MQTT_MAX_PACKET_SZ];

static const char *pub_topic = "test_topic";

struct mqtt_instance_t {
    MqttClient client;
    MqttNet network;
    int socket_fd;
    char *ip;
    bool connected;
    word16 cur_packet_id;
};

/*Private Methods-------------------------------------------------------------*/

static void setup_timeout(struct timeval* tv, int timeout_ms)
{
    tv->tv_sec = timeout_ms / 1000;
    tv->tv_usec = (timeout_ms % 1000) * 1000;

    /* Make sure there is a minimum value specified */
    if (tv->tv_sec < 0 || (tv->tv_sec == 0 && tv->tv_usec <= 0)) {
        tv->tv_sec = 0;
        tv->tv_usec = 100;
    }
}

static int socket_get_error(int sockFd)
{
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(sockFd, SOL_SOCKET, SO_ERROR, &so_error, &len);
    Log_Debug("errno %d\n", errno);
    return so_error;
}

static int mqtt_net_connect(void *context, const char* host, word16 port, int timeout_ms)
{
    int rc;
    int sock_fd, *p_sock_fd = (int*)context;
    struct sockaddr_in addr;
    struct addrinfo *result = NULL;
    struct addrinfo hints;
    struct timeval timeout;

    if (p_sock_fd == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }

    /* get address */
    XMEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    XMEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc >= 0 && result != NULL) {
        struct addrinfo* res = result;

        /* prefer ip4 addresses */
        while (res) {
            if (res->ai_family == AF_INET) {
                result = res;
                break;
            }
            res = res->ai_next;
        }
        if (result->ai_family == AF_INET) {
            addr.sin_port = htons(port);
            addr.sin_family = AF_INET;
            addr.sin_addr =
                ((struct sockaddr_in*)(result->ai_addr))->sin_addr;
        }
        else {
            rc = -1;
        }
        freeaddrinfo(result);
    }
    if (rc < 0) {
        return MQTT_CODE_ERROR_NETWORK;
    }

    sock_fd = socket(addr.sin_family, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        return MQTT_CODE_ERROR_NETWORK;
    }

    setup_timeout(&timeout, timeout_ms);

    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0) {
        Log_Debug("could not set mqtt socket receive timeout\n");
        close(sock_fd);
        return MQTT_CODE_ERROR_NETWORK;
    }

    if (setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) < 0) {
        Log_Debug("could not set mqtt socket send timeout\n");
        close(sock_fd);
        return MQTT_CODE_ERROR_NETWORK;
    }

    /* Start connect */
    rc = connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0) {
        Log_Debug("NetConnect: Error %d (Sock Err %d)\n", rc, socket_get_error(sock_fd));
        close(sock_fd);
        return MQTT_CODE_ERROR_NETWORK;
    }

    /* save socket number to context */
    *p_sock_fd = sock_fd;

    return MQTT_CODE_SUCCESS;
}

static int mqtt_net_read(void *context, byte* buf, int buf_len, int timeout_ms)
{
    int rc;
    int *sock_fd = (int*)context;
    int bytes = 0;
    struct timeval tv;

    if (sock_fd == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }

    /* Setup timeout */
    setup_timeout(&tv, timeout_ms);
    if (setsockopt(*sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv)) < 0)
        return MQTT_CODE_ERROR_NETWORK;

    /* Loop until buf_len has been read, error or timeout */
    while (bytes < buf_len) {
        rc = (int)recv(*sock_fd, &buf[bytes], buf_len - bytes, 0);
        if (rc < 0) {
            rc = socket_get_error(*sock_fd);
            if (rc == 0)
                break; /* timeout */
            Log_Debug("NetRead: Error %d\n", rc);
            return MQTT_CODE_ERROR_NETWORK;
        }
        bytes += rc; /* Data */
    }

    if (bytes == 0) return MQTT_CODE_ERROR_TIMEOUT;


    return bytes;
}

static int mqtt_net_write(void *context, const byte* buf, int buf_len, int timeout_ms)
{
    int rc;
    int *pSockFd = (int*)context;
    struct timeval tv;

    if (pSockFd == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }

    /* Setup timeout */
    setup_timeout(&tv, timeout_ms);
    setsockopt(*pSockFd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv));

    rc = (int)send(*pSockFd, buf, buf_len, 0);
    if (rc < 0) {
        Log_Debug("NetWrite: Error %d (Sock Err %d)\n",
            rc, socket_get_error(*pSockFd));
        return MQTT_CODE_ERROR_NETWORK;
    }

    return rc;
}

static int mqtt_net_disconnect(void *context)
{
    int *pSockFd = (int*)context;

    if (pSockFd == NULL) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }

    close(*pSockFd);
    *pSockFd = INVALID_SOCKET_FD;

    Log_Debug("mqtt net disconnected\n");

    return MQTT_CODE_SUCCESS;
}

static word16 mqtt_get_packetid(word16 *last_packet_id)
{
    /* Check rollover */
    if (*last_packet_id >= MAX_PACKET_ID) {
        *last_packet_id = 0;
    }
    return ++(*last_packet_id);
}

int mqtt_tls_cb(MqttClient* client) {
    int ret;
    char *cert_path = "test_cert/cert.pem"; /*paste the broker cert in this file*/

    Log_Debug("setting up tls\n");

    WOLFSSL_CTX *wolf_ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (wolf_ssl_ctx  == NULL) {
        Log_Debug("wolf_ssl_ctx  invalid");
        wolfSSL_CTX_free(wolf_ssl_ctx );
        return -1;
    }

    /* get the certificate which is used to validate the broker */
    char *cert_path_abs = Storage_GetAbsolutePathInImagePackage(cert_path);
    if (cert_path_abs == NULL) {
        Log_Debug("certpath invalid");
        wolfSSL_CTX_free(wolf_ssl_ctx );
        return -1;
    }

    ret = wolfSSL_CTX_load_verify_locations(wolf_ssl_ctx , cert_path_abs, NULL);
    if (ret != WOLFSSL_SUCCESS) {
        Log_Debug("ERROR: wolfSSL_CTX_load_verify_locations %d\n", ret);
        wolfSSL_CTX_free(wolf_ssl_ctx);
        return -1;
    }

    /*  uncomment following chunk to use DAA cert if broker requires client verification/mutual auth.
    */
    /*
    bool cert_ready = false;
    Log_Debug("checking device DAA authentication...\n");
    while (!cert_ready) {


        if (Application_IsDeviceAuthReady(&cert_ready) != 0) {
            Log_Debug("ERROR: Application_IsDeviceAuthReady: %d (%s)\n", errno, strerror(errno));
        }
        if (!cert_ready)
            Log_Debug("device auth not ready.\n");
        sleep(1);
    }
    Log_Debug("Device Authenticated!\n");
    Log_Debug("DAA certpath: %s\n", DeviceAuth_GetCertificatePath());
    */

    /*   uncomment folowing chunk to parse subject and issuer from the DAA cert
    */
    /*
    char buf[1000] = {0};
    WOLFSSL_X509* my_cert = wolfSSL_X509_load_certificate_file(DeviceAuth_GetCertificatePath(), WOLFSSL_FILETYPE_PEM);
    Log_Debug("subject: %s\n", wolfSSL_X509_NAME_oneline(wolfSSL_X509_get_subject_name(my_cert), buf, 1000));
    memset(buf, 0, sizeof(buf));
    Log_Debug("issuer: %s\n", wolfSSL_X509_NAME_oneline(wolfSSL_X509_get_issuer_name(my_cert), buf, 1000));
    */

    /*  uncomment following chunk to use DAA cert if broker requires client verification/mutual auth.
        comment line 298 or 299 depending on if you would like to use cert of chain-cert.
    */
    /*
    WOLFSSL_ABI WOLFSSL_API WOLFSSL_X509* wolfSSL_X509_load_certificate_file(const char* fname, int format);
    ret = wolfSSL_CTX_use_certificate_file(wolfSslCtx, DeviceAuth_GetCertificatePath(), WOLFSSL_FILETYPE_PEM);
    ret = wolfSSL_CTX_use_certificate_chain_file(wolfSslCtx, DeviceAuth_GetCertificatePath());
    if (ret != WOLFSSL_SUCCESS) {
        Log_Debug("ERROR: wolfSSL_CTX_use_certificate_chain_file %d\n", ret);
        wolfSSL_CTX_free(wolfSslCtx);
        return -1;
    }
    Log_Debug("loaded DAA certificate to wolfssl\n");
    */

    WOLFSSL *ssl = wolfSSL_new(wolf_ssl_ctx );
    if (ssl == NULL) {
        Log_Debug("ssl invalid\n");
        wolfSSL_CTX_free(wolf_ssl_ctx );
        return -1;
    }

    client->tls.ssl = ssl;
    client->tls.ctx = wolf_ssl_ctx ;
    Log_Debug("TLS setup successful \n");

    return WOLFSSL_SUCCESS;
}

static int message_mqtt_instance(char *message, size_t message_len, struct mqtt_instance_t *instance)
{
    MqttObject obj;
    int rc = 0;

    if (!instance->connected)
        return -1;

    /* Publish */
    XMEMSET(&obj, 0, sizeof(MqttObject));
    obj.publish.qos = MQTT_QOS;
    obj.publish.topic_name = pub_topic;
    obj.publish.packet_id = mqtt_get_packetid(&instance->cur_packet_id);
    obj.publish.buffer = (byte*)message;
    obj.publish.total_len = message_len;

    rc = MqttClient_Publish(&instance->client, &obj.publish);
    if (rc != MQTT_CODE_SUCCESS) {
        Log_Debug("could not publish to mqtt broker\n");
        return -1;
    }

    Log_Debug("mqtt publish success, message: %s\n", message);

    return 0;
}

static int init_mqtt_instance(struct mqtt_instance_t *instance, char* ip)
{
    int rc = 0;
    MqttObject obj;
    MqttNet *network = &instance->network;
    MqttClient *client = &instance->client;

    if (instance->connected)
        return 0;

    instance->ip = ip;
    instance->cur_packet_id = 0;

    /* Initialize MQTT client */
    XMEMSET(network, 0, sizeof(MqttNet));
    network->connect = mqtt_net_connect;
    network->write = mqtt_net_write;
    network->read = mqtt_net_read;
    network->disconnect = mqtt_net_disconnect;
    network->context = &instance->socket_fd;
    rc = MqttClient_Init(client, network, NULL, mqtt_send_buf, sizeof(mqtt_send_buf), mqtt_read_buf, sizeof(mqtt_read_buf), MQTT_CON_TIMEOUT_MS);
    if (rc != MQTT_CODE_SUCCESS) {
        Log_Debug("could not initialize mqtt client object\n");
        return -1;
    }
    Log_Debug("MQTT Init Success\n");

    /* Connect to broker */
    rc = MqttClient_NetConnect(client, ip, MQTT_TLS_PORT, MQTT_CON_TIMEOUT_MS, MQTT_USE_TLS, mqtt_tls_cb);
    if (rc != MQTT_CODE_SUCCESS) {
        Log_Debug("could not connect to mqtt broker\n");
        return -1;
    }
    Log_Debug("MQTT Network Connect Success: Host %s, Port %d, UseTLS %d\n", instance->ip, MQTT_TLS_PORT, MQTT_USE_TLS);

    /* Send Connect and wait for Ack */
    XMEMSET(&obj, 0, sizeof(obj));
    obj.connect.keep_alive_sec = MQTT_KEEP_ALIVE_SEC;
    obj.connect.client_id = MQTT_CLIENT_ID;
    obj.connect.username = MQTT_USERNAME;
    obj.connect.password = MQTT_PASSWORD;
    rc = MqttClient_Connect(client, &obj.connect);
    if (rc != MQTT_CODE_SUCCESS) {
        Log_Debug("mqtt client could not connect\n");
        return -1;
    }
    instance->connected = true;

    return 0;
}

static int deinit_mqtt_instance(struct mqtt_instance_t *instance)
{
    Log_Debug("disconnecting\n");
    MqttClient_Disconnect(&instance->client);
    instance->connected = false;
    instance->cur_packet_id = 0;
    MqttClient_NetDisconnect(&instance->client);
    memset(instance, 0, sizeof(struct mqtt_instance_t));
    instance->socket_fd = -1;

    return 0;
}


int main(void)
{
    struct mqtt_instance_t test;

    while(true) {
        sleep(10);
        bool network_ready = false;
        if (Networking_IsNetworkingReady(&network_ready) < 0 || !network_ready) {
            Log_Debug("not connected to network\n");
            continue;
        }

        if (init_mqtt_instance(&test, MQTT_BROKER_IP_ADDRESS) == 0 && test.connected) {
            char msg[] = "Hello World -sphere";
            message_mqtt_instance(msg, strlen(msg), &test);
        }

        deinit_mqtt_instance(&test);
    }

    return 0;
}
