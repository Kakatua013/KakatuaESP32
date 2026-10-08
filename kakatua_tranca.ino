#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─── Configurações de Rede e API ────────────────────────
const char* ssid = "NOME DO WIFI DE ACESSO";
const char* password = "SENHA DO WIFI DE ACESSO";
const char* apiKey = "insira API KEY AQUI";
const char* urlEstacao = "https://esp32.chobi.com.br/api/sensor/estacao/";
const char* urlTranca = "https://esp32.chobi.com.br/api/sensor/acesso/";

// ─── Configurações do Sensor e LED ──────────────────────
const int LED = 21;
const int pinoTrig = 13;
const int pinoEcho = 14;

// ─── Controle de Estado ─────────────────────────────────
bool trancaBloqueada = true;

// ─── Funções de comunicação ──────────────────────────────
void setEstacao(bool bloqueada) {
  HTTPClient http;
  http.begin(urlEstacao);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", apiKey);

  StaticJsonDocument<64> doc;
  doc["bloqueada"] = bloqueada;
  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode == 200) {
    Serial.println(bloqueada ? "-> API: Estacao BLOQUEADA" : "-> API: Estacao DESBLOQUEADA");
  } else {
    Serial.printf("Erro estacao: %d\n", httpCode);
  }
  http.end();
}

void setTranca(int id_tranca, String estado, bool reservada) {
  HTTPClient http;
  http.begin(urlTranca);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", apiKey);

  StaticJsonDocument<200> doc;
  doc["login"] = "esp32";
  doc["senha"] = "esp32";
  doc["id_tranca"] = id_tranca;
  doc["estado_tranca"] = estado;
  doc["tranca_reservada"] = reservada;
  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode == 200) {
    Serial.printf("-> API: Tranca %d atualizada para %s\n", id_tranca, estado.c_str());
  } else {
    Serial.printf("Erro tranca: %d\n", httpCode);
  }
  http.end();
}

// ─── Funções de Ação ─────────────────────────────────────
void trancar() {
  Serial.println("\n>>> Objeto detectado pelo sensor! Trancando a estacao...");
  setTranca(1, "fechada", false);
  setEstacao(true);
  trancaBloqueada = true;
}

// ─── Setup ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(LED, OUTPUT);
  pinMode(pinoTrig, OUTPUT);
  pinMode(pinoEcho, INPUT);

  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Conectado!");
  Serial.println("-------------------------------------");
  Serial.println("Aguardando deteccao por sensor ultrassonico...");
  Serial.println("-------------------------------------");
}

// ─── Loop ────────────────────────────────────────────────
void loop() {
  // 1. Dispara o pulso ultrassônico
  digitalWrite(pinoTrig, LOW);
  delayMicroseconds(2);
  digitalWrite(pinoTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinoTrig, LOW);

  // 2. Lê o tempo de retorno e calcula a distância
  long duracao = pulseIn(pinoEcho, HIGH);
  float distanciaCm = duracao * 0.034 / 2;

  Serial.printf("[Sensor] Distancia: %.1f cm | Tranca: %s\n", 
    distanciaCm, 
    trancaBloqueada ? "BLOQUEADA" : "LIVRE");

  // 3. Objeto detectado → trava
  if (distanciaCm > 0 && distanciaCm <= 10) {
    digitalWrite(LED, HIGH);

    if (!trancaBloqueada) {
      trancar();
    }

  // 4. Sem objeto → libera
  } else {
    digitalWrite(LED, LOW);

    if (trancaBloqueada) {
      Serial.println("\n>>> Sensor livre! Desbloqueando a estacao...");
      setTranca(1, "aberta", false);
      setEstacao(false);
      trancaBloqueada = false;
    }
  }

  delay(100);
}
