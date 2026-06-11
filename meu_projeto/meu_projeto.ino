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

struct LogEntry {
  char nivel[6];
  char mensagem[120];
};

LogEntry logsistema[MAX_LOGS];
int totalLogs = 0; 

unsigned long ultimoNtpUpdate  = 0; 
unsigned long ultimoSalvamento = 0;
#define INTERVALO_SALVAMENTO 30000  

// ============================================================
// FUNCOES — NTP / HORARIO
// ============================================================
String obterHorario() {
  return ntp.getFormattedTime();
}

long obterEpoch() {
  return ntp.getEpochTime(); 
}

// ============================================================
// FUNCOES — LOGS COM NIVEL (Ring Buffer Thread-Safe)
// ============================================================
void adicionarLog(String mensagem, const char* nivel = LOG_INFO) {
  String entrada = "[" + obterHorario() + "] [" + String(nivel) + "] " + mensagem;
  Serial.println(entrada); 
  
  if (mutexLogs != NULL && xSemaphoreTake(mutexLogs, portMAX_DELAY)) {
    int idx = totalLogs % MAX_LOGS; // Sobrescreve o mais antigo se estourar limite
    strncpy(logsistema[idx].nivel, nivel, 5);
    logsistema[idx].nivel[5] = '\0';
    strncpy(logsistema[idx].mensagem, entrada.c_str(), 119);
    logsistema[idx].mensagem[119] = '\0';
    totalLogs++; 
    xSemaphoreGive(mutexLogs);
  }
}

// ============================================================
// FUNCOES — PERSISTENCIA (LittleFS)
// ============================================================
#define ARQUIVO_DADOS   "/dados.json" 
#define ARQUIVO_CONFIG  "/config.json"

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
    
    // Apenas define qual audio deve tocar, sem executar comandos lentos aqui dentro
    if (contadorPessoas == LOTACAO_MAX) {
      acaoAudio = AUDIO_LOTACAO_MAX;
    } else if (contadorPessoas == LOTACAO_MAX / 2) {
      acaoAudio = AUDIO_METADE_MAX;
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
  int entradasPorHora[24] = {0}; 
  int saidasPorHora[24]   = {0};
  int porDia[30]          = {0}; 
  long agora = obterEpoch();
  
  xSemaphoreTake(mutexDados, portMAX_DELAY);
  int qtd = min(totalRegistros, MAX_REGISTROS);
  
  for (int i = 0; i < qtd; i++) { 
    int idx = (totalRegistros > MAX_REGISTROS) ? ((totalRegistros + i) % MAX_REGISTROS) : i;
    long diff = agora - historico[idx].timestampUnix;
    
    if (diff >= 0 && diff <= 86400L) { 
      int hora = (historico[idx].horario[0] - '0') * 10 + (historico[idx].horario[1] - '0');
      if (hora >= 0 && hora < 24) { 
        if (strcmp(historico[idx].tipo, "ENTRADA") == 0) entradasPorHora[hora]++;
        else                                             saidasPorHora[hora]++;
      }
    }
    long dia = diff / 86400L;
    if (dia >= 0 && dia < 30) porDia[29 - dia]++; 
  }
  xSemaphoreGive(mutexDados);

  String json = "{\"porHora\":{\"entradas\":[";
  for (int i = 0; i < 24; i++) { if (i > 0) json += ","; json += String(entradasPorHora[i]); } 
  json += "],\"saidas\":["; 
  for (int i = 0; i < 24; i++) { if (i > 0) json += ","; json += String(saidasPorHora[i]); } 
  json += "]},\"porDia\":["; 
  for (int i = 0; i < 30; i++) { if (i > 0) json += ","; json += String(porDia[i]); } 
  json += "]}"; 
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
  ntp.update(); 
  ultimoNtpUpdate = millis(); 
  adicionarLog("NTP sincronizado: " + ntp.getFormattedTime()); 

  carregarConfig();
  carregarDados();
  
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
    
    if (qtdArquivos <= 0) {
      adicionarLog("Erro no Cartao SD: Nao detectado ou pasta 01 ausente/vazia.", LOG_ERROR);
      dfplayerOk = false; // Desativa para prevenir chamadas travantes no loop
    } else {
      dfplayerOk = true; 
      adicionarLog("Cartao SD detectado com sucesso! Arquivos na pasta 01: " + String(qtdArquivos));
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
  
  if (millis() - ultimoNtpUpdate > 60000) { 
    ntp.update(); 
    ultimoNtpUpdate = millis();
  }

  if (millis() - ultimoSalvamento > INTERVALO_SALVAMENTO) { 
    uint64_t i_salvar = esp_timer_get_time();
    salvarDados();
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