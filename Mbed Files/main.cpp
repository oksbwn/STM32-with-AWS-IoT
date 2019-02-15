#include "mbed.h"
#include "NTPClient.h"
#include "MQTTNetwork.h"
#include "MQTTmbed.h"
#include "MQTTClient.h"
#include "MQTT_server_setting.h"
#include "mbed-trace/mbed_trace.h"
#include "mbed_events.h"
#include "mbedtls/error.h"

#define LED_ON  MBED_CONF_APP_LED_ON
#define LED_OFF MBED_CONF_APP_LED_OFF

#include "HTS221Sensor.h"
#include "LPS22HBSensor.h"

//Intialization of sensors
static DevI2C devI2c(PB_11,PB_10);
static LPS22HBSensor press_temp(&devI2c);
static HTS221Sensor hum_temp(&devI2c);

static volatile bool isPublish = false;
static volatile bool loginDataPub=false;
;

//Serial port for Debug
Serial pc(USBTX, USBRX); // tx, rx

/* Flag to be set when received a message from the server. */
static volatile bool isMessageArrived = false;
/* Buffer size for a receiving message. */
const int MESSAGE_BUFFER_SIZE = 256;
/* Buffer for a receiving message. */
char messageBuffer[MESSAGE_BUFFER_SIZE];

// An event queue is a very useful structure to debounce information between contexts (e.g. ISR and normal threads)
// This is great because things such as network operations are illegal in ISR, so updating a resource in a button's fall() function is not allowed
EventQueue eventQueue;
Thread thread1;
DigitalOut led(LED1);
DigitalOut relayOne(D5);

Ticker wetherDataTimer;
/*
 * Callback function called when a message arrived from server.
 */
void messageArrived(MQTT::MessageData& md)
{
    // Copy payload to the buffer.
    MQTT::Message &message = md.message;
    if(message.payloadlen >= MESSAGE_BUFFER_SIZE) {
        // TODO: handling error
    } else {
        memcpy(messageBuffer, message.payload, message.payloadlen);
    }
    messageBuffer[message.payloadlen] = '\0';

    isMessageArrived = true;
}

/*
 * Callback function called when the button1 is clicked.
 */
void sendWeatherData() {
    isPublish = true;
}
void sendLoginData(){
    loginDataPub=true;
}

int main(int argc, char* argv[])
{
    mbed_trace_init();
    
    pc.baud(115200);
    
    float temperature, humidity, pressure;
    const float version = 0.9;
    bool isSubscribed = false;
    
    wetherDataTimer.attach(&sendWeatherData, 120.0);
    
    NetworkInterface* network = NULL;
    MQTTNetwork* mqttNetwork = NULL;
    MQTT::Client<MQTTNetwork, Countdown>* mqttClient = NULL;

    DigitalOut led(MBED_CONF_APP_LED_PIN, LED_ON);

    printf("HelloMQTT: version is %.2f\r\n", version);
    printf("\r\n");

    printf("Opening network interface...\r\n");
    {
        network = NetworkInterface::get_default_instance();
        if (!network) {
            printf("Error! No network inteface found.\n");
            return -1;
        }

        printf("Connecting to network\n");
        nsapi_size_or_error_t ret = network->connect();
        if (ret) {
            printf("Unable to connect! returned %d\n", ret);
            return -1;
        }
    }
    printf("Network interface opened successfully.\r\n");
    printf("\r\n");

    // sync the real time clock (RTC)
    NTPClient ntp(network);
    ntp.set_server("time.google.com", 123);
    time_t now = ntp.get_timestamp();
    set_time(now);
    printf("Time is now %s", ctime(&now));
    
 //Sensors Initiailization
    press_temp.init(NULL);
    hum_temp.init(NULL);
    press_temp.enable();
    hum_temp.enable();
    
    


    printf("Connecting to host %s:%d ...\r\n", MQTT_SERVER_HOST_NAME, MQTT_SERVER_PORT);
    {
        mqttNetwork = new MQTTNetwork(network);
        int rc = mqttNetwork->connect(MQTT_SERVER_HOST_NAME, MQTT_SERVER_PORT, SSL_CA_PEM,
                SSL_CLIENT_CERT_PEM, SSL_CLIENT_PRIVATE_KEY_PEM);
        if (rc != MQTT::SUCCESS){
            const int MAX_TLS_ERROR_CODE = -0x1000;
            // Network error
            if((MAX_TLS_ERROR_CODE < rc) && (rc < 0)) {
                // TODO: implement converting an error code into message.
                printf("ERROR from MQTTNetwork connect is %d.", rc);
            }
            // TLS error - mbedTLS error codes starts from -0x1000 to -0x8000.
            if(rc <= MAX_TLS_ERROR_CODE) {
                const int buf_size = 256;
                char *buf = new char[buf_size];
                mbedtls_strerror(rc, buf, buf_size);
                printf("TLS ERROR (%d) : %s\r\n", rc, buf);
            }
            return -1;
        }
    }
    printf("Connection established.\r\n");
    printf("\r\n");

    printf("MQTT client is trying to connect the server ...\r\n");
    
        MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
        data.MQTTVersion = 3;
        data.clientID.cstring = (char *)MQTT_CLIENT_ID;
        data.username.cstring = (char *)MQTT_USERNAME;
        data.password.cstring = (char *)MQTT_PASSWORD;

        mqttClient = new MQTT::Client<MQTTNetwork, Countdown>(*mqttNetwork);
        int rc = mqttClient->connect(data);
        if (rc != MQTT::SUCCESS) {
            printf("ERROR: rc from MQTT connect is %d\r\n", rc);
            return -1;
        }
    
    printf("Client connected.\r\n");
    printf("\r\n");


    printf("Client is trying to subscribe a topic \"%s\".\r\n", MQTT_TOPIC_SUB);
    {
        int rc = mqttClient->subscribe(MQTT_TOPIC_SUB, MQTT::QOS0, messageArrived);
        if (rc != MQTT::SUCCESS) {
            printf("ERROR: rc from MQTT subscribe is %d\r\n", rc);
            return -1;
        }
        isSubscribed = true;
    }
    printf("Client has subscribed a topic \"%s\".\r\n", MQTT_TOPIC_SUB);
    printf("\r\n");

    // Enable button 1
    InterruptIn btn1 = InterruptIn(MBED_CONF_APP_USER_BUTTON);
    btn1.rise(sendLoginData);
    
    printf("To send a packet, push the button 1 on your board.\r\n\r\n");

    // Turn off the LED to let users know connection process done.
    led = LED_OFF;

    while(1) {
        /* Check connection */
        char jsonBuffer[120];
        hum_temp.get_temperature(&temperature);
        hum_temp.get_humidity(&humidity);
        press_temp.get_pressure(&pressure);
        printf("[T] %.2f C, [H]   %.2f%%, [P] %.2f mbar\r\n", temperature, humidity,pressure);
    
        if(!mqttClient->isConnected()){
            break;
        }
        /* Pass control to other thread. */
        if(mqttClient->yield() != MQTT::SUCCESS) {
            break;
        }
        /* Received a control message. */
        if(isMessageArrived) {
            isMessageArrived = false;
            // Just print it out here.
            printf("\r\nMessage arrived:\r\n%s\r\n\r\n", messageBuffer);
//            found=str.find("COORD:");
//        if (found!=string::npos) {
            if(strstr(messageBuffer,"ON")){
                 led=1;
                 relayOne=1;
            }
            else if(strstr(messageBuffer,"OFF"))
            {
             led=0;
             relayOne=0;
                }
        }
        /* Publish data */
        if(loginDataPub){
            loginDataPub=false;
            
            static unsigned short id = 0;

            // When sending a message, LED lights blue.
            led = LED_ON;

            MQTT::Message message;
            message.retained = false;
            message.dup = false;

//            char *buf = new char[jsonBuffer];
            message.payload = (void*)jsonBuffer;

            message.qos = MQTT::QOS0;
            message.id = id++;
            int ret =sprintf(jsonBuffer,"{\"DEVID\":\"1\",\"EID\":\"1\",\"TYPE\":\"IN\"}");
//            int ret = snprintf(buf, buf_size, "%d", count);
            if(ret < 0) {
                printf("ERROR: snprintf() returns %d.", ret);
                continue;
            }
            message.payloadlen = ret;
            // Publish a message.
            printf("Publishing message.\r\n");
            int rc = mqttClient->publish(PUB_CARD, message);
            if(rc != MQTT::SUCCESS) {
                printf("ERROR: rc from MQTT publish is %d\r\n", rc);
            }
            printf("Message published.\r\n");
            delete[] jsonBuffer;    

            led = LED_OFF;
            
            }
        if(isPublish) {
            isPublish = false;
            static unsigned short id = 0;

  

            // When sending a message, LED lights blue.
            led = LED_ON;

            MQTT::Message message;
            message.retained = false;
            message.dup = false;

//            char *buf = new char[jsonBuffer];
            message.payload = (void*)jsonBuffer;

            message.qos = MQTT::QOS0;
            message.id = id++;
            int ret =sprintf(jsonBuffer,"{\"DEVID\":\"1\",\"HUM\":\"%.2f%%\",\"PRES\":\"%.2f\",\"TEMP\":\"%.2f\"}", humidity,pressure,temperature);
//            int ret = snprintf(buf, buf_size, "%d", count);
            if(ret < 0) {
                printf("ERROR: snprintf() returns %d.", ret);
                continue;
            }
            message.payloadlen = ret;
            // Publish a message.
            printf("Publishing message.\r\n");
            int rc = mqttClient->publish(MQTT_TOPIC_PUB, message);
            if(rc != MQTT::SUCCESS) {
                printf("ERROR: rc from MQTT publish is %d\r\n", rc);
            }
            printf("Message published.\r\n");
            delete[] jsonBuffer;    

            led = LED_OFF;
        }
    }

    printf("The client has disconnected.\r\n");

    if(mqttClient) {
        if(isSubscribed) {
            mqttClient->unsubscribe(MQTT_TOPIC_SUB);
            mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
        }
        if(mqttClient->isConnected()) 
            mqttClient->disconnect();
        delete mqttClient;
    }
    if(mqttNetwork) {
        mqttNetwork->disconnect();
        delete mqttNetwork;
    }
    if(network) {
        network->disconnect();
        // network is not created by new.
    }
}
