#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ═══ CONFIGURAÇÃO ═══════════════════════════════════════════════════════════

// Portal de configuração (WiFi e API key ficam salvos na memória do ESP)
const char* NOME_PORTAL  = "Kakatua-Config";   // nome da rede que o ESP cria
const char* SENHA_PORTAL = "kakatua123";       // senha dessa rede (mínimo 8 caracteres)
const int PINO_CONFIG = 0;                     // botão BOOT: segure 3 s para reabrir o portal
const unsigned long TEMPO_BOTAO_MS = 3000;

// Servidor. Use UMA das duas linhas:
//  A) Domínio com HTTPS (precisa de internet; se o ESP estiver na MESMA rede do servidor
//     e der erro de conexão, o roteador pode não suportar "NAT loopback": use a opção B).
//  B) IP do servidor na MESMA rede do ESP, HTTP direto: mais simples e bem mais rápido.
const char* API_BASE = "https://esp32.chobi.com.br/api/sensor";
// const char* API_BASE = "http://192.168.15.4:8000/api/sensor";

// LED
const int LED = 21;

// Solenoides: nível que ACIONA (HIGH p/ MOSFET ou relé ativo em alto).
// Se o seu módulo de relé for ativo em baixo, troque os dois valores.
#define SOLE_ATIVO   HIGH
#define SOLE_REPOUSO LOW
const unsigned long TEMPO_PULSO_MS = 5000;   // quanto tempo a solenoide fica acionada

// Sensores: histerese + confirmação, para não ficar "pulando" entre livre e ocupada.
const float   DIST_OCUPADA_CM = 10.0;   // até aqui = ocupada
const float   DIST_LIVRE_CM   = 14.0;   // daqui pra cima (ou sem eco) = livre
const uint8_t CONFIRMACOES    = 3;      // leituras seguidas iguais para aceitar a mudança
const bool    DEBUG_LEITURAS  = true;   // mostra no Serial as 3 leituras brutas de cada sensor

// Envio ao servidor
const unsigned long RETRY_MS     = 10000;   // se o envio falhar, tenta de novo depois disso
const unsigned long HEARTBEAT_MS = 0;       // 0 = só envia quando muda. Ex.: 60000 reenvia a cada 60 s
                                            // (cuidado: o envio manda tranca_reservada=false)
const unsigned long OFFLINE_REINICIA_MS = 300000;  // reinicia o ESP se ficar 5 min sem WiFi

// Consulta de comandos
const unsigned long INTERVALO_POLL_MS = 1000;   // uma tranca consultada a cada 1 s (revezando)

// Leitor RFID (RC522). Alimente em 3,3 V (NUNCA em 5 V).
// Pinos escolhidos para não colidir com sensores (13,14,18,19), solenoides (16,17,25,26) e LED (21).
const int PINO_RFID_SCK  = 27;
const int PINO_RFID_MISO = 32;
const int PINO_RFID_MOSI = 33;
const int PINO_RFID_SS   = 4;     // pino SDA/SS do módulo
const int PINO_RFID_RST  = 22;
// Estação onde o leitor fica. 0 = um leitor para todas as estações: o servidor escolhe
// (guarda na 1ª estação livre; libera a estação que for do dono da tag).
const int ID_TRANCA_LEITOR = 0;
const unsigned long RFID_REPETE_MS = 3000;   // ignora a mesma tag lida de novo dentro desse tempo

// ═══ ESTADO GLOBAL ══════════════════════════════════════════════════════════

Preferences prefs;
WiFiManager wm;
WiFiManagerParameter paramApiKey("apikey", "API key do servidor", "", 80,
                                 "placeholder=\"deixe em branco para manter a atual\"");
String apiKey;

// Endereço do site (Django) que recebe as leituras de tag. Configurado pelo portal.
WiFiManagerParameter paramSite("site", "Endereco do site (ex.: https://kakatua.exemplo.com.br)", "", 100,
                               "placeholder=\"deixe em branco para manter o atual\"");
String siteBase;

MFRC522 rfid(PINO_RFID_SS, PINO_RFID_RST);
bool rfidOk = false;
String ultimaTag;
unsigned long ultimaTagEm = 0;

WiFiClientSecure clienteTLS;
WiFiClient clientePlano;

unsigned long ultimoPoll = 0;
unsigned long intervaloPoll = INTERVALO_POLL_MS;
int proximaTranca = 0;

const int8_t DESCONHECIDO = -1;   // ainda não leu o sensor com confiança
const int8_t LIVRE        = 0;
const int8_t OCUPADA      = 1;

struct Tranca {
  int id;
  int pinoTrig;
  int pinoEcho;
  int soleAberta;      // segura a tranca ABERTA. Pulso = solta e a tranca fecha
  int soleFechada;     // segura a tranca FECHADA. Pulso = solta e a tranca abre
  int8_t estado;       // estado confirmado pelo sensor
  uint8_t confirmacoes;        // leituras seguidas que contradizem o estado atual
  bool pendente;               // true = servidor ainda não recebeu o estado atual
  unsigned long proximaTentativa;
  unsigned long ultimoOk;
  long ultimoComando;          // id do último comando já tratado
  bool comandoSincronizado;    // false até a 1ª leitura (evita executar comando antigo ao ligar)
};

Tranca trancas[] = {
  // id trig echo sAberta sFechada estado        cfm pend prox ok cmd sinc
  {1, 13, 14, 16, 17, DESCONHECIDO, 0, false, 0, 0, 0, false},
  {2, 18, 19, 25, 26, DESCONHECIDO, 0, false, 0, 0, 0, false}
};
const int NUM_TRANCAS = sizeof(trancas) / sizeof(trancas[0]);

// ═══ PORTAL E BOTÃO ═════════════════════════════════════════════════════════

void salvarConfig() {
  String nova = String(paramApiKey.getValue());
  nova.trim();
  if (nova.length() > 0 && nova != apiKey) {
    apiKey = nova;
    prefs.putString("apikey", apiKey);
    Serial.println("API key atualizada.");
  }

  String site = String(paramSite.getValue());
  site.trim();
  while (site.endsWith("/")) site.remove(site.length() - 1);
  if (site.length() > 0 && site != siteBase) {
    siteBase = site;
    prefs.putString("site", siteBase);
    Serial.printf("Endereco do site atualizado: %s\n", siteBase.c_str());
  }
}

void abrirPortalConfig() {
  Serial.println("\n>>> Abrindo portal de configuracao...");
  Serial.printf("Conecte no WiFi \"%s\" (senha: %s) e abra 192.168.4.1\n", NOME_PORTAL, SENHA_PORTAL);
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal(NOME_PORTAL, SENHA_PORTAL);   // bloqueia até salvar ou expirar
  salvarConfig();
  ESP.restart();
}

void verificarBotaoConfig() {
  static unsigned long inicio = 0;
  if (digitalRead(PINO_CONFIG) == LOW) {
    if (inicio == 0) inicio = millis();
    else if (millis() - inicio > TEMPO_BOTAO_MS) {
      inicio = 0;
      abrirPortalConfig();
    }
  } else {
    inicio = 0;
  }
}

// ═══ WIFI ═══════════════════════════════════════════════════════════════════

void garantirWiFi() {
  static unsigned long offlineDesde = 0;
  static unsigned long ultimaTentativa = 0;

  if (WiFi.status() == WL_CONNECTED) {
    offlineDesde = 0;
    return;
  }
  if (offlineDesde == 0) offlineDesde = millis();
  if (millis() - offlineDesde > OFFLINE_REINICIA_MS) {
    Serial.println("Muito tempo sem WiFi. Reiniciando...");
    ESP.restart();
  }
  if (millis() - ultimaTentativa < 10000) return;
  ultimaTentativa = millis();
  Serial.println("WiFi caiu. Tentando reconectar...");
  WiFi.disconnect();
  WiFi.reconnect();
}

// ═══ HTTP ═══════════════════════════════════════════════════════════════════

// Explica no Serial o motivo do erro (é a principal pista quando algo não chega ao servidor)
void logHttp(const char* quem, int code) {
  const char* dica = "";
  switch (code) {
    case -100: dica = "sem WiFi"; break;
    case -101: dica = "URL invalida (confira API_BASE)"; break;
    case -102: dica = "API key vazia: segure BOOT por 3 s e configure no portal"; break;
    case 301: case 302: case 307: case 308:
      dica = "redirecionamento (confira http/https e a barra final da URL)"; break;
    case 400: dica = "host nao permitido (ALLOWED_HOSTS) ou JSON recusado"; break;
    case 401: case 403: dica = "API key ou login recusados (compare o tamanho/final da chave mostrado no boot com o do servidor)"; break;
    case 404: dica = "rota ou ID nao existe no servidor"; break;
    case 500: case 502: case 503: dica = "erro no servidor (veja os logs do Django/Nginx)"; break;
    default:
      if (code < 0) dica = "falha de conexao/TLS/DNS";
      break;
  }
  if (code < 0 && code > -100) {
    Serial.printf("[HTTP] %s -> %d (%s) %s\n", quem, code, HTTPClient::errorToString(code).c_str(), dica);
  } else {
    Serial.printf("[HTTP] %s -> %d %s\n", quem, code, dica);
  }
}

// Faz GET/POST no servidor. Retorna o código HTTP (negativo = falha local).
int requisicao(const char* metodo, const String &caminho, const String &corpo, String *resposta = nullptr,
               const String *base = nullptr, uint16_t timeoutMs = 4000) {
  if (WiFi.status() != WL_CONNECTED) return -100;
  if (apiKey.length() == 0) return -102;

  String url = (base != nullptr ? *base : String(API_BASE)) + caminho;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(4000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool iniciou;
  if (url.startsWith("https://")) {
    clienteTLS.setInsecure();   // sem validar certificado (simples). Para validar, use setCACert().
    iniciou = http.begin(clienteTLS, url);
  } else {
    iniciou = http.begin(clientePlano, url);
  }
  if (!iniciou) return -101;

  http.addHeader("X-API-Key", apiKey);
  int code;
  if (strcmp(metodo, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(corpo);
  } else {
    code = http.GET();
  }
  if (code > 0) {
    String corpoResp = http.getString();
    // em caso de erro, mostra o que o servidor respondeu (costuma dizer o motivo)
    if (code >= 400) Serial.printf("[HTTP] resposta do servidor: %.150s\n", corpoResp.c_str());
    if (resposta != nullptr) *resposta = corpoResp;
  }
  http.end();
  return code;
}

bool enviarEstacao(bool bloqueada) {
  StaticJsonDocument<64> doc;
  doc["bloqueada"] = bloqueada;
  String payload;
  serializeJson(doc, payload);

  int code = requisicao("POST", "/estacao/", payload);
  if (code == 200 || code == 201) {
    Serial.println(bloqueada ? "-> API: Estacao BLOQUEADA" : "-> API: Estacao DESBLOQUEADA");
    return true;
  }
  logHttp("estacao", code);
  return false;
}

bool enviarTranca(int id_tranca, const char* estado, bool reservada) {
  StaticJsonDocument<200> doc;
  doc["login"] = "esp32";
  doc["senha"] = "esp32";
  doc["id_tranca"] = id_tranca;
  doc["estado_tranca"] = estado;
  doc["tranca_reservada"] = reservada;
  String payload;
  serializeJson(doc, payload);

  int code = requisicao("POST", "/acesso/", payload);
  if (code == 200 || code == 201) {
    Serial.printf("-> API: Tranca %d atualizada para %s\n", id_tranca, estado);
    return true;
  }
  logHttp("tranca", code);
  return false;
}

// Lê o comando pendente da tranca no servidor.
// Resposta esperada: {"comando": "fechar" | "abrir" | "", "comando_id": 17, ...}
// Retorna false se não conseguiu consultar (nesse caso NADA é acionado).
bool consultarComando(const Tranca &t, String &comando, long &comandoId) {
  String resp;
  int code = requisicao("GET", "/tranca/" + String(t.id) + "/", "", &resp);
  if (code != 200) {
    logHttp(("consulta tranca " + String(t.id)).c_str(), code);
    return false;
  }

  StaticJsonDocument<256> doc;
  DeserializationError erro = deserializeJson(doc, resp);
  if (erro) {
    Serial.printf("Erro JSON tranca %d: %s\n", t.id, erro.c_str());
    return false;
  }

  const char* c = doc["comando"] | "";
  comando = String(c);
  comandoId = doc["comando_id"] | 0L;
  return true;
}

// ═══ SENSORES ═══════════════════════════════════════════════════════════════

// Uma leitura do HC-SR04. Retorna 0 se não houve eco.
float lerDistanciaCm(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  long duracao = pulseIn(echo, HIGH, 30000);  // timeout 30 ms
  return duracao * 0.034 / 2;                 // 0 se não houve eco
}

// Mediana de 3 leituras: ignora leituras isoladas erradas (eco perdido, ruído).
float medirDistanciaCm(int trig, int echo, float *bruto) {
  float v[3];
  for (int k = 0; k < 3; k++) {
    v[k] = lerDistanciaCm(trig, echo);
    bruto[k] = v[k];
    delay(30);   // também evita um sensor captar o eco do outro
  }
  if (v[0] > v[1]) { float x = v[0]; v[0] = v[1]; v[1] = x; }
  if (v[1] > v[2]) { float x = v[1]; v[1] = v[2]; v[2] = x; }
  if (v[0] > v[1]) { float x = v[0]; v[0] = v[1]; v[1] = x; }
  return v[1];
}

// Histerese: entre DIST_OCUPADA_CM e DIST_LIVRE_CM mantém o estado que já estava.
int8_t classificar(const Tranca &t, float d) {
  if (d > 0 && d <= DIST_OCUPADA_CM) return OCUPADA;
  if (d == 0 || d >= DIST_LIVRE_CM) return LIVRE;
  return t.estado;
}

const char* textoEstado(int8_t e) {
  if (e == OCUPADA) return "OCUPADA";
  if (e == LIVRE) return "LIVRE";
  return "?";
}

// A estação só fica bloqueada se TODAS as trancas estiverem ocupadas
bool todasOcupadas() {
  for (int i = 0; i < NUM_TRANCAS; i++) {
    if (trancas[i].estado != OCUPADA) return false;
  }
  return true;
}

void atualizarTranca(Tranca &t) {
  float bruto[3];
  float d = medirDistanciaCm(t.pinoTrig, t.pinoEcho, bruto);
  int8_t visto = classificar(t, d);

  if (DEBUG_LEITURAS) {
    Serial.printf("[Sensor %d] %.1f cm (brutas %.1f %.1f %.1f) | %s | confirmando %u/%u\n",
                  t.id, d, bruto[0], bruto[1], bruto[2],
                  textoEstado(t.estado), t.confirmacoes, CONFIRMACOES);
  }

  if (visto == DESCONHECIDO || visto == t.estado) {
    t.confirmacoes = 0;
    return;
  }
  if (++t.confirmacoes < CONFIRMACOES) return;

  // mudança confirmada
  t.estado = visto;
  t.confirmacoes = 0;
  t.pendente = true;                 // precisa avisar o servidor
  t.proximaTentativa = millis();     // e já
  Serial.printf("\n>>> Tranca %d: %s\n", t.id,
                visto == OCUPADA ? "objeto detectado (OCUPADA)" : "sensor livre (LIVRE)");
}

// Envia ao servidor o estado das trancas. Só dá a mudança como entregue se o servidor
// confirmar; se falhar, tenta de novo depois (antes o estado se perdia em silêncio).
void sincronizarComServidor() {
  unsigned long agora = millis();
  for (int i = 0; i < NUM_TRANCAS; i++) {
    Tranca &t = trancas[i];
    if (t.estado == DESCONHECIDO) continue;

    if (!t.pendente && HEARTBEAT_MS > 0 && agora - t.ultimoOk >= HEARTBEAT_MS) {
      t.pendente = true;
      t.proximaTentativa = agora;
    }
    if (!t.pendente) continue;
    if ((long)(agora - t.proximaTentativa) < 0) continue;

    bool ok = enviarTranca(t.id, t.estado == OCUPADA ? "fechada" : "aberta", false)
              && enviarEstacao(todasOcupadas());
    if (ok) {
      t.pendente = false;
      t.ultimoOk = millis();
    } else {
      t.proximaTentativa = millis() + RETRY_MS;
      Serial.printf("   Tranca %d: nao entregue, nova tentativa em %lu s\n", t.id, RETRY_MS / 1000);
    }
  }
}

// ═══ SOLENOIDES ═════════════════════════════════════════════════════════════

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
  if (millis() - ultimoPoll < intervaloPoll) return;
  ultimoPoll = millis();

  Tranca &t = trancas[proximaTranca];
  proximaTranca = (proximaTranca + 1) % NUM_TRANCAS;

  String comando;
  long comandoId;
  if (!consultarComando(t, comando, comandoId)) {
    // servidor fora do ar: espaça as consultas (até 10 s) para não travar o loop dos sensores
    intervaloPoll = (intervaloPoll * 2 > 10000UL) ? 10000UL : intervaloPoll * 2;
    return;
  }
  intervaloPoll = INTERVALO_POLL_MS;

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

// ═══ LEITOR RFID ════════════════════════════════════════════════════════════

void iniciarRFID() {
  SPI.begin(PINO_RFID_SCK, PINO_RFID_MISO, PINO_RFID_MOSI, PINO_RFID_SS);
  rfid.PCD_Init();
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  rfidOk = (v != 0x00 && v != 0xFF);
  if (rfidOk) {
    Serial.printf("RFID: leitor encontrado (versao 0x%02X)\n", v);
  } else {
    Serial.println("RFID: leitor NAO encontrado (confira a fiacao e a alimentacao de 3,3 V)");
  }
}

// Retorno visual para o usuário: n piscadas de ms cada
void piscarLED(int n, int ms) {
  for (int i = 0; i < n; i++) {
    digitalWrite(LED, HIGH);
    delay(ms);
    digitalWrite(LED, LOW);
    delay(ms);
  }
}

// Lê uma tag (se houver) e devolve o UID em hexadecimal maiúsculo; "" se não houver
String lerTag() {
  if (!rfidOk) return "";
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return "";

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
}

// Manda o UID ao site. O site decide: cadastrar a tag, guardar a bike ou liberar a estação.
void enviarTag(const String &uid) {
  if (siteBase.length() == 0) {
    Serial.println("Tag lida, mas o endereco do site nao esta configurado (segure BOOT por 3 s).");
    piscarLED(3, 80);
    return;
  }

  StaticJsonDocument<96> doc;
  doc["uid"] = uid;
  doc["id_tranca"] = ID_TRANCA_LEITOR;
  String payload;
  serializeJson(doc, payload);

  String resp;
  // tempo maior: o site ainda consulta/comanda o servidor das trancas antes de responder
  int code = requisicao("POST", "/api/tag/", payload, &resp, &siteBase, 12000);
  if (code != 200) {
    logHttp("tag", code);
    piscarLED(1, 600);
    return;
  }

  StaticJsonDocument<256> r;
  if (deserializeJson(r, resp)) {
    Serial.println("Resposta invalida do site.");
    piscarLED(1, 600);
    return;
  }
  const char* resultado = r["resultado"] | "";
  const char* mensagem = r["mensagem"] | "";
  Serial.printf("[Tag %s] %s: %s\n", uid.c_str(), resultado, mensagem);

  bool ok = strcmp(resultado, "cadastrada") == 0 || strcmp(resultado, "guardada") == 0 ||
            strcmp(resultado, "liberada") == 0;
  if (ok) piscarLED(2, 120);    // 2 piscadas rápidas = deu certo
  else    piscarLED(1, 600);    // 1 piscada longa = recusada/desconhecida
}

void verificarTag() {
  String uid = lerTag();
  if (uid.length() == 0) return;
  if (uid == ultimaTag && millis() - ultimaTagEm < RFID_REPETE_MS) return;   // leitura repetida
  ultimaTag = uid;

  Serial.printf("\n>>> Tag lida: %s\n", uid.c_str());
  enviarTag(uid);
  ultimaTagEm = millis();
}

// ═══ SETUP ══════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  pinMode(PINO_CONFIG, INPUT_PULLUP);

  // lê a API key salva na memória do ESP
  prefs.begin("kakatua", false);
  apiKey = prefs.getString("apikey", "");
  siteBase = prefs.getString("site", "");

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

  iniciarRFID();

  // Conecta no WiFi salvo. Se não houver (ou falhar), abre o portal de configuração.
  wm.addParameter(&paramApiKey);
  wm.addParameter(&paramSite);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);
  Serial.println("Conectando ao WiFi (ou abrindo o portal de configuracao)...");
  if (!wm.autoConnect(NOME_PORTAL, SENHA_PORTAL)) {
    Serial.println("Nao conectou. Reiniciando...");
    delay(1000);
    ESP.restart();
  }
  salvarConfig();
  WiFi.setAutoReconnect(true);

  Serial.println(" Conectado!");
  Serial.printf("Servidor: %s\n", API_BASE);
  if (siteBase.length() == 0) {
    Serial.println("ATENCAO: endereco do site nao configurado (leitor de tag desativado). Segure o BOOT por 3 s.");
  } else {
    Serial.printf("Site: %s\n", siteBase.c_str());
  }
  if (apiKey.length() == 0) {
    Serial.println("ATENCAO: API key nao configurada. Segure o BOOT por 3 s para abrir o portal.");
  } else {
    // para comparar com a chave do servidor sem mostrar a chave inteira
    unsigned int n = apiKey.length();
    Serial.printf("API key salva: %u caracteres, termina em \"%s\"\n",
                  n, apiKey.substring(n > 3 ? n - 3 : 0).c_str());
  }
  Serial.println("-------------------------------------");
  Serial.println("Aguardando deteccao por sensores ultrassonicos...");
  Serial.println("-------------------------------------");
}

// ═══ LOOP ═══════════════════════════════════════════════════════════════════

void loop() {
  verificarBotaoConfig();   // segurar o BOOT por 3 s abre o portal de configuração
  garantirWiFi();

  bool algumaOcupada = false;
  for (int i = 0; i < NUM_TRANCAS; i++) {
    atualizarTranca(trancas[i]);
    if (trancas[i].estado == OCUPADA) algumaOcupada = true;
  }
  digitalWrite(LED, algumaOcupada ? HIGH : LOW);

  verificarTag();             // leitor RFID: cadastra/guarda/libera pela tag
  sincronizarComServidor();   // avisa o servidor (com nova tentativa se falhar)
  verificarComandos();        // lê comandos do servidor e aciona as solenoides
  delay(50);
}
