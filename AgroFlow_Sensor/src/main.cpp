/*
===========================================================================================
 ==         ESP32 - Sensor de Umidade - Módulo de Provisionamento Híbrido         ==
===========================================================================================
 == Funcionalidades:                                                                 ==
 == 1. Se não configurado, cria um Access Point com nome único (AgroFlowSensor-XXXXXX).   ==
 == 2. Usa um Portal Cativo para redirecionar o usuário para a página de configuração.    ==
 == 3. A página de configuração escaneia e lista as redes Wi-Fi disponíveis.            ==
 == 4. Após o usuário fornecer as credenciais, salva-as e conecta à rede principal.      ==
 == 5. Usa seu endereço MAC como um ID único para se identificar na rede MQTT.           ==
 == 6. Lê um sensor de umidade de solo real (HW-080) e envia os dados via MQTT.          ==
===========================================================================================
*/

// --- Bibliotecas ---
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "time.h"

// ====== OBJETOS GLOBAIS ======
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ====== CONFIGURAÇÕES GLOBAIS ======
#define MQTT_HOST "test.mosquitto.org"
#define MQTT_PORT 1883
#define MQTT_PUB_TOPIC "sensors/humidity"
#define RESET_PIN_1 22
#define RESET_PIN_2 23

// --- NOVO: CONFIGURAÇÕES DO SENSOR ---
#define SENSOR_PIN 34 // Pino analógico onde o sensor está conectado (AOUT -> GPIO 34)

// !! IMPORTANTE: VALORES DE CALIBRAÇÃO !!
// Para leituras precisas, você DEVE calibrar estes valores para o seu sensor e solo.
// 1. Com o sensor no ar (COMPLETAMENTE SECO), veja o valor impresso no Serial Monitor e coloque aqui.
// 2. Com o sensor submerso em um copo com água, veja o valor e coloque aqui.
const int DRY_VALUE = 2850; // Valor de exemplo para sensor seco (maior valor)
const int WET_VALUE = 1350; // Valor de exemplo para sensor em água (menor valor)


// --- Configurações do Servidor de Horário (NTP) ---
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600; // Offset para o fuso horário do Brasil (GMT-3)
const int daylightOffset_sec = 0;      // Sem horário de verão

// --- Variáveis de Operação ---
String uniqueId = "";
long lastMsg = 0;
char msgBuffer[200];
char commandTopic[100];

// PÁGINA HTML DE CONFIGURAÇÃO (ARMAZENADA NA MEMÓRIA FLASH)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>Configurar Sensor AgroFlow</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 100vh; background-color: #f0f2f5; margin: 0; }
    .container { background-color: white; padding: 2rem; border-radius: 8px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); width: 100%; max-width: 400px; }
    h2 { color: #1a202c; text-align: center; }
    label { display: block; margin-bottom: 0.5rem; font-weight: 600; color: #4a5568; }
    input, select { width: 100%; padding: 0.75rem; margin-bottom: 1rem; border: 1px solid #cbd5e0; border-radius: 4px; box-sizing: border-box; }
    button { width: 100%; background-color: #2e7d32; color: white; padding: 0.85rem; border: none; border-radius: 4px; cursor: pointer; font-size: 1rem; }
    .wifi-scan { display: flex; align-items: center; gap: 0.5rem; }
    #spinner { cursor: pointer; font-size: 1.5rem; }
  </style>
  <script>
    function scanNetworks() {
      const select = document.getElementById('ssid');
      const spinner = document.getElementById('spinner');
      select.innerHTML = '<option>Procurando redes...</option>';
      fetch('/scan').then(r => r.json()).then(nets => {
        select.innerHTML = '<option value="">Selecione uma rede</option>';
        nets.forEach(n => {
          const opt = document.createElement('option');
          opt.value = n.ssid;
          opt.textContent = `${n.ssid} (${n.rssi}dBm)`;
          select.appendChild(opt);
        });
      }).catch(e => {
        select.innerHTML = '<option>Erro ao buscar redes</option>';
      });
    }
    window.onload = scanNetworks;
  </script>
</head><body>
  <div class="container">
    <h2>Conectar Sensor à Rede</h2>
    <form action="/save" method="POST">
      <label for="ssid">Rede Wi-Fi:</label>
      <div class="wifi-scan">
        <select id="ssid" name="ssid" required></select>
        <span id="spinner" onclick="scanNetworks()">&#8635;</span>
      </div>
      <label for="password">Senha da Rede:</label>
      <input type="password" id="password" name="password">
      <button type="submit">Salvar e Conectar</button>
    </form>
  </div>
</body></html>
)rawliteral";


// ====== FUNÇÕES AUXILIARES (DA VERSÃO ORIGINAL) ======
void clearConfigAndRestart() {
  Serial.println("Limpando todas as configuracoes e reiniciando...");
  preferences.clear();
  delay(1000);
  ESP.restart();
}

unsigned long long getUnixTimestampMillis() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Falha ao obter o tempo");
    return 0;
  }
  time(&now);
  return (unsigned long long)now * 1000;
}

// ====== FUNÇÕES DO PORTAL DE CONFIGURAÇÃO ======
void handleRoot() { server.send(200, "text/html", index_html); }
void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == "") continue;
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}
void handleSave() {
  preferences.putString("ssid", server.arg("ssid"));
  preferences.putString("password", server.arg("password"));
  String responsePage = "<html><body style='font-family: sans-serif; text-align: center; margin-top: 50px;'>";
  responsePage += "<h2>Configuracoes salvas!</h2>";
  responsePage += "<p>O dispositivo sera reiniciado em 3 segundos para se conectar a sua rede.</p>";
  responsePage += "</body></html>";
  server.send(200, "text/html", responsePage);
  delay(3000);
  ESP.restart();
}
void startConfigurationPortal() {
  byte mac[6];
  WiFi.macAddress(mac);
  String apName = "AgroFlowSensor-" + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
  apName.toUpperCase();
  WiFi.softAP(apName.c_str());
  IPAddress ip = WiFi.softAPIP();
  Serial.println("\n--- MODO DE CONFIGURACAO VIA PORTAL WEB ---");
  Serial.print("Conecte-se a rede: ");
  Serial.println(apName);
  Serial.print("Acesse o IP: http://");
  Serial.println(ip);
  dnsServer.start(53, "*", ip);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  Serial.println("Servidor web iniciado. Aguardando configuracao...");
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
  }
}

// --- NOVO: FUNÇÃO PARA LER O SENSOR ---
float readSensorData() {
  // Lê o valor analógico bruto do pino do sensor
  int rawValue = analogRead(SENSOR_PIN);
  
  // Imprime o valor bruto para ajudar na calibração
  Serial.print("Valor bruto do sensor: ");
  Serial.println(rawValue);

  // Mapeia o valor lido para uma porcentagem (0-100%)
  // A ordem de DRY e WET é invertida na função map() porque um valor
  // analógico mais ALTO (seco) corresponde a 0% de umidade.
  int humidityPercent = map(rawValue, DRY_VALUE, WET_VALUE, 0, 100);

  // Garante que o valor final esteja sempre dentro do intervalo de 0 a 100
  humidityPercent = constrain(humidityPercent, 0, 100);

  return (float)humidityPercent;
}


// ====== FUNÇÕES DE OPERAÇÃO (WIFI & MQTT) ======
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Mensagem recebida no topico: ");
  Serial.println(topic);
  if (length > 0) {
    payload[length] = '\0';
    String message = (char*)payload;
    Serial.print("Payload recebido: '");
    Serial.print(message);
    Serial.println("'");
    message.trim();
    if (message.equalsIgnoreCase("RESET")) {
      Serial.println("Comando de reset valido! Reiniciando...");
      clearConfigAndRestart();
    } else {
      Serial.println("Comando invalido.");
    }
  } else {
    Serial.println("Payload vazio.");
  }
}

void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Conectando ao MQTT Broker...");
    if (mqtt.connect(uniqueId.c_str())) {
      Serial.println("conectado.");
      mqtt.subscribe(commandTopic);
      Serial.print("Inscrito no topico de comando: ");
      Serial.println(commandTopic);
    } else {
      Serial.print("falhou, rc=");
      Serial.print(mqtt.state());
      Serial.println(" tentando novamente em 5 segundos");
      delay(5000);
    }
  }
}

// --- MODIFICADO: Agora publica dados REAIS do sensor ---
void publishSensorData() {
  // Chama a nova função para obter a umidade do sensor
  float humidity = readSensorData();
  unsigned long long timestamp = getUnixTimestampMillis();

  if (timestamp == 0) {
    Serial.println("Aguardando sincronizacao de tempo...");
    return;
  }

  StaticJsonDocument<200> doc;
  doc["id"] = uniqueId;
  doc["humidity"] = humidity;
  doc["timestamp"] = timestamp;
  
  size_t n = serializeJson(doc, msgBuffer);
  mqtt.publish(MQTT_PUB_TOPIC, msgBuffer, n);
  
  Serial.print("Mensagem publicada: ");
  Serial.println(msgBuffer);
}


// ====== FUNÇÕES PRINCIPAIS: SETUP & LOOP ======
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nIniciando dispositivo...");

  // Configura o ID único do dispositivo usando o endereço MAC
  byte mac[6];
  WiFi.macAddress(mac);
  for (int i = 0; i < 6; i++){
    if (mac[i] < 16) uniqueId += "0";
    uniqueId += String(mac[i], HEX);
  }
  uniqueId.toUpperCase();
  Serial.print("ID unico deste dispositivo: ");
  Serial.println(uniqueId);
  
  pinMode(RESET_PIN_1, INPUT_PULLUP);
  pinMode(RESET_PIN_2, OUTPUT);
  digitalWrite(RESET_PIN_2, LOW);

  if (digitalRead(RESET_PIN_1) == LOW) {
    Serial.println("Reset fisico detectado na inicializacao!");
    clearConfigAndRestart();
  }

  preferences.begin("sensor-config", false);
  String ssid = preferences.getString("ssid", "");

  if (ssid == "") {
    startConfigurationPortal(); // Bloqueia a execução aqui até que o dispositivo seja configurado
  } else {
    Serial.println("Configuracao encontrada. Tentando conectar a rede...");
    String password = preferences.getString("password", "");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    snprintf(commandTopic, sizeof(commandTopic), "sensors/%s/command", uniqueId.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      retries++;
      if (retries > 40) {
        Serial.println("\nFalha ao conectar. Credenciais podem estar erradas.");
        clearConfigAndRestart();
      }
    }
    Serial.print("\nWiFi conectado! IP: ");
    Serial.println(WiFi.localIP());

    Serial.println("Sincronizando relogio com servidor NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
  }
}

void loop() {
  if (digitalRead(RESET_PIN_1) == LOW) {
    Serial.println("Reset fisico detectado durante a operacao!");
    clearConfigAndRestart();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Conexao WiFi perdida. Reiniciando para tentar reconectar...");
    delay(1000);
    ESP.restart();
  }

  if (!mqtt.connected()) {
    reconnectMQTT();
  }
  mqtt.loop();

  long now = millis();
  if (now - lastMsg > 5000) { // Envia dados a cada 5 segundos
    lastMsg = now;
    // --- MODIFICADO: Chama a função que lê e publica os dados do sensor ---
    publishSensorData();
  }
}