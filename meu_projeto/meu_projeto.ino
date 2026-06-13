// ============================================================
// INCLUDES
// ============================================================
#include <WiFi.h> 
#include <WebServer.h> 
#include <NTPClient.h> 
#include <WiFiUDP.h> 
#include "DFRobotDFPlayerMini.h" 
#include <Wire.h> 
#include <Adafruit_GFX.h> 
#include <Adafruit_SSD1306.h> 
#include <LittleFS.h>
#include "esp_sleep.h"

// ============================================================
// CONFIGURAÇÕES GERAIS E PERFORMANCE
// ============================================================
char wifi_ssid_dinamico[32] = "CLARO_2G47948C";
char wifi_pass_dinamico[64] = "UwWz4AZpYN"; 

// char wifi_ssid_dinamico[32] = "Esp32";
// char wifi_pass_dinamico[64] = "testeEsp32"; 

// Parâmetros operacionais configuráveis via web
int  LOTACAO_MAX             = 100;
int  SENSOR_INTERVALO_MS     = 15;
int  COOLDOWN_MS_CONFIG      = 500;

// Variáveis de Profiling (Tempo de Execução em Microssegundos)
volatile uint32_t tempo_verificarPassagem = 0;
volatile uint32_t tempo_verificarEncoder = 0; 
volatile uint32_t tempo_atualizarDisplay = 0; 
volatile uint32_t tempo_salvarDados = 0; 
volatile uint32_t tempo_handleClient = 0;

// Cálculo Dinâmico de Uso de CPU (%)
volatile uint8_t uso_cpu_c1 = 0;
volatile uint8_t uso_cpu_total = 0;
uint32_t cpu_freq_mhz = 0; 
String status_wifi_ap = "Inicializando..."; 

// Handles das Tasks e Mutex do FreeRTOS
TaskHandle_t TaskSensoresHandle = NULL;
SemaphoreHandle_t mutexDados = NULL; // Proteção para histórico e contador
SemaphoreHandle_t mutexLogs  = NULL; // Proteção para array de logs

// ============================================================
// SENSOR IR — E18-D80NK (Sensores de passagem A e B)
// ============================================================
#define PINO_SENSOR_A 32 
#define PINO_SENSOR_B 33 
#define INTERVALO_MAX 2000 

enum EstadoSensor {
  IDLE,
  AGUARDANDO_B_APOS_A,
  AGUARDANDO_A_APOS_B,
  COOLDOWN
};

EstadoSensor  estadoSensor       = IDLE; 
unsigned long tempoMudancaEstado = 0;

// ============================================================
// SENSOR — ENCODER KY-040 (Catraca)
// ============================================================
#define ENCODER_CLK         18 
#define ENCODER_DT          19 
#define PASSOS_POR_CONTAGEM 7 

volatile int passoEncoder      = 0;
int          ultimoPasso       = 0;
int          estadoCLK_anterior = 0;

// ============================================================
// ATUADOR — DFPLAYER MINI (Audio)
// ============================================================
#define MP3_RX 26 
#define MP3_TX 27 

#define AUDIO_METADE_MAX  1
#define AUDIO_LOTACAO_MAX 2
#define AUDIO_PASTA       1

HardwareSerial      mp3Serial(1);
DFRobotDFPlayerMini dfplayer; 
bool                dfplayerOk = false;

// ============================================================
// ATUADOR — DISPLAY OLED SSD1306
// ============================================================
#define SCREEN_WIDTH   128 
#define SCREEN_HEIGHT  64 
#define OLED_RESET     -1 
#define OLED_ADDRESS   0x3C 
#define INTERVALO_TELA 5000 

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
int           telaAtual  = 0; 
unsigned long ultimaTela = 0;

// ============================================================
// SERVIDOR WEB + NTP
// ============================================================
WiFiUDP   ntpUDP; 
NTPClient ntp(ntpUDP, "pool.ntp.org", -3 * 3600); 
WebServer server(80);

// ============================================================
// LOGICA CENTRAL — Contador, Historico e Logs
// ============================================================
int contadorPessoas = 0; 

struct Registro {
  char horario[9];
  char tipo[8]; 
  char origem[10]; 
  int  total; 
  long timestampUnix; 
};

const int MAX_REGISTROS = 100; // Tamanho do buffer circular
Registro historico[MAX_REGISTROS];
int totalRegistros = 0; // Cresce continuamente (usado com módulo)

// ============================================================
// SISTEMA DE LOGS COM NÍVEL (ERROR / WARN / INFO)
// ============================================================
#define MAX_LOGS 100
#define LOG_INFO  "INFO"
#define LOG_WARN  "WARN"
#define LOG_ERROR "ERROR"

// Tempo de vida de um log: 24h. Apos isso e removido.
#define LOG_TTL_SEGUNDOS 86400L
#define ARQUIVO_LOGS     "/logs.json"

struct LogEntry {
  char nivel[6];
  char mensagem[120];
  long timestampUnix; // Epoch da ocorrencia. 0 = pendente (criado antes do NTP sincronizar)
};

LogEntry logsistema[MAX_LOGS];
int totalLogs = 0; 

unsigned long ultimoNtpUpdate  = 0; 
unsigned long ultimoSalvamento = 0;
#define INTERVALO_SALVAMENTO 30000  

// ------------------------------------------------------------
// Controle de sincronizacao NTP
// ------------------------------------------------------------
// EPOCH_VALIDO_MIN: qualquer epoch abaixo disso (09/09/2001) significa
// que o NTP ainda nao sincronizou de verdade. A lib NTPClient inicia o
// relogio em 0 (1970), o que com o offset -3h aparece como "03:00:00".
#define EPOCH_VALIDO_MIN        1000000000L
#define NTP_RETRY_INTERVAL_MS   5000     // Re-tentativa rapida enquanto NAO sincronizado
#define NTP_REFRESH_INTERVAL_MS 60000    // Re-sincronizacao periodica quando JA sincronizado

bool ntpSincronizado = false;            // true assim que recebemos um epoch valido

// ============================================================
// FUNCOES — NTP / HORARIO
// ============================================================

// Forward declaration: adicionarLog e definida mais abaixo (secao de
// logs), mas e usada aqui pelas funcoes de NTP. O argumento padrao fica
// AQUI (primeira declaracao vista pelo compilador) e NAO na definicao.
void adicionarLog(String mensagem, const char* nivel = LOG_INFO);
// carimbarLogsPendentes e definida na secao de manutencao de logos, mas
// e chamada aqui quando o NTP sincroniza.
void carimbarLogsPendentes();

// Tenta sincronizar agora. Usa forceUpdate() para enviar o pacote
// imediatamente (update() respeita o intervalo interno da lib e pode
// nao reenviar). Retorna true se obteve um epoch valido.
bool tentarSincronizarNtp() {
  if (WiFi.status() != WL_CONNECTED) return false; // Sem rede STA, NTP nao funciona

  ntp.forceUpdate();
  if (ntp.getEpochTime() >= EPOCH_VALIDO_MIN) {
    if (!ntpSincronizado) {
      ntpSincronizado = true;
      adicionarLog("NTP sincronizado: " + ntp.getFormattedTime());
      // Carimba retroativamente os logs de boot que ficaram com ts=0
      carimbarLogsPendentes();
    }
    return true;
  }
  return false;
}

String obterHorario() {
  // Enquanto nao sincronizou, devolve um marcador claro em vez do
  // falso "03:00:00" que enganava os registros e graficos.
  if (!ntpSincronizado) return "--:--:--";
  return ntp.getFormattedTime();
}

long obterEpoch() {
  long e = ntp.getEpochTime();
  if (e < EPOCH_VALIDO_MIN) return 0; // Epoch invalido -> 0 (filtrado pelos consumidores)
  return e; 
}

// ============================================================
// FUNCOES — LOGS COM NIVEL (Ring Buffer Thread-Safe)
// ============================================================
void adicionarLog(String mensagem, const char* nivel) {
  String entrada = "[" + obterHorario() + "] [" + String(nivel) + "] " + mensagem;
  Serial.println(entrada); 
  
  if (mutexLogs != NULL && xSemaphoreTake(mutexLogs, portMAX_DELAY)) {
    int idx = totalLogs % MAX_LOGS; // Sobrescreve o mais antigo se estourar limite
    strncpy(logsistema[idx].nivel, nivel, 5);
    logsistema[idx].nivel[5] = '\0';
    strncpy(logsistema[idx].mensagem, entrada.c_str(), 119);
    logsistema[idx].mensagem[119] = '\0';
    // obterEpoch() retorna 0 enquanto o NTP nao sincronizou; esses logs
    // serao carimbados depois, em carimbarLogsPendentes().
    logsistema[idx].timestampUnix = obterEpoch();
    totalLogs++; 
    xSemaphoreGive(mutexLogs);
  }
}

// ============================================================
// FUNCOES — PERSISTENCIA E MANUTENCAO DE LOGS
// ============================================================
// O caminho do arquivo (ARQUIVO_LOGS) e definido na secao de
// persistencia logo abaixo; estas funcoes o utilizam.

// Escapa aspas e barras invertidas para JSON valido.
static String escaparJson(const char* s) {
  String out;
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') out += '\\';
    out += *p;
  }
  return out;
}

// Assim que o NTP sincroniza, carimba retroativamente os logs que foram
// criados com timestamp 0 (durante o boot). Usa a hora atual como
// aproximacao — sao poucos segundos de diferenca do momento real.
void carimbarLogsPendentes() {
  long agora = obterEpoch();
  if (agora < EPOCH_VALIDO_MIN) return; // NTP ainda nao pronto

  if (mutexLogs != NULL && xSemaphoreTake(mutexLogs, portMAX_DELAY)) {
    int qtd = min(totalLogs, MAX_LOGS);
    for (int i = 0; i < qtd; i++) {
      if (logsistema[i].timestampUnix == 0) {
        logsistema[i].timestampUnix = agora;
      }
    }
    xSemaphoreGive(mutexLogs);
  }
}

// Remove logs com mais de 24h. Compacta o array mantendo a ordem
// cronologica. Logs pendentes (ts=0) nunca expiram aqui — so apos serem
// carimbados pelo NTP.
void expurgarLogsExpirados() {
  long agora = obterEpoch();
  if (agora < EPOCH_VALIDO_MIN) return; // Sem hora confiavel, nao expira nada

  if (mutexLogs != NULL && xSemaphoreTake(mutexLogs, portMAX_DELAY)) {
    int qtd = min(totalLogs, MAX_LOGS);

    // Reconstroi o array em ordem cronologica, descartando os expirados.
    // 'static' para NAO alocar ~13KB na stack da loopTask (estouraria).
    static LogEntry temp[MAX_LOGS];
    int novoTotal = 0;
    for (int i = 0; i < qtd; i++) {
      int idx = (totalLogs > MAX_LOGS) ? ((totalLogs + i) % MAX_LOGS) : i;
      long ts = logsistema[idx].timestampUnix;
      bool expirado = (ts != 0) && ((agora - ts) > LOG_TTL_SEGUNDOS);
      if (!expirado) {
        temp[novoTotal++] = logsistema[idx];
      }
    }
    for (int i = 0; i < novoTotal; i++) logsistema[i] = temp[i];
    totalLogs = novoTotal; // Reinicia a contagem linear (sem mais wrap-around pendente)

    xSemaphoreGive(mutexLogs);
  }
}

void salvarLogs() {
  File f = LittleFS.open(ARQUIVO_LOGS, "w");
  if (!f) { Serial.println("[Logs] Falha ao salvar logs."); return; }

  xSemaphoreTake(mutexLogs, portMAX_DELAY);
  int qtd = min(totalLogs, MAX_LOGS);

  f.print("[");
  bool primeiro = true;
  for (int i = 0; i < qtd; i++) {
    int idx = (totalLogs > MAX_LOGS) ? ((totalLogs + i) % MAX_LOGS) : i;
    if (!primeiro) f.print(",");
    primeiro = false;
    f.print("{\"n\":\""); f.print(logsistema[idx].nivel);
    f.print("\",\"m\":\""); f.print(escaparJson(logsistema[idx].mensagem));
    f.print("\",\"ts\":"); f.print(logsistema[idx].timestampUnix);
    f.print("}");
  }
  f.print("]");

  xSemaphoreGive(mutexLogs);
  f.close();
}

void carregarLogs() {
  if (!LittleFS.exists(ARQUIVO_LOGS)) return;
  File f = LittleFS.open(ARQUIVO_LOGS, "r");
  if (!f) return;
  String json = f.readString();
  f.close();

  long agora = obterEpoch();
  int pos = 0;
  int carregados = 0;

  xSemaphoreTake(mutexLogs, portMAX_DELAY);
  totalLogs = 0;

  while (carregados < MAX_LOGS) {
    int ini = json.indexOf("{\"n\":\"", pos);
    if (ini < 0) break;

    int nIni = ini + 6;
    int nFim = json.indexOf("\"", nIni);
    if (nFim < 0) break;
    String nivel = json.substring(nIni, nFim);

    int mIni = json.indexOf("\"m\":\"", nFim);
    if (mIni < 0) break;
    mIni += 5;
    // Encontra o fechamento da string de mensagem respeitando escapes
    int mFim = mIni;
    while (mFim < (int)json.length()) {
      if (json[mFim] == '\\') { mFim += 2; continue; }
      if (json[mFim] == '"') break;
      mFim++;
    }
    String msg = json.substring(mIni, mFim);
    msg.replace("\\\"", "\"");
    msg.replace("\\\\", "\\");

    int tsIni = json.indexOf("\"ts\":", mFim);
    if (tsIni < 0) break;
    tsIni += 5;
    long ts = json.substring(tsIni).toInt();

    pos = json.indexOf("}", tsIni);
    if (pos < 0) break;
    pos++;

    // Descarta ja na carga se estiver expirado (com hora confiavel)
    if (agora >= EPOCH_VALIDO_MIN && ts != 0 && (agora - ts) > LOG_TTL_SEGUNDOS) {
      continue;
    }

    int idx = totalLogs % MAX_LOGS;
    strncpy(logsistema[idx].nivel, nivel.c_str(), 5);
    logsistema[idx].nivel[5] = '\0';
    strncpy(logsistema[idx].mensagem, msg.c_str(), 119);
    logsistema[idx].mensagem[119] = '\0';
    logsistema[idx].timestampUnix = ts;
    totalLogs++;
    carregados++;
  }
  xSemaphoreGive(mutexLogs);
}

// ============================================================
// FUNCOES — PERSISTENCIA (LittleFS)
// ============================================================
#define ARQUIVO_DADOS    "/dados.json" 
#define ARQUIVO_CONFIG   "/config.json"
#define ARQUIVO_GRAFICOS "/graficos.json"

// ============================================================
// ACUMULADORES PERSISTENTES DOS GRAFICOS
// ------------------------------------------------------------
// Diferente do buffer circular de 100 registros, estes arrays
// guardam totais historicos que NAO sao sobrescritos. Eles sao
// incrementados a cada passagem e salvos em /graficos.json,
// sobrevivendo a reboot / reupload de codigo.
// ============================================================
int  acumEntradasPorHora[24] = {0}; // Entradas acumuladas por hora do dia (0-23)
int  acumSaidasPorHora[24]   = {0}; // Saidas acumuladas por hora do dia (0-23)
int  acumPorDia[30]          = {0}; // Movimento total dos ultimos 30 dias (janela deslizante)
long graficosUltimoDiaEpoch  = 0;   // Epoch (alinhado a meia-noite) do ultimo dia contabilizado em acumPorDia[29]

void salvarConfig() {
  File f = LittleFS.open(ARQUIVO_CONFIG, "w");
  if (!f) { adicionarLog("Falha ao salvar config.", LOG_ERROR); return; }
  f.print("{\"lotacao_max\":");  f.print(LOTACAO_MAX);
  f.print(",\"sensor_intervalo_ms\":"); f.print(SENSOR_INTERVALO_MS);
  f.print(",\"cooldown_ms\":"); f.print(COOLDOWN_MS_CONFIG);
  f.print("}");
  f.close();
  adicionarLog("Configuracao salva.");
}

void carregarConfig() {
  if (!LittleFS.exists(ARQUIVO_CONFIG)) return;
  File f = LittleFS.open(ARQUIVO_CONFIG, "r");
  if (!f) return;
  String json = f.readString(); f.close();

  int idx;
  idx = json.indexOf("\"lotacao_max\":");
  if (idx >= 0) LOTACAO_MAX = json.substring(idx + 14).toInt();

  idx = json.indexOf("\"sensor_intervalo_ms\":");
  if (idx >= 0) SENSOR_INTERVALO_MS = json.substring(idx + 22).toInt();

  idx = json.indexOf("\"cooldown_ms\":");
  if (idx >= 0) COOLDOWN_MS_CONFIG = json.substring(idx + 14).toInt();

  adicionarLog("Configuracao carregada: lotacao=" + String(LOTACAO_MAX) +
               ", intervalo=" + String(SENSOR_INTERVALO_MS) + "ms");
}

void salvarDados() {
  File f = LittleFS.open(ARQUIVO_DADOS, "w"); 
  if (!f) {
    adicionarLog("Nao foi possivel salvar dados.", LOG_ERROR);
    return;
  }

  xSemaphoreTake(mutexDados, portMAX_DELAY);
  int qtd = min(totalRegistros, MAX_REGISTROS);
  
  f.print("{\"contador\":"); 
  f.print(contadorPessoas); 
  f.print(",\"total\":"); 
  f.print(qtd); 
  f.print(",\"registros\":["); 

  for (int i = 0; i < qtd; i++) { 
    if (i > 0) f.print(",");
    
    // Calcula o índice real garantindo ordem cronológica (do mais antigo para o novo)
    int idx = (totalRegistros > MAX_REGISTROS) ? ((totalRegistros + i) % MAX_REGISTROS) : i;
    
    f.print("{\"h\":\"");   f.print(historico[idx].horario); 
    f.print("\",\"t\":\""); f.print(historico[idx].tipo); 
    f.print("\",\"o\":\""); f.print(historico[idx].origem); 
    f.print("\",\"v\":");   f.print(historico[idx].total); 
    f.print(",\"ts\":");    f.print(historico[idx].timestampUnix); 
    f.print("}"); 
  }
  xSemaphoreGive(mutexDados);

  f.print("]}"); 
  f.close(); 
  adicionarLog("Dados salvos (" + String(qtd) + " registros).");
}

void carregarDados() {
  if (!LittleFS.exists(ARQUIVO_DADOS)) { 
    adicionarLog("Nenhum dado salvo. Iniciando zerado."); 
    return;
  }

  File f = LittleFS.open(ARQUIVO_DADOS, "r"); 
  if (!f) {
    adicionarLog("Nao foi possivel ler dados salvos.", LOG_ERROR);
    return;
  }

  String json = f.readString(); 
  f.close(); 

  int idx = json.indexOf("\"contador\":");
  if (idx >= 0) contadorPessoas = json.substring(idx + 11).toInt(); 

  idx = json.indexOf("\"total\":");
  if (idx >= 0) {
    int lido = json.substring(idx + 8).toInt();
    totalRegistros = min(lido, MAX_REGISTROS); 
  }

  int pos = json.indexOf("\"registros\":[");
  if (pos < 0) { adicionarLog("Formato de dados invalido.", LOG_ERROR); return; }
  pos += 13;

  for (int i = 0; i < totalRegistros; i++) { 
    int ini, fim;
    ini = json.indexOf("\"h\":\"", pos) + 5;
    fim = json.indexOf("\"", ini); 
    json.substring(ini, fim).toCharArray(historico[i].horario, 9); 
    historico[i].horario[8] = '\0';
    
    ini = json.indexOf("\"t\":\"", pos) + 5;
    fim = json.indexOf("\"", ini); 
    json.substring(ini, fim).toCharArray(historico[i].tipo, 8); 
    historico[i].tipo[7] = '\0';
    
    ini = json.indexOf("\"o\":\"", pos) + 5;
    fim = json.indexOf("\"", ini); 
    json.substring(ini, fim).toCharArray(historico[i].origem, 10); 
    historico[i].origem[9] = '\0';
    
    ini = json.indexOf("\"v\":", pos) + 4;
    historico[i].total = json.substring(ini).toInt(); 

    ini = json.indexOf("\"ts\":", pos) + 5;
    historico[i].timestampUnix = json.substring(ini).toInt();
    
    pos = json.indexOf("}", pos) + 1; 
  }

  adicionarLog("Dados carregados: " + String(totalRegistros) + " registros. Contador: " + String(contadorPessoas));
}

// ============================================================
// FUNCOES — PERSISTENCIA DOS GRAFICOS (acumuladores historicos)
// ============================================================

// Trunca um epoch para o inicio do dia (meia-noite UTC). Usado para
// detectar virada de dia e rotacionar o array acumPorDia[].
long inicioDoDia(long epoch) {
  return (epoch / 86400L) * 86400L;
}

// Verifica se o dia virou desde o ultimo registro e desloca a janela
// de 30 dias, abrindo espaco (zerado) para o novo dia no indice 29.
// Deve ser chamada com o mutexDados JA adquirido.
void rotacionarDiasSeNecessario(long epochAtual) {
  if (epochAtual < 1000000000L) return; // NTP ainda nao sincronizou; nao rotaciona

  long diaAtual = inicioDoDia(epochAtual);

  // Primeira inicializacao
  if (graficosUltimoDiaEpoch == 0) {
    graficosUltimoDiaEpoch = diaAtual;
    return;
  }

  long diffDias = (diaAtual - graficosUltimoDiaEpoch) / 86400L;
  if (diffDias <= 0) return; // mesmo dia, nada a fazer

  if (diffDias >= 30) {
    // Passou tempo demais: zera tudo
    for (int i = 0; i < 30; i++) acumPorDia[i] = 0;
  } else {
    // Desloca a esquerda 'diffDias' posicoes, zerando os dias novos
    for (int i = 0; i < 30; i++) {
      acumPorDia[i] = (i + diffDias < 30) ? acumPorDia[i + diffDias] : 0;
    }
  }
  graficosUltimoDiaEpoch = diaAtual;
}

void salvarGraficos() {
  File f = LittleFS.open(ARQUIVO_GRAFICOS, "w");
  if (!f) { adicionarLog("Nao foi possivel salvar graficos.", LOG_ERROR); return; }

  xSemaphoreTake(mutexDados, portMAX_DELAY);

  f.print("{\"ultimoDia\":"); f.print(graficosUltimoDiaEpoch);

  f.print(",\"entradasPorHora\":[");
  for (int i = 0; i < 24; i++) { if (i > 0) f.print(","); f.print(acumEntradasPorHora[i]); }

  f.print("],\"saidasPorHora\":[");
  for (int i = 0; i < 24; i++) { if (i > 0) f.print(","); f.print(acumSaidasPorHora[i]); }

  f.print("],\"porDia\":[");
  for (int i = 0; i < 30; i++) { if (i > 0) f.print(","); f.print(acumPorDia[i]); }

  f.print("]}");

  xSemaphoreGive(mutexDados);
  f.close();
}

// Le um array de inteiros de um JSON simples a partir de 'pos'.
// Retorna a posicao logo apos o ']' lido.
static int lerArrayInt(const String& json, int pos, int* destino, int n) {
  int p = json.indexOf('[', pos);
  if (p < 0) return pos;
  p++;
  for (int i = 0; i < n; i++) {
    while (p < (int)json.length() && (json[p] == ' ' || json[p] == ',')) p++;
    destino[i] = json.substring(p).toInt();
    int virg = json.indexOf(',', p);
    int fech = json.indexOf(']', p);
    if (fech < 0) break;
    if (virg < 0 || virg > fech) { p = fech + 1; break; }
    p = virg + 1;
  }
  int fim = json.indexOf(']', pos);
  return (fim >= 0) ? fim + 1 : p;
}

void carregarGraficos() {
  if (!LittleFS.exists(ARQUIVO_GRAFICOS)) {
    adicionarLog("Nenhum grafico salvo. Iniciando zerado.");
    return;
  }
  File f = LittleFS.open(ARQUIVO_GRAFICOS, "r");
  if (!f) { adicionarLog("Nao foi possivel ler graficos salvos.", LOG_ERROR); return; }

  String json = f.readString();
  f.close();

  int idx = json.indexOf("\"ultimoDia\":");
  if (idx >= 0) graficosUltimoDiaEpoch = json.substring(idx + 12).toInt();

  int pos = json.indexOf("\"entradasPorHora\":");
  if (pos >= 0) lerArrayInt(json, pos, acumEntradasPorHora, 24);

  pos = json.indexOf("\"saidasPorHora\":");
  if (pos >= 0) lerArrayInt(json, pos, acumSaidasPorHora, 24);

  pos = json.indexOf("\"porDia\":");
  if (pos >= 0) lerArrayInt(json, pos, acumPorDia, 30);

  adicionarLog("Graficos historicos carregados (persistencia restaurada).");
}

// ============================================================
// FUNCOES — DFPLAYER MINI (Audio)
// ============================================================
void tocarAudio(int faixa) {
  if (!dfplayerOk) return; 
  dfplayer.playFolder(AUDIO_PASTA, faixa);
  adicionarLog("Tocando faixa: " + String(faixa)); 
}

// ============================================================
// FUNCOES — LOGICA CENTRAL (Registros e Contagem)
// ============================================================
int contarEntradasUltimaHora() {
  int total = 0;
  long agora = obterEpoch(); 
  
  xSemaphoreTake(mutexDados, portMAX_DELAY);
  int qtd = min(totalRegistros, MAX_REGISTROS);
  for (int i = 0; i < qtd; i++) { 
    int idx = (totalRegistros > MAX_REGISTROS) ? ((totalRegistros + i) % MAX_REGISTROS) : i;
    if (strcmp(historico[idx].tipo, "ENTRADA") == 0 && (agora - historico[idx].timestampUnix) <= 3600L) {
      total++;
    }
  }
  xSemaphoreGive(mutexDados);
  return total; 
}

int contarSaidasUltimaHora() {
  int total = 0; 
  long agora = obterEpoch();
  
  xSemaphoreTake(mutexDados, portMAX_DELAY);
  int qtd = min(totalRegistros, MAX_REGISTROS);
  for (int i = 0; i < qtd; i++) { 
    int idx = (totalRegistros > MAX_REGISTROS) ? ((totalRegistros + i) % MAX_REGISTROS) : i;
    if (strcmp(historico[idx].tipo, "SAIDA") == 0 && (agora - historico[idx].timestampUnix) <= 3600L) {
      total++;
    }
  }
  xSemaphoreGive(mutexDados);
  return total; 
}

float calcularPorcentagem() {
  return ((float)contadorPessoas / LOTACAO_MAX) * 100.0;
}

// Ring Buffer Thread-Safe
void registrarPassagem(bool entrada, const char* origem) {
  int total_local = 0;
  int acaoAudio = 0; // 0 = nenhum, 1 = metade, 2 = lotacao max
  
  if (xSemaphoreTake(mutexDados, portMAX_DELAY)) {
    if (entrada) {
      contadorPessoas++;
    } else {
      if (contadorPessoas > 0) {
        contadorPessoas--;
      }
    }
    
    total_local = contadorPessoas;
    
    String horario = obterHorario();
    String tipo    = entrada ? "ENTRADA" : "SAIDA";

    // Apenas define qual audio deve tocar se for uma ENTRADA
    if (entrada) { 
      if (contadorPessoas == LOTACAO_MAX) {
        acaoAudio = AUDIO_LOTACAO_MAX;
      } else if (contadorPessoas == LOTACAO_MAX / 2) {
        acaoAudio = AUDIO_METADE_MAX;
      }
    }

    int idx = totalRegistros % MAX_REGISTROS;
    strncpy(historico[idx].horario, horario.c_str(), 8);
    strncpy(historico[idx].tipo,    tipo.c_str(),    7); 
    strncpy(historico[idx].origem,  origem,          9);
    historico[idx].horario[8]    = '\0'; 
    historico[idx].tipo[7]       = '\0';
    historico[idx].origem[9]     = '\0'; 
    historico[idx].total         = contadorPessoas;
    historico[idx].timestampUnix = obterEpoch(); 
    totalRegistros++; 

    // --- Atualiza acumuladores persistentes dos graficos ---
    long epochAgora = historico[idx].timestampUnix;
    // So contabiliza nos graficos se o NTP ja estava sincronizado
    // (epoch valido e horario "HH:MM:SS" real, nao o marcador "--:--:--").
    if (epochAgora >= EPOCH_VALIDO_MIN) {
      rotacionarDiasSeNecessario(epochAgora);

      int hora = (horario[0] - '0') * 10 + (horario[1] - '0');
      if (hora >= 0 && hora < 24) {
        if (entrada) acumEntradasPorHora[hora]++;
        else         acumSaidasPorHora[hora]++;
      }
      // Toda passagem conta como movimento do dia atual (indice 29)
      acumPorDia[29]++;
    }
    // -------------------------------------------------------
    
    xSemaphoreGive(mutexDados);
  }
  
  // EXECUÇÃO DO ÁUDIO: Feita de forma totalmente segura FORA do Mutex crítico
  if (acaoAudio == AUDIO_LOTACAO_MAX) {
    tocarAudio(AUDIO_LOTACAO_MAX);
  } else if (acaoAudio == AUDIO_METADE_MAX) {
    adicionarLog("Lotacao atingiu 50%.", LOG_WARN);
    tocarAudio(AUDIO_METADE_MAX);
  }
  
  String act = entrada ? "ENTRADA" : "SAIDA";
  adicionarLog(act + " (" + String(origem) + ") - Total: " + String(total_local));
}

// ============================================================
// FUNCOES — SENSOR IR E18-D80NK
// ============================================================
void verificarPassagem() {
  bool sensorA = digitalRead(PINO_SENSOR_A) == LOW;
  bool sensorB = digitalRead(PINO_SENSOR_B) == LOW; 
  unsigned long agora = millis();
  
  switch (estadoSensor) { 
    case IDLE:
      if (sensorA && !sensorB) {
        estadoSensor = AGUARDANDO_B_APOS_A;
        tempoMudancaEstado = agora; 
      } else if (sensorB && !sensorA) {
        estadoSensor = AGUARDANDO_A_APOS_B;
        tempoMudancaEstado = agora; 
      }
      break;
      
    case AGUARDANDO_B_APOS_A:
      if (sensorB) {
        registrarPassagem(true, "SENSOR");
        estadoSensor = COOLDOWN; 
        tempoMudancaEstado = agora;
      } else if (agora - tempoMudancaEstado > INTERVALO_MAX) {
        estadoSensor = IDLE;
      }
      break;

    case AGUARDANDO_A_APOS_B:
      if (sensorA) {
        registrarPassagem(false, "SENSOR");
        estadoSensor = COOLDOWN; 
        tempoMudancaEstado = agora;
      } else if (agora - tempoMudancaEstado > INTERVALO_MAX) {
        estadoSensor = IDLE;
      }
      break;

    case COOLDOWN:
      if (!sensorA && !sensorB && agora - tempoMudancaEstado > (unsigned long)COOLDOWN_MS_CONFIG) {
        estadoSensor = IDLE;
      }
      break;
  }
}

// ============================================================
// FUNCOES — ENCODER KY-040
// ============================================================
void IRAM_ATTR handleEncoder() {
  int clk = digitalRead(ENCODER_CLK);
  int dt  = digitalRead(ENCODER_DT); 
  if (clk != estadoCLK_anterior) { 
    if (dt != clk) passoEncoder++;
    else           passoEncoder--; 
    estadoCLK_anterior = clk;
  }
}

void verificarEncoder() {
  int delta = passoEncoder - ultimoPasso;
  if (delta >= PASSOS_POR_CONTAGEM) { 
    ultimoPasso = passoEncoder; 
    registrarPassagem(true, "ENCODER");
  } else if (delta <= -PASSOS_POR_CONTAGEM) { 
    ultimoPasso = passoEncoder; 
    registrarPassagem(false, "ENCODER");
  }
}

// ============================================================
// FUNCOES — DISPLAY OLED
// ============================================================
void atualizarDisplay() {
  if (millis() - ultimaTela < INTERVALO_TELA) return; 
  ultimaTela = millis();
  oled.clearDisplay(); 
  oled.setTextColor(SSD1306_WHITE); 

  int entradas = contarEntradasUltimaHora();
  int saidas   = contarSaidasUltimaHora();
  
  xSemaphoreTake(mutexDados, portMAX_DELAY);
  int total_pessoas = contadorPessoas;
  float pct = calcularPorcentagem();
  xSemaphoreGive(mutexDados);

  switch (telaAtual) { 
    case 0:
      oled.setTextSize(1); oled.setCursor(0, 0);
      oled.println("Total atual:"); 
      oled.setTextSize(3); oled.setCursor(0, 20); oled.println(total_pessoas); 
      break;
    case 1:
      oled.setTextSize(1); oled.setCursor(0, 0); oled.println("Entradas (1h):"); 
      oled.setTextSize(3); oled.setCursor(0, 20); oled.println(entradas); 
      break;
    case 2:
      oled.setTextSize(1); oled.setCursor(0, 0); oled.println("Saidas (1h):"); 
      oled.setTextSize(3); oled.setCursor(0, 20); oled.println(saidas);
      break;
    case 3: 
      oled.setTextSize(1); oled.setCursor(0, 0); oled.println("Lotacao:"); 
      oled.setTextSize(3); oled.setCursor(0, 20); 
      oled.print(pct, 1); oled.println("%"); 
      break;
  }

  for (int i = 0; i < 4; i++) { 
    if (i == telaAtual) oled.fillCircle(52 + i * 10, 58, 3, SSD1306_WHITE);
    else                oled.drawCircle(52 + i * 10, 58, 3, SSD1306_WHITE);
  }

  oled.display(); 
  telaAtual = (telaAtual + 1) % 4;
}

// ============================================================
// FUNCOES — SERVIDOR WEB (handlers)
// ============================================================
void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) { server.send(404, "text/plain", "index.html nao encontrado"); return; } 
  server.streamFile(file, "text/html"); 
  file.close();
}

void handleStaticFile(const char* path, const char* contentType) {
  File file = LittleFS.open(path, "r");
  if (!file) { server.send(404, "text/plain", "Arquivo nao encontrado"); return; } 
  server.streamFile(file, contentType); 
  file.close();
}

void handleApiStatus() {
  int contagemPorHora[24] = {0}; 
  long agora = obterEpoch();
  
  xSemaphoreTake(mutexDados, portMAX_DELAY);
  int total_atual = contadorPessoas;
  float pct_atual = calcularPorcentagem();
  int qtd = min(totalRegistros, MAX_REGISTROS);
  
  for (int i = 0; i < qtd; i++) { 
    int idx = (totalRegistros > MAX_REGISTROS) ? ((totalRegistros + i) % MAX_REGISTROS) : i;
    if ((agora - historico[idx].timestampUnix) <= 86400L) { 
      int hora = (historico[idx].horario[0] - '0') * 10 + (historico[idx].horario[1] - '0');
      if (hora >= 0 && hora < 24) contagemPorHora[hora]++; 
    }
  }
  xSemaphoreGive(mutexDados);

  int horaMaisCheia = 0;
  for (int i = 1; i < 24; i++) { 
    if (contagemPorHora[i] > contagemPorHora[horaMaisCheia]) horaMaisCheia = i;
  }

  String json = "{"; 
  json += "\"total\":"         + String(total_atual)                + ",";
  json += "\"entradas1h\":"    + String(contarEntradasUltimaHora()) + ",";
  json += "\"saidas1h\":"      + String(contarSaidasUltimaHora())   + ",";
  json += "\"porcentagem\":"   + String(pct_atual, 1)               + ",";
  json += "\"lotacaoMax\":"    + String(LOTACAO_MAX)                + ",";
  json += "\"horaMaisCheia\":" + String(horaMaisCheia); 
  json += "}"; 
  server.send(200, "application/json", json); 
}

void handleApiHistorico() {
  String json = "[";
  xSemaphoreTake(mutexDados, portMAX_DELAY);
  int qtd = min(totalRegistros, MAX_REGISTROS);
  
  for (int i = 0; i < qtd; i++) { 
    if (i > 0) json += ",";
    int idx = (totalRegistros > MAX_REGISTROS) ? ((totalRegistros + i) % MAX_REGISTROS) : i;
    json += "{"; 
    json += "\"horario\":\"" + String(historico[idx].horario) + "\",";
    json += "\"tipo\":\""    + String(historico[idx].tipo)    + "\",";
    json += "\"origem\":\""  + String(historico[idx].origem)  + "\","; 
    json += "\"total\":"     + String(historico[idx].total);
    json += "}"; 
  }
  xSemaphoreGive(mutexDados);
  
  json += "]"; 
  server.send(200, "application/json", json);
}

void handleApiLogs() {
  String json = "[";
  xSemaphoreTake(mutexLogs, portMAX_DELAY);
  int qtd = min(totalLogs, MAX_LOGS);
  
  for (int i = 0; i < qtd; i++) { 
    if (i > 0) json += ",";
    int idx = (totalLogs > MAX_LOGS) ? ((totalLogs + i) % MAX_LOGS) : i;
    json += "{\"nivel\":\""; json += logsistema[idx].nivel; 
    json += "\",\"msg\":\"";
    String msg = String(logsistema[idx].mensagem);
    msg.replace("\"", "\\\"");
    json += msg;
    json += "\"}";
  }
  xSemaphoreGive(mutexLogs);
  
  json += "]"; 
  server.send(200, "application/json", json); 
}

void handleExportarLogs() {
  String content = "=== LOGS DO SISTEMA ANALISTA DE FLUXO ===\n";
  content += "Exportado em: " + obterHorario() + "\n";
  
  xSemaphoreTake(mutexLogs, portMAX_DELAY);
  int qtd = min(totalLogs, MAX_LOGS);
  content += "Total de registros armazenados: " + String(qtd) + "\n";
  content += "==========================================\n\n";
  
  for (int i = 0; i < qtd; i++) {
    int idx = (totalLogs > MAX_LOGS) ? ((totalLogs + i) % MAX_LOGS) : i;
    content += String(logsistema[idx].mensagem) + "\n";
  }
  xSemaphoreGive(mutexLogs);
  
  server.sendHeader("Content-Disposition", "attachment; filename=\"logs_sistema.txt\"");
  server.send(200, "text/plain", content);
  adicionarLog("Logs exportados via interface web.");
}

void handleApiGraficos() {
  // Agora os graficos refletem acumuladores HISTORICOS persistidos em
  // /graficos.json, e nao mais uma janela do buffer circular. Eles
  // sobrevivem a reboot / reupload de codigo.
  xSemaphoreTake(mutexDados, portMAX_DELAY);

  // Garante que a janela de dias esteja alinhada ao dia atual mesmo
  // que nenhuma passagem tenha ocorrido desde o ultimo boot.
  rotacionarDiasSeNecessario(obterEpoch());

  String json = "{\"porHora\":{\"entradas\":[";
  for (int i = 0; i < 24; i++) { if (i > 0) json += ","; json += String(acumEntradasPorHora[i]); }
  json += "],\"saidas\":[";
  for (int i = 0; i < 24; i++) { if (i > 0) json += ","; json += String(acumSaidasPorHora[i]); }
  json += "]},\"porDia\":[";
  for (int i = 0; i < 30; i++) { if (i > 0) json += ","; json += String(acumPorDia[i]); }
  json += "]}";

  xSemaphoreGive(mutexDados);
  server.send(200, "application/json", json);
}

// ============================================================
// METRICAS DE PERFORMANCE & MEMORIA DETALHADA
// ============================================================
void handleApiPerformance() {
  uint32_t free_heap    = ESP.getFreeHeap();
  uint32_t flash_size   = ESP.getFlashChipSize();
  uint32_t psram_size   = ESP.getPsramSize();
  uint32_t psram_livre  = ESP.getFreePsram();

  String json = "{";
  json += "\"cpu_freq\":"      + String(cpu_freq_mhz)    + ",";
  json += "\"uso_cpu\":"       + String(uso_cpu_total)   + ",";
  json += "\"heap_livre\":"    + String(free_heap)       + ",";
  json += "\"heap_total\":"    + String(ESP.getHeapSize()) + ",";
  json += "\"flash_total\":"   + String(flash_size)      + ",";
  
  if (psram_size > 0) {
    json += "\"psram_detectada\":true,";
    json += "\"psram_total\":"  + String(psram_size)   + ",";
    json += "\"psram_livre\":"  + String(psram_livre)  + ",";
  } else {
    json += "\"psram_detectada\":false,";
    json += "\"psram_total\":0,";
    json += "\"psram_livre\":0,";
  }
  json += "\"wifi_status\":\"" + status_wifi_ap + "\","; 
  
  json += "\"tempos_funcoes\":{";
  json += "\"verificarPassagem\":" + String(tempo_verificarPassagem) + ",";
  json += "\"verificarEncoder\":"  + String(tempo_verificarEncoder)  + ",";
  json += "\"atualizarDisplay\":"  + String(tempo_atualizarDisplay)  + ",";
  json += "\"salvarDados\":"       + String(tempo_salvarDados)       + ",";
  json += "\"handleClient\":"      + String(tempo_handleClient);
  json += "},";

  json += "\"tasks\":[";
  json += "{\"nome\":\"Core0_Web_Server\",\"core\":0,\"prioridade\":1,\"stack_min_garantido\":" + String(uxTaskGetStackHighWaterMark(NULL)) + "},";
  if (TaskSensoresHandle != NULL) {
    json += "{\"nome\":\"Core1_TaskSensores\",\"core\":1,\"prioridade\":2,\"stack_min_garantido\":" + String(uxTaskGetStackHighWaterMark(TaskSensoresHandle)) + "}";
  } else {
    json += "{\"nome\":\"Core1_TaskSensores\",\"core\":1,\"prioridade\":2,\"stack_min_garantido\":0}";
  }
  json += "],";
  json += "\"energia_modo\":\"Light Sleep (Timer Wakeup via FreeRTOS Yield)\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleApiGetConfig() {
  String json = "{";
  json += "\"lotacao_max\":"         + String(LOTACAO_MAX)          + ",";
  json += "\"sensor_intervalo_ms\":" + String(SENSOR_INTERVALO_MS)  + ",";
  json += "\"cooldown_ms\":"         + String(COOLDOWN_MS_CONFIG);
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiSetConfig() {
  bool alterado = false;
  if (server.hasArg("lotacao_max")) {
    int novaLotacao = server.arg("lotacao_max").toInt();
    if (novaLotacao > 0 && novaLotacao <= 10000) {
      LOTACAO_MAX = novaLotacao;
      alterado = true;
      adicionarLog("Lotacao maxima alterada para " + String(LOTACAO_MAX));
    } else {
      adicionarLog("Valor invalido para lotacao_max: " + server.arg("lotacao_max"), LOG_WARN);
    }
  }

  if (server.hasArg("sensor_intervalo_ms")) {
    int novoIntervalo = server.arg("sensor_intervalo_ms").toInt();
    if (novoIntervalo >= 5 && novoIntervalo <= 5000) {
      SENSOR_INTERVALO_MS = novoIntervalo;
      alterado = true;
      adicionarLog("Frequencia de leitura alterada para " + String(SENSOR_INTERVALO_MS) + " ms");
    } else {
      adicionarLog("Valor invalido para sensor_intervalo_ms.", LOG_WARN);
    }
  }

  if (server.hasArg("cooldown_ms")) {
    int novoCooldown = server.arg("cooldown_ms").toInt();
    if (novoCooldown >= 100 && novoCooldown <= 10000) {
      COOLDOWN_MS_CONFIG = novoCooldown;
      alterado = true;
      adicionarLog("Cooldown alterado para " + String(COOLDOWN_MS_CONFIG) + " ms");
    } else {
      adicionarLog("Valor invalido para cooldown_ms.", LOG_WARN);
    }
  }

  if (alterado) {
    salvarConfig();
    server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"Parametros atualizados.\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"erro\",\"msg\":\"Nenhum parametro valido recebido.\"}");
  }
}

void handleConfigWifi() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String novo_ssid = server.arg("ssid");
    String nova_senha = server.arg("password");
    novo_ssid.toCharArray(wifi_ssid_dinamico, 32);
    nova_senha.toCharArray(wifi_pass_dinamico, 64);
    
    adicionarLog("Configuracao de Wi-Fi alterada via Web.");
    server.send(200, "text/plain", "Sucesso! Trocando de rede de forma assincrona.");
    
    WiFi.disconnect();
    WiFi.begin(wifi_ssid_dinamico, wifi_pass_dinamico);
  } else {
    server.send(400, "text/plain", "Parametros invalidos.");
  }
}

// ============================================================
// WIFI E SERVIDOR (Fluxo Assíncrono Sem Delays)
// ============================================================
bool conectarWiFi() {
  WiFi.mode(WIFI_AP_STA); 
  WiFi.begin(wifi_ssid_dinamico, wifi_pass_dinamico); 
  Serial.println("Iniciando conexao assincrona...");
  WiFi.softAP("ESP32_Acesso_Performance", "12345678");
  status_wifi_ap = "AP Ativo (ESP32_Acesso_Performance) | Conectando STA...";
  return true;
}

void verificarConexao() {
  static unsigned long ultimaChecagemWifi = 0;
  static bool jaPrintouIP = false;
  
  if (millis() - ultimaChecagemWifi > 10000) { 
    ultimaChecagemWifi = millis();
    if (WiFi.status() == WL_CONNECTED) {
      status_wifi_ap = "STA Conectado (" + WiFi.localIP().toString() + ") | AP Ativo";
      if (!jaPrintouIP) {
        Serial.println("\n[WiFi] Conectado com sucesso!");
        Serial.print("[WiFi] IP do Servidor Web: ");
        Serial.println(WiFi.localIP());
        jaPrintouIP = true;
      }
    } else {
      status_wifi_ap = "STA Desconectado | AP Ativo (Contingencia)";
      WiFi.begin(wifi_ssid_dinamico, wifi_pass_dinamico);
      jaPrintouIP = false;
    }
  }
}

void iniciarServidor() {
  conectarWiFi();
  if (!LittleFS.begin()) { 
    Serial.println("Erro ao montar LittleFS."); 
  } else {
    Serial.println("LittleFS montado.");
  }

  ntp.begin(); 

  // Aguarda ate ~8s pela conexao STA antes da primeira sincronizacao.
  // O conectarWiFi() inicia a conexao de forma assincrona; o NTP so
  // funciona depois que o STA realmente associar.
  Serial.print("[NTP] Aguardando WiFi para sincronizar");
  unsigned long inicioEspera = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicioEspera < 8000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  // Tenta sincronizar agora; se falhar, o loop continua tentando a cada 5s.
  for (int i = 0; i < 5 && !tentarSincronizarNtp(); i++) {
    delay(500);
  }
  ultimoNtpUpdate = millis(); 

  if (!ntpSincronizado) {
    adicionarLog("NTP ainda nao sincronizado no boot. Retentando em background.", LOG_WARN);
  }

  carregarConfig();
  carregarDados();
  carregarGraficos();
  carregarLogs();
  // Os logs de boot ja podem ter sido criados com ts=0; se o NTP ja
  // sincronizou ate aqui, carimba-os com a hora real e remove expirados.
  carimbarLogsPendentes();
  expurgarLogsExpirados();
  
  server.on("/",                 handleRoot);
  server.on("/membros.html",     []() { handleStaticFile("/membros.html",     "text/html"); });
  server.on("/graficos.html",    []() { handleStaticFile("/graficos.html",    "text/html"); });
  server.on("/logs.html",        []() { handleStaticFile("/logs.html",        "text/html"); });
  server.on("/performance.html", []() { handleStaticFile("/performance.html", "text/html"); }); 
  server.on("/configuracao.html",[]() { handleStaticFile("/configuracao.html","text/html"); });
  server.on("/changelog.html",   []() { handleStaticFile("/changelog.html",   "text/html"); });
  server.on("/style.css",        []() { handleStaticFile("/style.css",        "text/css");  });
  server.serveStatic("/img", LittleFS, "/img");
  
  server.on("/api/status",       handleApiStatus); 
  server.on("/api/historico",    handleApiHistorico);
  server.on("/api/logs",         handleApiLogs);
  server.on("/api/logs/export",  handleExportarLogs);
  server.on("/api/graficos",     handleApiGraficos);
  server.on("/api/performance",  handleApiPerformance);
  server.on("/api/config_wifi",  handleConfigWifi);
  server.on("/api/config",       HTTP_GET,  handleApiGetConfig);
  server.on("/api/config",       HTTP_POST, handleApiSetConfig);

  server.on("/api/upload_dados", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      File f = LittleFS.open("/dados.json", "w");
      f.print(server.arg("plain"));
      f.close();
      server.send(200, "text/plain", "Dados restaurados com sucesso!");
      carregarDados();
    }
  });
  server.begin(); 
  adicionarLog("Servidor iniciado."); 
}

// ============================================================
// INICIALIZACAO DOS SENSORES E ATUADORES
// ============================================================
void iniciarSensores() {
  pinMode(PINO_SENSOR_A, INPUT_PULLUP); 
  pinMode(PINO_SENSOR_B, INPUT_PULLUP);
  adicionarLog("Sensores IR iniciados."); 

  pinMode(ENCODER_CLK, INPUT_PULLUP); 
  pinMode(ENCODER_DT,  INPUT_PULLUP); 
  estadoCLK_anterior = digitalRead(ENCODER_CLK); 
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), handleEncoder, CHANGE); 
  adicionarLog("Encoder KY-040 iniciado.");
  
  mp3Serial.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX); 
  
  // Aguarda o DFPlayer inicializar eletricamente
  vTaskDelay(pdMS_TO_TICKS(6000));
  
  if (!dfplayer.begin(mp3Serial, true, false)) { 
    adicionarLog("DFPlayer nao encontrado (Erro de comunicacao Serial).", LOG_WARN);
    dfplayerOk = false;
  } else {
    dfplayer.volume(25); 
    adicionarLog("DFPlayer conectado via Serial. Verificando Cartao SD...");
    
    // Tenta ler a quantidade de arquivos na pasta para validar a presenca do SD
    int qtdArquivos = dfplayer.readFileCountsInFolder(AUDIO_PASTA);
    
    // DEPOIS — tenta algumas vezes com delay antes de desistir
    dfplayerOk = false;
    for (int tentativa = 0; tentativa < 10; tentativa++) {
        vTaskDelay(pdMS_TO_TICKS(1500)); // dá tempo ao módulo indexar o SD
        int qtdArquivos = dfplayer.readFileCountsInFolder(AUDIO_PASTA);
        if (qtdArquivos > 0) {
            dfplayerOk = true;
            adicionarLog("Cartao SD OK. Arquivos na pasta 01: " + String(qtdArquivos));
            break;
        }
        adicionarLog("Tentativa SD " + String(tentativa + 1) + "/3 falhou (retorno: " + String(qtdArquivos) + ")", LOG_WARN);
    }
    if (!dfplayerOk) {
        adicionarLog("Cartao SD nao detectado apos 3 tentativas.", LOG_ERROR);
    }
  }

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) { 
    adicionarLog("OLED nao encontrado.", LOG_WARN);
  } else {
    Wire.begin(21, 22); 
    oled.clearDisplay(); 
    oled.display(); 
    adicionarLog("OLED iniciado.");
  }
}

// ============================================================
// WORKER TASK - FREE RTOS (EXCLUSIVA PARA SENSORES NO CORE 1)
// ============================================================
void TaskSensores(void *pvParameters) {
  adicionarLog("Task de Leitura de Sensores Alocada no Core 1.");
  uint64_t total_busy_c1 = 0;
  uint64_t inicio_ciclo_c1 = esp_timer_get_time();
  unsigned long ultimo_calculo_c1 = millis();
  
  for (;;) {
    uint64_t i_passagem = esp_timer_get_time();
    verificarPassagem();
    tempo_verificarPassagem = esp_timer_get_time() - i_passagem;

    uint64_t i_encoder = esp_timer_get_time();
    verificarEncoder();
    tempo_verificarEncoder = esp_timer_get_time() - i_encoder;

    total_busy_c1 += (tempo_verificarPassagem + tempo_verificarEncoder);
    
    // Libera a CPU do Core 1, permitindo background tasks / sleep
    vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVALO_MS));
    
    if (millis() - ultimo_calculo_c1 > 1000) {
      uint64_t total_elapsed_c1 = esp_timer_get_time() - inicio_ciclo_c1;
      if (total_elapsed_c1 > 0) {
        uso_cpu_c1 = (total_busy_c1 * 100) / total_elapsed_c1;
      }
      total_busy_c1 = 0;
      inicio_ciclo_c1 = esp_timer_get_time();
      ultimo_calculo_c1 = millis();
    }
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200); 
  cpu_freq_mhz = ESP.getCpuFreqMHz();
  
  // Inicialização Segura dos Mutexes para proteger dados globais
  mutexDados = xSemaphoreCreateMutex();
  mutexLogs  = xSemaphoreCreateMutex();

  esp_sleep_enable_timer_wakeup(0);
  // WiFi.setSleep(true); // Redução do rádio no FreeRTOS idle

  iniciarServidor(); 
  iniciarSensores();
  
  xTaskCreatePinnedToCore(
    TaskSensores,         
    "TaskSensores",       
    4096,                 
    NULL,                 
    2,                    
    &TaskSensoresHandle,  
    1                     
  );
  adicionarLog("Sistema Analista de Fluxo iniciado. CPU: " + String(cpu_freq_mhz) + " MHz");
}

// ============================================================
// LOOP PRINCIPAL (CORE 0 — PROCESSAMENTO DE REDE E DISPLAY)
// ============================================================
void loop() {
  static uint64_t total_busy_c0 = 0;
  static uint64_t inicio_ciclo_c0 = esp_timer_get_time();
  static unsigned long ultimo_calculo_c0 = millis();

  uint64_t inicio_work = esp_timer_get_time();
  
  // Enquanto NAO sincronizado, re-tenta a cada 5s (recupera de boot
  // sem rede ou de quedas de WiFi). Depois de sincronizado, faz apenas
  // o refresh periodico de 60s para corrigir deriva do relogio interno.
  unsigned long intervaloNtp = ntpSincronizado ? NTP_REFRESH_INTERVAL_MS : NTP_RETRY_INTERVAL_MS;
  if (millis() - ultimoNtpUpdate > intervaloNtp) {
    tentarSincronizarNtp();
    ultimoNtpUpdate = millis();
  }

  if (millis() - ultimoSalvamento > INTERVALO_SALVAMENTO) { 
    uint64_t i_salvar = esp_timer_get_time();
    expurgarLogsExpirados(); // Remove logs com mais de 24h antes de persistir
    salvarDados();
    salvarGraficos();
    salvarLogs();
    tempo_salvarDados = esp_timer_get_time() - i_salvar;
    ultimoSalvamento = millis(); 
  }

  verificarConexao(); 

  uint64_t i_client = esp_timer_get_time();
  server.handleClient();
  tempo_handleClient = esp_timer_get_time() - i_client;

  uint64_t i_display = esp_timer_get_time();
  atualizarDisplay(); 
  tempo_atualizarDisplay = esp_timer_get_time() - i_display;

  uint64_t fim_work = esp_timer_get_time();
  total_busy_c0 += (fim_work - inicio_work);

  // Cede processamento e entra em Idle
  vTaskDelay(pdMS_TO_TICKS(5));
  
  if (millis() - ultimo_calculo_c0 > 1000) {
    uint64_t total_elapsed_c0 = esp_timer_get_time() - inicio_ciclo_c0;
    if (total_elapsed_c0 > 0) {
      uint8_t uso_cpu_c0 = (total_busy_c0 * 100) / total_elapsed_c0;
      uso_cpu_total = (uso_cpu_c0 + uso_cpu_c1) / 2;
      if (uso_cpu_total > 100) uso_cpu_total = 100;
    }
    total_busy_c0 = 0;
    inicio_ciclo_c0 = esp_timer_get_time();
    ultimo_calculo_c0 = millis();
  }
}