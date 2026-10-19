#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─── Configurações de Rede e API ────────────────────────
const char* ssid = "Nome do Wifi";
const char* password = "Senha do Wifi";
const char* apiKey = "API key";
const char* urlEstacao = "https://esp32.chobi.com.br/api/sensor/estacao/";
const char* urlTranca = "https://esp32.chobi.com.br/api/sensor/acesso/";
// GET .../tranca/<id>/ -> usado para ler o comando pendente de cada tranca
const char* urlConsultaTranca = "https://esp32.chobi.com.br/api/sensor/tranca/";

// ─── LED ────────────────────────────────────────────────
const int LED = 21;

// ─── Solenoides ─────────────────────────────────────────
// Nível que ACIONA a solenoide (HIGH p/ MOSFET ou módulo relé ativo em alto).
// Se o seu módulo de relé for ativo em baixo, troque os dois valores.
#define SOLE_ATIVO   HIGH
#define SOLE_REPOUSO LOW
const unsigned long TEMPO_PULSO_MS = 500;   // quanto tempo a solenoide fica acionada

// ─── Consulta de comandos ───────────────────────────────
const unsigned long INTERVALO_POLL_MS = 1000;  // uma tranca consultada a cada 1 s (revezando)
unsigned long ultimoPoll = 0;
int proximaTranca = 0;

// ─── Estrutura de cada tranca (sensor + 2 solenoides) ───
struct Tranca {
  int id;
  int pinoTrig;
  int pinoEcho;
  int soleAberta;    // segura a tranca ABERTA. Pulso = solta e a tranca fecha
  int soleFechada;   // segura a tranca FECHADA. Pulso = solta e a tranca abre
  bool ocupada;      // estado lido pelo sensor
  long ultimoComando;          // id do último comando já tratado
  bool comandoSincronizado;    // false até a 1ª leitura (evita executar comando antigo ao ligar)
};

Tranca trancas[] = {
  //id trig echo sAberta sFechada ocupada ultCmd sinc
  {1, 13, 14, 16, 17, true, 0, false},
  {2, 18, 19, 25, 26, true, 0, false}
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

// Lê o comando pendente da tranca no servidor.
// Resposta esperada: {"comando": "fechar" | "abrir" | "", "comando_id": 17, ...}
// Retorna false se não conseguiu consultar (nesse caso NADA é acionado).
bool consultarComando(const Tranca &t, String &comando, long &comandoId) {
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(String(urlConsultaTranca) + t.id + "/");
  http.addHeader("X-API-Key", apiKey);

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("Erro consulta tranca %d: %d\n", t.id, httpCode);
    http.end();
    return false;
  }

  StaticJsonDocument<256> doc;
  DeserializationError erro = deserializeJson(doc, http.getString());
  http.end();
  if (erro) {
    Serial.printf("Erro JSON tranca %d: %s\n", t.id, erro.c_str());
    return false;
  }

  comando = doc["comando"] | "";
  comandoId = doc["comando_id"] | 0L;
  return true;
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

// ─── Ações dos sensores (mesma lógica que já funcionava) ─
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

// ─── Ações das solenoides ───────────────────────────────
void pulsarSolenoide(int pino) {
  digitalWrite(pino, SOLE_ATIVO);
  delay(TEMPO_PULSO_MS);
  digitalWrite(pino, SOLE_REPOUSO);
}

// Usuário guardou a bike: solta a tranca aberta -> ela fecha e prende na solenoide "fechada"
void fecharTranca(Tranca &t) {
  Serial.printf("\n>>> Tranca %d: FECHAR (pulso na solenoide do pino %d)\n", t.id, t.soleAberta);
  pulsarSolenoide(t.soleAberta);
}

// Usuário retirou a bike: solta a tranca fechada -> ela abre e prende na solenoide "aberta"
void abrirTranca(Tranca &t) {
  Serial.printf("\n>>> Tranca %d: ABRIR (pulso na solenoide do pino %d)\n", t.id, t.soleFechada);
  pulsarSolenoide(t.soleFechada);
}

// Consulta uma tranca por vez (revezando) e executa o comando se for novo
void verificarComandos() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - ultimoPoll < INTERVALO_POLL_MS) return;
  ultimoPoll = millis();

  Tranca &t = trancas[proximaTranca];
  proximaTranca = (proximaTranca + 1) % NUM_TRANCAS;

  String comando;
  long comandoId;
  if (!consultarComando(t, comando, comandoId)) return;   // erro: não aciona nada

  // Primeira leitura depois de ligar: só guarda o id, não executa comando antigo
  if (!t.comandoSincronizado) {
    t.ultimoComando = comandoId;
    t.comandoSincronizado = true;
    return;
  }

  if (comandoId == t.ultimoComando) return;   // comando já tratado
  t.ultimoComando = comandoId;

  if (comando == "fechar")     fecharTranca(t);
  else if (comando == "abrir") abrirTranca(t);
}

// ─── Setup ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);

  for (int i = 0; i < NUM_TRANCAS; i++) {
    pinMode(trancas[i].pinoTrig, OUTPUT);
    pinMode(trancas[i].pinoEcho, INPUT);

    // solenoides em repouso desde o início
    digitalWrite(trancas[i].soleAberta, SOLE_REPOUSO);
    digitalWrite(trancas[i].soleFechada, SOLE_REPOUSO);
    pinMode(trancas[i].soleAberta, OUTPUT);
    pinMode(trancas[i].soleFechada, OUTPUT);
    digitalWrite(trancas[i].soleAberta, SOLE_REPOUSO);
    digitalWrite(trancas[i].soleFechada, SOLE_REPOUSO);
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

  verificarComandos();   // lê comandos do servidor e aciona as solenoides
  delay(100);
}
