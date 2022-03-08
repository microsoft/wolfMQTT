
#include <wolfmqtt/mqtt_client.h>
#include <applibs/log.h>
#include <applibs/networking.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <netdb.h>


#define MQTT_QOS             MQTT_QOS_0
#define MQTT_USE_TLS     0
#define MQTT_PORT        1883
#define MQTT_KEEP_ALIVE_SEC  60
#define MQTT_CMD_TIMEOUT_MS  30000
#define MQTT_CON_TIMEOUT_MS  3000
#define MQTT_CLIENT_ID       "WolfMQTTSphere"
#define MQTT_MAX_PACKET_SZ   1024
#define INVALID_SOCKET_FD    -1
#define MQTT_BROKER_IP_ADDRESS "broker IP here" /*enter your mqtt broker IP here
                                                and in app_manifest.json allowed
                                                connections section*/

static byte mqtt_send_buf[MQTT_MAX_PACKET_SZ];
static byte mqtt_read_buf[MQTT_MAX_PACKET_SZ];

static const char *pub_topic = "PUB-TOPIC";

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
        Log_Debug("NetConnect: Error %d (Sock Err %d)\n", rc, socket_get_error(*p_sock_fd));
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
    if (setsockopt(*pSockFd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv)) < 0)
        return MQTT_CODE_ERROR_NETWORK;

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

static int mqtt_tls_cb(MqttClient* client)
{
    (void)client;
    return 0;
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

static int init_mqtt_instance(struct mqtt_instance_t *instance, const char* ip)
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
    rc = MqttClient_NetConnect(client, ip, MQTT_PORT, MQTT_CON_TIMEOUT_MS, MQTT_USE_TLS, mqtt_tls_cb);
    if (rc != MQTT_CODE_SUCCESS) {
        Log_Debug("could not connect to mqtt broker\n");
        return -1;
    }
    Log_Debug("MQTT Network Connect Success: Host %s, Port %d, UseTLS %d\n", instance->ip, MQTT_PORT, MQTT_USE_TLS);

    /* Send Connect and wait for Ack */
    XMEMSET(&obj, 0, sizeof(obj));
    obj.connect.keep_alive_sec = MQTT_KEEP_ALIVE_SEC;
    obj.connect.client_id = MQTT_CLIENT_ID;
    obj.connect.username = NULL;
    obj.connect.password = NULL;
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
    instance->connected = false;
    instance->cur_packet_id = 0;
    memset(&instance->ip, 0, sizeof(instance->ip));
    mqtt_net_disconnect(&instance->socket_fd);
    memset(&instance->client, 0, sizeof(instance->client));
    memset(&instance->network, 0, sizeof(instance->network));
    instance->socket_fd = -1;
    return 0;
}


int main(void)
{
    struct mqtt_instance_t test;

    while(true) {
        sleep(5);

        bool network_ready = false;
        if (Networking_IsNetworkingReady(&network_ready) < 0 || !network_ready) {
            Log_Debug("not connected to network\n");
            continue;
        }

        if (init_mqtt_instance(&test, MQTT_BROKER_IP_ADDRESS) < 0) {
            continue;
        }

        if (test.connected) {
            char msg[] = "Hello World -sphere";
            message_mqtt_instance(msg, strlen(msg), &test);
        }

        deinit_mqtt_instance(&test);
        sleep(15);
    }

    return 0;
}
