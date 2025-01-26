#include "DHTesp.h"
#include <ESP32Servo.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>  // Librería para cliente seguro
#include <PubSubClient.h>
#include <cmath>

// Para el formato Protobuf
#include "sensor_data.pb.h" 
#include "alert_data.pb.h" 
#include "pb_encode.h"
#include "pb_decode.h"

/* SENSORS VARIABLES */

#define LIGHT_SENSOR 32 //Light Sensor Pin
#define POT_SENSOR 34 // Potentiometer Sensor Pin. It will act as a PH sensor
#define TMP_HUM_SENSOR 15 // DHT22 Sensor Pin
#define US_TRIG 4 // Ultrasonic Trig Pin
#define US_ECHO 16 // Ultrasonic Echo pin
#define SERVO_PIN 18 // Servo pin
#define LED_PIN 22 // Led pin. When the led is on it's supposed that the ventilation is on
#define MAX_TEMP 32
#define MAX_POT 4095
#define PH_MAX 7
#define PH_MIN 5
#define HUMIDITY_MAX 70
#define HUMIDITY_MIN 50
const float GAMMA = 0.7;
const float RL10 = 50;
DHTesp dht_sensor;
Servo servo;

/* MQTT VARIABLES */

#define TOPIC_TEMP "IoTHydroponic/temperature"
#define TOPIC_LIGHT "IoTHydroponic/light"
#define TOPIC_HEIGHT "IoTHydroponic/height"
#define TOPIC_PH "IoTHydroponic/ph"
#define TOPIC_HUMIDITY "IoTHydroponic/humidity"
#define TOPIC_VENTILATION "IoTHydroponic/ventilation"
#define TOPIC_SERVO "IoTHydroponic/servo"
#define TOPIC_ALERT "IoTHydroponic/alert"
// #define BROKER_IP "127.0.0.1"
// #define BROKER_PORT 1883
#define MQTT_MAX_PACKET_SIZE 512
const char* mqtt_server = "";
const int mqtt_port = 8883;  // Puerto para conexión segura
const char* mqtt_user = "";
const char* mqtt_password = "";


/* WiFi RELATED VARIABLES */

const char* ssid = "Wokwi-GUEST";
const char* password = "";
WiFiClientSecure espClient;  // Cliente seguro para SSL
PubSubClient client(espClient);

/* QUEUES */

QueueHandle_t xQueueTemp;
QueueHandle_t xQueueHumidity;
QueueHandle_t xQueueHeight;
QueueHandle_t xQueuePh;
QueueHandle_t xQueueLight;

/* TASKS DEFINITION */

TaskHandle_t xTaskPublicData;
TaskHandle_t xTaskMoveServo;
TaskHandle_t xTaskLedVentilation;
TaskHandle_t xTaskReadData;
TaskHandle_t xTaskResetConnection;

void wifiConnect()
{
  Serial.print("Connecting to WiFi");
  WiFi.begin("Wokwi-GUEST", "", 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }

  Serial.println("Connected to the WiFi network");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}


void mqttConnect() {
  // Configurar la conexión al servidor MQTT
  client.setServer(mqtt_server, mqtt_port);

  // Configurar cliente seguro para evitar verificación de certificado
  espClient.setInsecure();

  while (!client.connected()) {
      Serial.print("Conectando a MQTT...");
      if (client.connect("ESP32_Client", mqtt_user, mqtt_password)) {
        Serial.println("Conectado!");
      } else {
        Serial.print("Error de conexión. Estado = ");
        Serial.println(client.state());
        delay(2000);
      }
  }
}

// Función para redondear a un número específico de decimales
float roundToDecimals(float value, int decimals) {
    float multiplier = pow(10.0, decimals);
    return round(value * multiplier) / multiplier;
}

uint8_t protobufBuffer[128]; // Buffer para almacenar los datos serializados

void sendSensorValue(const char* topic, const char* name, float value) {
    SensorValue sensorData = SensorValue_init_zero; // Inicializa la estructura

    // Rellena los datos
    strncpy(sensorData.name, name, sizeof(sensorData.name) - 1); // Nombre del sensor

     // Redondea el valor al número deseado de decimales
    value = roundToDecimals(value, 2); // Redondear a 2 decimales
    sensorData.value = value;

    // Imprime los valores para verificar
    Serial.println("Datos de sensorData:");
    Serial.println(sensorData.name);
    Serial.println(sensorData.value);

    // Serializa los datos a Protobuf
    pb_ostream_t stream = pb_ostream_from_buffer(protobufBuffer, sizeof(protobufBuffer));
    if (!pb_encode(&stream, SensorValue_fields, &sensorData)) {
        Serial.println("Error al serializar Protobuf");
        return;
    }

    // Publica los datos en MQTT
    if (client.publish(topic, protobufBuffer, stream.bytes_written)) {
        Serial.printf("Protobuf enviado: %s = %f\n", name, value);
    } else {
        Serial.printf("Error al enviar Protobuf: %s\n", name);
    }
}

void sendAlertValue(const char* topic, const char* name, const char* value) {
    AlertValue alertData = AlertValue_init_zero; // Inicializa la estructura

    // Rellena los datos
    strncpy(alertData.name, name, sizeof(alertData.name) - 1); // Nombre de la alerta
    strncpy(alertData.value, value, sizeof(alertData.value) - 1); // Mensaje de la alerta

    // Imprime los valores para verificar
    Serial.println("Datos de alertData:");
    Serial.println(alertData.name);
    Serial.println(alertData.value);

    // Serializa los datos a Protobuf
    pb_ostream_t stream = pb_ostream_from_buffer(protobufBuffer, sizeof(protobufBuffer));
    if (!pb_encode(&stream, AlertValue_fields, &alertData)) {
        Serial.println("Error al serializar Protobuf");
        return;
    }

    // Publica los datos en MQTT
    if (client.publish(topic, protobufBuffer, stream.bytes_written)) {
        Serial.printf("Protobuf enviado: %s = %f\n", name, value);
    } else {
        Serial.printf("Error al enviar Protobuf: %s\n", name);
    }
}


void vTaskPublicData( void * pvParameters ){
  char *pcTaskName;
  pcTaskName = ( char * ) pvParameters;
  portBASE_TYPE xStatus; 
  const portTickType xTicksToWait = 1000 / portTICK_RATE_MS;

  printf("Starting task PUBLIC DATA\n");

  for(;;) {
    
    float temp, height, light;
    int ph, humidity;
    char buffer[10];  // Asegúrate de que el tamaño sea suficiente

    xStatus = xQueueReceive(xQueueTemp, &temp, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %f\n", temp);
    }
    else{
      printf( "Could not receive from the queue TEMP.\r\n" );
      continue;
    }

    xStatus = xQueueReceive(xQueuePh, &ph, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %d\n", ph);
    }
    else{
      printf( "Could not receive from the queue PH.\r\n" );
      continue;
    }

    xStatus = xQueueReceive(xQueueHumidity, &humidity, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %d\n", humidity);
    }
    else{
      printf( "Could not receive from the queue HUMIDITY.\r\n" );
      continue;
    }

    xStatus = xQueueReceive(xQueueLight, &light, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %d\n", light);
    }
    else{
      printf( "Could not receive from the queue LIGHT.\r\n" );
      continue;
    }

    xStatus = xQueueReceive(xQueueHeight, &height, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %d\n", height);
    }
    else{
      printf( "Could not receive from the queue HEIGHT.\r\n" );
      continue;
    }

    sendSensorValue(TOPIC_TEMP, "temp", temp);
    sendSensorValue(TOPIC_LIGHT, "light", light);
    sendSensorValue(TOPIC_HEIGHT, "height", height);
    sendSensorValue(TOPIC_PH, "ph", (float)ph);
    sendSensorValue(TOPIC_HUMIDITY, "humidity",humidity);

    printf("Data Read: Temp = %.2f, Light = %.2f, Height = %.2f, pH = %d, Humidity = %d\r\n", temp, light, height, ph, humidity);
    vTaskDelay(2000/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL); // NULL indica que nos referimos a esta tarea
}

void vTaskMoveServo( void * pvParameters ){
  char *pcTaskName;
  pcTaskName = ( char * ) pvParameters;
  portBASE_TYPE xStatus; 
  const portTickType xTicksToWait = 1000 / portTICK_RATE_MS;

  printf("Starting task MOVE SERVO\n");

  for(;;) {
    
    float temp;
    int ph, humidity, pos = 40;

    // LECTURAS DE LOS DATOS

    xStatus = xQueuePeek(xQueueTemp, &temp, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %f\n", temp);
    }
    else{
      printf( "Could not receive from the queue TEMP.\r\n" );
      continue;
    }

    xStatus = xQueuePeek(xQueuePh, &ph, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %d\n", ph);
    }
    else{
      printf( "Could not receive from the queue PH.\r\n" );
      continue;
    }

    xStatus = xQueuePeek(xQueueHumidity, &humidity, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %d\n", humidity);
    }
    else{
      printf( "Could not receive from the queue HUMIDITY.\r\n" );
      continue;
    }

    // COMPROBACIONES PARA EL MOVIMIENTO DEL SERVO

    if (temp > MAX_TEMP) {
      pos += 35;
      sendAlertValue(TOPIC_ALERT, "Alerta temperatura", "La temperatura es demasiado alta");
    }
    if (ph < PH_MIN || ph > PH_MAX) {
      pos += 35;
      sendAlertValue(TOPIC_ALERT, "Alerta Ph", "El Ph está fuera de sus niveles normales");
    }
    if (humidity < HUMIDITY_MIN) {
      pos += 35;
      sendAlertValue(TOPIC_ALERT, "Alerta humedad", "La humedad relativa es muy baja");
    }
    if (humidity > HUMIDITY_MAX){
      pos -= 35;
      sendAlertValue(TOPIC_ALERT, "Alerta humedad", "La humedad relativa es muy alta");
    }
    sendSensorValue(TOPIC_SERVO, "servo", (float) pos);

    servo.write(pos); 

    printf("Moving Servo");

    vTaskDelay(1500/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL); // NULL indica que nos referimos a esta tarea
}

void vTaskLedVentilation( void * pvParameters ){
  char *pcTaskName;
  pcTaskName = ( char * ) pvParameters;
  portBASE_TYPE xStatus; 
  const portTickType xTicksToWait = 1000 / portTICK_RATE_MS;

  printf("Starting task LED VENTILATION\n");

  for(;;) {
    
    float temp;

    if( uxQueueMessagesWaiting( xQueueTemp ) == 0) continue;

    xStatus = xQueuePeek(xQueueTemp, &temp, xTicksToWait); // Leemos sin borrar el dato para que se pueda publicar

    if( xStatus == pdPASS ){
      printf( "Received = %f\n", temp);
    }
    else{
      printf( "Could not receive from the queue TEMP.\r\n" );
      continue;
    }

    if (temp > MAX_TEMP) {
      digitalWrite(LED_PIN, HIGH); 
      sendSensorValue(TOPIC_VENTILATION, "ventilation", 1.0);
    }
    else {
      digitalWrite(LED_PIN, LOW);
      sendSensorValue(TOPIC_VENTILATION, "ventilation", 0.0);
    }

    vTaskDelay(1500/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL); // NULL indica que nos referimos a esta tarea
}

void vTaskReadData( void * pvParameters ){
  char *pcTaskName;
  pcTaskName = ( char * ) pvParameters;
  portBASE_TYPE xStatus; 

  printf("Starting task READ DATA\n");

  for(;;) {
    
    float temp, height, light;
    int ph, humidity;

    TempAndHumidity data = dht_sensor.getTempAndHumidity();

    // TEMPERATURE READ

    temp = data.temperature;

    xStatus = xQueueSendToBack( xQueueTemp, &temp, 0 );
    if( xStatus != pdPASS ) printf("Could not send Temperature\n");

    // HEIGHT READ

    digitalWrite(US_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(US_TRIG, LOW);

    height = pulseIn(US_ECHO, HIGH) / 58;

    xStatus = xQueueSendToBack( xQueueHeight, &height, 0 );
    if( xStatus != pdPASS ) printf("Could not send Height\n");

    // LIGHT READ

    int analogValue = analogRead(LIGHT_SENSOR);
    float voltage = analogValue / 4096. * 5;
    float resistance = 2000 * voltage / (1 - voltage / 5);
    light = pow(RL10 * 1e3 * pow(10, GAMMA) / resistance, (1 / GAMMA));

    xStatus = xQueueSendToBack( xQueueLight, &light, 0 );
    if( xStatus != pdPASS ) printf("Could not send Light\n");

    // PH READ

    ph = analogRead(POT_SENSOR)*14 / MAX_POT; // Los valores del ph deben de estar en un rango entre 0 y 14

    xStatus = xQueueSendToBack( xQueuePh, &ph, 0 );
    if( xStatus != pdPASS ) printf("Could not send PH\n");

    // HUMIDITY READ

    humidity = data.humidity;

    xStatus = xQueueSendToBack( xQueueHumidity, &humidity, 0 );
    if( xStatus != pdPASS ) printf("Could not send Humidity\n");

    printf("Data Read: Temp = %.2f, Light = %.2f, Height = %.2f, pH = %d, Humidity = %d\r\n", temp, light, height, ph, humidity);

    vTaskDelay(1000/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL); // NULL indica que nos referimos a esta tarea
}

void vTaskResetConnection( void * pvParameters ){
  char *pcTaskName;
  pcTaskName = ( char * ) pvParameters;
  portBASE_TYPE xStatus; 

  printf("Starting task RESET CONECTION\n");

  for(;;) {

    if(!client.connected()){
      client.setServer(mqtt_server, mqtt_port);
      espClient.setInsecure();
      client.connect("ESP32_Client", mqtt_user, mqtt_password);
    } 

    vTaskDelay(1000/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL); // NULL indica que nos referimos a esta tarea
}

void app_main(){

  xQueueTemp = xQueueCreate( 3, sizeof( float ) );
  xQueueHumidity = xQueueCreate( 3, sizeof( int ) );
  xQueueHeight = xQueueCreate( 3, sizeof( float ) );
  xQueuePh = xQueueCreate( 3, sizeof( int ) );
  xQueueLight = xQueueCreate( 3, sizeof( float ) );

  if( xQueueTemp != NULL && xQueueHumidity != NULL && xQueueHeight != NULL && xQueuePh != NULL && xQueueLight != NULL ){

    xTaskCreate(vTaskPublicData, "Task Public Data", 6000, NULL, 2, NULL);
    xTaskCreate(vTaskMoveServo, "Task Move Servo", 2500, NULL, 1, NULL);
    xTaskCreate(vTaskLedVentilation, "Task Led Ventilation", 2500, NULL, 1, NULL);
    xTaskCreate(vTaskReadData, "Task Read Data", 4000, NULL, 3, NULL);
    xTaskCreate(vTaskResetConnection, "Task to reset connection", 6000, NULL, 4, NULL);

    printf("Tasks created\n");
  }
  
}

void setup() {
  Serial.begin(115200);
  Serial.println("System Reset");

  pinMode(LED_PIN, OUTPUT);
  pinMode(US_TRIG, OUTPUT);
  pinMode(US_ECHO, INPUT);
  dht_sensor.setup(TMP_HUM_SENSOR, DHTesp::DHT22);

  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(40);

  delay(4000);
  wifiConnect();
  mqttConnect();

  app_main();
}
void loop() {
  
}