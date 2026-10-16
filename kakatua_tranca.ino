#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─── Configurações de Rede e API ────────────────────────
const char* ssid = "Nome do WIfi";
const char* password = "Senha do Wifi";
const char* apiKey = "Api key";
const char* urlEstacao = "https://esp32.chobi.com.br/api/sensor/estacao/";
const char* urlTranca = "https://esp32.chobi.com.br/api/sensor/acesso/";

// ─── LED ────────────────────────────────────────────────
const int LED = 21;

// ─── Estrutura de cada tranca/sensor ────────────────────
struct Tranca {
  int id;
  int pinoTrig;
  int pinoEcho;
  bool ocupada;   // antes: trancaBloqueada
};

Tranca trancas[] = {
  {1, 13, 14, true},
  {2, 18, 19, true}
};
const int NUM_TRANCAS = sizeof(trancas) / sizeof(trancas[0]);

// ─── Funções de comunicação ─────────────────────────────
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

// ─── Leitura de distância (reutilizável p/ qualquer sensor) ──
float lerDistanciaCm(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  long duracao = pulseIn(echo, HIGH, 30000);  // timeout 30 ms
  return duracao * 0.034 / 2;                 // 0 se não houve eco
}

// ─── Estação só fica bloqueada se TODAS as trancas estiverem ocupadas ──
bool todasOcupadas() {
  for (int i = 0; i < NUM_TRANCAS; i++) {
    if (!trancas[i].ocupada) return false;
  }
  return true;
}

// ─── Ações por tranca ───────────────────────────────────
void trancar(Tranca &t) {
  Serial.printf("\n>>> Tranca %d: objeto detectado! Trancando...\n", t.id);
  setTranca(t.id, "fechada", false);
  t.ocupada = true;
  setEstacao(todasOcupadas());
}

void liberar(Tranca &t) {
  Serial.printf("\n>>> Tranca %d: sensor livre! Liberando...\n", t.id);
  setTranca(t.id, "aberta", false);
  t.ocupada = false;
  setEstacao(false);   // se uma está livre, a estação não está cheia
}

// ─── Setup ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);

  for (int i = 0; i < NUM_TRANCAS; i++) {
    pinMode(trancas[i].pinoTrig, OUTPUT);
    pinMode(trancas[i].pinoEcho, INPUT);
  }

  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Conectado!");
  Serial.println("-------------------------------------");
  Serial.println("Aguardando deteccao por sensores ultrassonicos...");
  Serial.println("-------------------------------------");
}

// ─── Loop ───────────────────────────────────────────────
void loop() {
  bool algumDetectado = false;

  for (int i = 0; i < NUM_TRANCAS; i++) {
    Tranca &t = trancas[i];
    float dist = lerDistanciaCm(t.pinoTrig, t.pinoEcho);

    Serial.printf("[Sensor %d] %.1f cm | %s\n", t.id, dist,
                  t.ocupada ? "OCUPADA" : "LIVRE");

    bool detectado = (dist > 0 && dist <= 10);
    if (detectado) algumDetectado = true;

    if (detectado && !t.ocupada)      trancar(t);
    else if (!detectado && t.ocupada) liberar(t);

    delay(50);  // evita um sensor captar o eco do outro
  }

  digitalWrite(LED, algumDetectado ? HIGH : LOW);
  delay(100);
}
