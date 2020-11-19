/** Proyecto HoMeter Versión 0.9 
 *  
 *  Work in progress...
 *  
 *  Versión basada en un ESP8266: el 8266 solo tiene un ADC de baja precisión, lo que 
 *  limita la implementación a una sola línea de corriente.  Posteriores versiones
 *  mejorarán esta condición
 *  
 *  1) Generador de mensajes para Watson IoT (solo los mensajes) -- Ok
 *  2) Conexión a WiFi -- Ok
 *  3) Conexión a Watson IoT -- Ok.
 *  4) Publicar mensajes -- Ok.
 *  5) Agregar medidor de consumo de agua -- Ok.
 *  6) Agregar medidor de consumo de energía -- Ok.
 *  7) Agregar recepción de comandos para prender - apagar circuito
 *  8) Modificar para que sea SSL
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

/**==========================
 * Medidor de consumo de agua
 * ==========================
 * 
 * Notas sobre la implementación:  
 * 
 * Se usa una filosofía de medir los pulsos del medidor de flujo de agua
 * a partir de los "rise ups", definiendo una interrupción que se activa
 * cuando la entrada pasa de BAJA a ALTA
 */

// constantes y variables específicas para medición de flujo de agua
#define EEPROM_LITERS_OFFSET 0   // Offset de dirección EEPROM

// estas las dejo como constantes para cambiarlas por valores parametrizables
// en la próxima versión
const int flowMeterPin = D2;
const int pulsesPerLitre = 440;  // número de pulsos por litro (ver documentación  
                                 // del medidor que se tenga disponible
int flowCounter = 0; // cuenta los pulsos del medidor de flujo
int litres = 0;      // cuenta los litros consumidos entre reportes a Watson IoT

/**
 * Función de interrupción que incrementa el contador
 */
void ICACHE_RAM_ATTR pin_ISR()
{
  flowCounter++;
}

/**
 * Configura la medición de flujo de agua
 */
void setupFlowMeter() 
{
  // inicializa el acceso a la memoria EEPROM
  EEPROM.begin(1);
  litres=EEPROM.read(EEPROM_LITERS_OFFSET);

  // inicializa el pin de entrada para el medidor de flujo de agua
  pinMode(flowMeterPin, INPUT);

  // Agrega un manejador de interrupciones al vector de ISR
  attachInterrupt(digitalPinToInterrupt(flowMeterPin), pin_ISR, RISING);
}

/**
 * función que verifica el número de pulsos del medidor de flujo
 * de agua y acumula los litros consumidos
 */
void accumLitres() 
{
  if (flowCounter > pulsesPerLitre) 
  {
    litres++;
    Serial.println();
    Serial.print("Litres: ");
    Serial.print(litres);

    // Escribe el número de litros en la EEPROM y resta el número de pulsos al contador
    // no lo reinicia a cero, porque en este lapso pudo darse un nuevo pulso
    EEPROM.write(EEPROM_LITERS_OFFSET, litres);
    EEPROM.commit();
    flowCounter -= pulsesPerLitre;
  }
}

/**=============================
 * Medidor de consumo de energía
 * =============================
 * 
 * Notas sobre la implementación:  
 * 
 * Se usa un medidor no intrusivo tipo SCT013 de 30A/1V. Esta primera versión utiliza el 
 * ADC del ESP8266, que es muy limitado en precisión: es de 10 bit y con algunos problemas
 * de linearidad en los extremos del rango util.
 * En la tarjeta de desarrollo ESP8266 la entrada se amplifica a 3.3V, luego el programa 
 * debe ser recalibrado si se usa una tarjeta diferente como la ESP01
 * 
 * La segunda limitación importante es que ESP8266 solo tiene una entrada ADC, por lo que
 * solo es usable para una prueba de concepto.
 */

// constantes y variables específicas para medición de consumo energético

#define EEPROM_ACCUM_KWH_OFFSET 8  // Offset de dirección EEPROM
#define ADC_INPUT A0               // Pin del ADC
#define HOME_VOLTAGE 110.0         // Voltaje domiciliario estándar en Colombia
#define SCT_MAX_CURRENT 30.0       // Corriente máxima para el SCT013 usado
#define MILLIS_PER_HOUR 3600000.0  // Milisegundos en una hora
long kwhMillis = millis();         // Inicia un control de tiempo específico 
                                   // para las mediciones de corriente
double totalKwh;                   // acumulador para consumo de energía

/**
 * inicializa la medición de kwh
 * como parte de esta inicialización, lee el valor que pueda haber en EEPROM
 * para el consumo acumulado
 */
void kwhSetup()
{
  // inicializa el acceso a la memoria EEPROM
  // qué pasará si se inicia la lectura del EEPROM cuando ya ha sido inicializado?
  EEPROM.begin(1);
  totalKwh=EEPROM.read(EEPROM_ACCUM_KWH_OFFSET);
  kwhMillis = millis();
}

/**
 * Mide la corriente promedio consumida durante medio segundo usando el 
 * método irms (corriente media) 
 */
double getIrms()
{
  /* El valor del multiplicador hay que afinarlo: */
  float voltageSensor;
  float current = 0;
  float sum = 0; // lleva la sumatoria de los cuadrados de la corriente medida
  long startTime = millis(); // tiempo en el que comienza la medición
  int n = 0; // contador de muestreos

  while (millis() - startTime < 500) // toma muestras durante 500 milisegundos
  {
    // calcula el voltaje entregado por el sensor
    // el ADC del ESP12 es de 10 bit. Tengo duda con el voltaje: La tarjeta del kit de 
    // desarrollo debe tener un divisor de voltaje para que trabaje a 3.3V... 
    voltageSensor = analogRead(ADC_INPUT) * (1.1 / 1023.0); // ADC es de 10bit
                
    current = voltageSensor * SCT_MAX_CURRENT;
    sum += sq(current); 
    n++;  
    delay(1);
  }
  sum *= 2; // para compensar el hemiciclo negativo
  current = sqrt(sum)/n; // ecuación de rms
  return current;
}


/**
 * mide el consumo durante medio segundo y extrapola este consumo al período desde la
 * anterior medición, para rellenar el tiempo no medido porque el ESP8266 estuviese
 * haciendo otras labores.
 */
void accumKwh() {
  // mide consumo 
  float irms = getIrms(); // obtiene la corriente efectiva durante medio segundo
  float p = irms * HOME_VOLTAGE; // potencia = corriente * voltaje y se expresa en Watts
  long prevMillis = kwhMillis;
  kwhMillis = millis();
  double kwh = p * (kwhMillis - prevMillis) / MILLIS_PER_HOUR;  
  
  // acumula el número de kwh y los graba en EEPROM para que no se pierda en caso de 
  // reinicio del HoMeter
  totalKwh += kwh;

  // Escribe el acumulado de kwh en la EEPROM para que no se pierda en caso de reinicio
  EEPROM.write(EEPROM_ACCUM_KWH_OFFSET, totalKwh);
  EEPROM.commit();
}


/**=================
 * Conectividad WiFi
 * =================
 * 
 * Notas sobre la implementación
 * 
 * Una de las capacidades atractivas de ESP8266 es poseer WiFi integrado 
 * Tiene algunas limitaciones como no soportar 5GHz y no aceptar nombres de red con 
 * espacios en blanco o caracteres especiales, pero por lo demás es muy funcional.
 */

// Definición de variables y constantes para establecer conectividad WiFi
const char* ssid="XXXXXXXX";          // Nombre de la red WiFi
const char* password="xxxxxxxx";      // Contraseña de la red WiFi


/*
 * Función que se conecta a la red WiFi
 */
void wifiConnect() {
  Serial.print("Conectándose a "); Serial.print(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nConectado a WiFi. Dirección IP: "); Serial.println(WiFi.localIP());
}


/**===============================
 * Conectividad con IBM Watson IoT
 * ===============================
 * 
 * Notas sobre la implementación
 * 
 * Basado en el ejemplo y taller práctico de de Ant Elder en 
 * https://developer.ibm.com/recipes/tutorials/run-an-esp8266arduino-as-a-iot-foundation-managed-device/
 */

// Definición de Constantes y Variables para Comunicación con Watson IoT
#define ORG "xxxxxx"                   // Nombre de la organización en Watson IoT
#define DEVICE_TYPE "xxxxxxxxxx"       // Nombre del tipo de dispositivo configurado en Watson IoT
#define DEVICE_ID "xxxxxxxxxxxxxx"     // Nombre del dispositivo configurado en Watson IoT
#define TOKEN "xxxxxxxxxxxxxxxxxx"     // Token de Acceso para el dispositivo

// La URL del servicio de Watson IoT se ensambla agregando al nombre de la organización
// la constante ".messaging.internetofthings.ibmcloud.com"
char server[] = ORG ".messaging.internetofthings.ibmcloud.com";  
char authMethod[] = "use-token-auth";  // Tipo de identificación que se usará para conectarse a WIoT
char token[] = TOKEN;

// La identificación del dispositivo en Watson IoT está conformado por la siguiente contatenación de 
// valores
char clientId[] = "d:" ORG ":" DEVICE_TYPE ":" DEVICE_ID;

// definición de tópicos para...
const char publishTopic[] = "iot-2/evt/status/fmt/json";  // publicar mediciones
const char onoffTopic[] = "iot-2/cmd/onoffcmd/fmt/json";  // recibe comandos on-off
const char manageTopic[] = "iotdevice-1/#";               // recibir comandos de configuración
const char responseTopic[] = "iotdevice-1/response";      // responder a solicitudes del servidor
const char updateTopic[] = "iotdm-1/device/update";       // recibir actualizaciones
const char rebootTopic[] = "iotdm-1/mgmt/initiate/device/reboot"; // recibir órdenes de reinicio

int publishInterval = 30000;  // Intervalo entre publicaciones de medidas
                              // 30 segundos por default

long lastPublishMillis;
/*
 * callback, operación a ser ejecutada cuando se recibe un mensaje
 */
void callback(char* topic, byte* payload, unsigned int payloadLength) {
  Serial.print("callback invocado para tópico: "); Serial.println(topic);

  if (strcmp (responseTopic, topic) == 0) {
    return; // para el ejemplo, solo imprime la respuesta
  }

  if (strcmp (rebootTopic, topic) == 0) {
    Serial.println("Reiniciando...");
    ESP.restart();
  }

  if (strcmp (updateTopic, topic) == 0) {
    handleUpdate(payload);
  }
}

WiFiClient wifiClient;
PubSubClient client(server, 1883, callback, wifiClient);


/*
 * Función que se conecta a MQTT
 */
void mqttConnect() {
  if (!client.connected()) {
    Serial.print("Reconectando cliente MQTT con "); Serial.println(server);
    client.setKeepAlive(60);
    while(!client.connect(clientId, authMethod, token)) {
      Serial.print(".");
      delay(500);
    }
    Serial.println();
  }
}


/*
 * se subscribe a los tópicos de gestión del ejemplo
 */
void initManagedDevice() {
  if (client.subscribe(responseTopic)) {
    Serial.println("Suscripción a respuestas exitosa");
  } else {
    Serial.println("Suscripción a respuestas falló");
  }

  if (client.subscribe(rebootTopic)) {
    Serial.println("Suscripción a reboot exitosa");
  } else {
    Serial.println("Falló suscripción a reboot");
  }

  if (client.subscribe(updateTopic)) {
    Serial.println("Suscripción a actualizaciones exitosa");
  } else {
    Serial.println("Falló suscripción a actualizaciones");
  }

  if (client.subscribe(onoffTopic)) {
    Serial.println("Suscripción a on/off exitosa");
  } else {
    Serial.println("Falló suscripción a on/off");
  }

  if (client.subscribe(manageTopic)) {
    Serial.println("Suscripción a gestión exitosa");
  } else {
    Serial.println("Falló suscripción a gestión");
  }

  StaticJsonDocument<300> jsonDocument;
  jsonDocument["d"]["metadata"]["PUBLISH_INTERVAL"] = publishInterval;
  jsonDocument["d"]["supports"]["deviceActions"] = true;

  Serial.println("publicando metadata del dispositivo:"); 
  serializeJsonPretty(jsonDocument, Serial); 
  Serial.println();
  
  char buff[300];
  serializeJson(jsonDocument, buff);
  if (client.publish(manageTopic, buff)) {
    Serial.println("Publicación del dispositivo exitosa");
  } else {
    Serial.println("Falló publicación del dispositivo");
  }
}


/*
 * Publicación periódica de datos
 */
void publishData() {
  StaticJsonDocument<512> jsonDocument;
  jsonDocument["d"]["counter"] = millis() / 1000;
  jsonDocument["d"]["flowCounter"] = flowCounter;
  jsonDocument["m"]["water_01"]["msu"] = "litres";
  jsonDocument["m"]["water_01"]["value"] = litres;
  jsonDocument["m"]["power_01"]["msu"] = "kwh";
  jsonDocument["m"]["power_01"]["msu"] = totalKwh;
  
  Serial.println("Publicando datos:"); serializeJsonPretty(jsonDocument, Serial); Serial.println();
  char buff[512];
  serializeJson(jsonDocument, buff);

  // Si no hay conexión con Watson IoT MQTT, la restablece
  mqttConnect();
  
  if (client.publish(publishTopic, buff)) {
    Serial.println("Publicación exitosa");
  } else {
    Serial.println("Falló publicación de datos");
  }
}


/*
 * gestiona actualización
 */
void handleUpdate(byte* payload) {
  StaticJsonDocument<300> jsonDocument;
  DeserializationError error = deserializeJson(jsonDocument, payload);
  if (error) {
    Serial.println("handleUpdate: Falló con error ");
    Serial.println(error.c_str());
    return;
  }
  
  Serial.println("handleUpdate: datos:"); serializeJsonPretty(jsonDocument, Serial); Serial.println();

  JsonArray fields = jsonDocument["d"]["fields"];
  for (JsonArray::iterator it = fields.begin(); it != fields.end(); ++it) {
    JsonObject field = *it;
    const char* fieldName = field["field"];
    if (strcmp(fieldName, "metadata") == 0) {
      JsonObject fieldValue = field["value"];
      if (fieldValue.containsKey("PUBLISH_INTERVAL")) {
        publishInterval = fieldValue["PUBLISH_INTERVAL"];
        Serial.print("PUBLISH_INTERVAL:"); Serial.println(publishInterval);
      }
    }
  }
}


/**
 * Inicialización típica del marco de trabajo Arduino
 */
void setup() {
  // inicializa puerto serial para debugging
  Serial.begin(115200); Serial.println();

  // inicializa medidor de flujo de agua
  setupFlowMeter();

  // inicializa el medidor de consumo eléctrico
  kwhSetup();

  // conecta a WiFi
  wifiConnect();

  // conecta a Watson IoT para poder matricular 
  mqttConnect();

  // inicializa gestión del dispositivo
  initManagedDevice();
}


/**
 * loop principal típico del marco de trabajo Arduino
 */
void loop() {
  // Acumula litros
  accumLitres();

  // Acumula consumo energético
  accumKwh();

  // publica datos si llegó el momento de hacerlo
  if (millis() - lastPublishMillis > publishInterval) {
    publishData();
    lastPublishMillis = millis();
  }
  delay(1);
}
