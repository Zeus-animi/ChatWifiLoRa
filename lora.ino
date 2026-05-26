/*
  ============================================================================
  LoRaMeshChat  -  Firmware para Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
  ============================================================================

  Autor base do conceito: voce :)  | Reescrito para Heltec V3 / SX1262.
  Inspirado no seu antigo "esp8266-Chat" (AsyncWebServer + WebSocket + HTML
  no flash). Aqui o transporte do chat NAO e o WiFi: o WiFi serve apenas a
  interface (paginas + WebSocket entre o seu telemovel/PC e a placa). O envio
  e a recepcao real das mensagens entre placas e feito por LoRa (SX1262),
  formando uma rede MESH com retransmissao (flood controlado por TTL).

  -------------------------------------------------------------------------
  COMO FUNCIONA (visao geral, "com niveis"):
  -------------------------------------------------------------------------
  1) Ao ligar, a placa cria um Access Point WiFi proprio e fica em
     192.168.1.1. Voce liga o telemovel nessa rede e abre http://192.168.1.1
  2) Aparece a tela inicial com dois botoes:  [ CONEXAO ]   [ HOSPEDAR ]
       - CONEXAO  -> a placa escuta beacons LoRa por alguns segundos e lista
                     as outras placas LoRa que estao ligadas por perto. Voce
                     escolhe uma e clica "Conectar" para entrar na malha.
       - HOSPEDAR -> esta placa anuncia-se como ADMINISTRADOR da malha.
                     (Nota tecnica: numa rede mesh LoRa nao existe "servidor
                     central" obrigatorio; todos falam com todos. O HOST e o
                     moderador: pode autorizar, bloquear e expulsar placas.)
  3) Depois pede o NOME de utilizador. Com o nome definido, abre o chat:
       - Chat GERAL (todos da malha recebem)
       - Chat PRIVADO por placa (mensagem direcionada a um nodeId)
  4) Varios telemoveis/PCs podem ligar-se ao MESMO WiFi da placa, cada um
     com o seu proprio nome. Cada utilizador WiFi e um "cliente" daquela
     placa; a placa coloca as mensagens deles na malha LoRa.
  5) Da para descarregar o historico do chat (.txt e .json).

  -------------------------------------------------------------------------
  BIBLIOTECAS NECESSARIAS (Arduino IDE -> Gestor de Bibliotecas):
  -------------------------------------------------------------------------
    - "RadioLib"            (jgromes)              >= 6.x
    - "ESPAsyncWebServer"   (ESP32Async / me-no-dev)
    - "AsyncTCP"            (ESP32Async / me-no-dev)   <-- ESP32 (NAO ESPAsyncTCP)
    - "ArduinoJson"         (bblanchon)            >= 6.x
    - "U8g2"                (olikraus)             (opcional, OLED)

  PLACA (Boards Manager): "esp32 by Espressif" -> selecione
    "Heltec WiFi LoRa 32(V3)"  (ou "ESP32S3 Dev Module").

  Frequencia: 868.0 MHz (Europa/Portugal). Mude LORA_FREQ p/ 915.0 nos EUA.

  ============================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <RadioLib.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ----- OLED DESLIGADO para poupar bateria. Mantido em 0. --------------------
// O ecra OLED foi desativado a pedido (economia de energia). Para reativar,
// mude para 1 e instale a biblioteca U8g2.
#define USE_OLED 0
#if USE_OLED
  #include <U8g2lib.h>
  #include <Wire.h>
  // Heltec V3 OLED: SDA=17, SCL=18, RST=21
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C oled(U8G2_R0, /*SCL=*/18, /*SDA=*/17, /*RST=*/21);
#endif

// ===========================================================================
//   DECLARACOES ANTECIPADAS DE TIPOS
//   (O Arduino IDE gera prototipos das funcoes no topo do ficheiro. Sem estas
//    linhas, um prototipo como "PeerNode* findPeer(...)" apareceria ANTES da
//    definicao da struct, causando: 'PeerNode' does not name a type.)
// ===========================================================================
struct PeerNode;
struct ChatLine;
struct SeenEntry;
struct WsClientInfo;


// Pinagem confirmada da Heltec WiFi LoRa 32 V3:
#define LORA_NSS    8
#define LORA_DIO1   14
#define LORA_RST    12
#define LORA_BUSY   13
#define LORA_SCK    9
#define LORA_MISO   11
#define LORA_MOSI   10

// Parametros de radio. TODAS as placas DEVEM usar os mesmos valores!
float  LORA_FREQ        = 868.0;   // MHz  (915.0 nos EUA)
float  LORA_BW          = 125.0;   // kHz
uint8_t LORA_SF         = 9;       // Spreading Factor (7..12). 9 = bom equilibrio
uint8_t LORA_CR         = 7;       // Coding Rate 4/7
uint8_t LORA_SYNC       = 0x34;    // Sync word (privado). 0x34 = rede privada
int8_t  LORA_POWER      = 20;      // dBm (2..22). 14 dBm e o limite legal EU868 ERP
uint16_t LORA_PREAMBLE  = 8;

SPIClass spiLoRa(HSPI);
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, spiLoRa);

// Flags de IRQ do radio (recepcao)
volatile bool loraGotPacket = false;
void IRAM_ATTR onLoRaDio1() { loraGotPacket = true; }

// ===========================================================================
//                         CONFIGURACAO WiFi (AP)
// ===========================================================================
IPAddress apIP(192, 168, 1, 1);
IPAddress apGW(192, 168, 1, 1);
IPAddress apMASK(255, 255, 255, 0);

// O SSID inclui parte do ID da placa para nao colidir entre as duas placas.
String apSSID;                       // definido no setup
const char* apPASSWORD = "Zeus6996"; // >= 8 chars. Mude se quiser.

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ===========================================================================
//                         IDENTIDADE / ESTADO
// ===========================================================================
Preferences prefs;

uint16_t myNodeId;            // ID curto desta placa (derivado do MAC/EFUSE)
String   myNodeIdHex;         // "A1B2"
String   myDeviceName;        // nome amigavel da placa (ex.: "Placa-A1B2")

bool   isHost      = false;   // esta placa e o administrador da malha?
uint16_t hostId    = 0;       // ID do host atual da malha (0 = nenhum)
bool   joinedMesh  = false;   // ja entrou/aceitou participar da malha?

// "Quem hospedou primeiro vence": momento (epoch relativo) em que ESTA placa
// reivindicou o papel de host. Como as placas nao tem relogio real, usamos um
// numero crescente baseado em millis no instante da reivindicacao + um desempate
// pelo nodeId (menor id ganha em caso de empate exato). Menor claimTime = mais antigo.
uint32_t myHostClaim   = 0;   // 0 = nao reivindiquei
uint32_t hostClaimSeen = 0;   // claim do host atualmente reconhecido

// ===========================================================================
//                    PROTOCOLO LoRa (pacotes da malha)
// ===========================================================================
// Formato de pacote: JSON compacto. Campos:
//   t  : tipo  (BEACON, JOIN, WELCOME, MSG, DM, ADMIN, ACK)
//   src: nodeId de origem (hex)
//   dst: nodeId de destino (hex) ou "ALL"
//   id : id unico da mensagem (para deduplicacao no flood)
//   ttl: time-to-live (saltos restantes)
//   nm : nome do utilizador (em MSG/DM)
//   bd : corpo/texto
//   act: acao admin (KICK/BAN/UNBAN/AUTH/HOSTCLAIM/HOSTYIELD)
//
// Tipos:
//   BEACON  : "estou vivo", enviado periodicamente. Usado no scan "CONEXAO".
//   JOIN    : pedido de entrada na malha (enviado ao host).
//   WELCOME : host aceita / informa estado.
//   MSG     : mensagem de chat geral (dst=ALL).
//   DM      : mensagem direta para um nodeId.
//   ADMIN   : comandos de moderacao do host.
//   ACK     : confirmacao opcional.

enum PktType { PKT_BEACON, PKT_JOIN, PKT_WELCOME, PKT_MSG, PKT_DM, PKT_ADMIN, PKT_ACK };

const uint8_t  DEFAULT_TTL   = 4;     // saltos maximos no flood
const uint32_t BEACON_EVERY  = 8000;  // ms entre beacons
uint32_t lastBeacon = 0;

// ----- Deduplicacao de flood: guarda ids ja vistos -----
#define SEEN_MAX 64
struct SeenEntry { uint32_t id; uint32_t ts; };
SeenEntry seen[SEEN_MAX];
uint8_t seenHead = 0;

bool alreadySeen(uint32_t id) {
  for (uint8_t i = 0; i < SEEN_MAX; i++) if (seen[i].id == id) return true;
  return false;
}
void markSeen(uint32_t id) {
  seen[seenHead].id = id;
  seen[seenHead].ts = millis();
  seenHead = (seenHead + 1) % SEEN_MAX;
}
uint32_t makeMsgId() {
  // id unico = (nodeId << 16) | contador aleatorio crescente
  static uint16_t ctr = 0;
  ctr++;
  return ((uint32_t)myNodeId << 16) | ctr;
}

// ===========================================================================
//             TABELA DE NODES (placas) e PEERS (vistos no scan)
// ===========================================================================
struct PeerNode {
  uint16_t id;
  String   name;
  uint32_t lastSeen;   // millis
  int      rssi;
  bool     banned;
  bool     authorized; // relevante quando esta placa e host
  bool     used;
};
#define MAX_PEERS 24
PeerNode peers[MAX_PEERS];

PeerNode* findPeer(uint16_t id) {
  for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used && peers[i].id == id) return &peers[i];
  return nullptr;
}
PeerNode* addOrUpdatePeer(uint16_t id, const String& name, int rssi) {
  PeerNode* p = findPeer(id);
  if (!p) {
    for (int i = 0; i < MAX_PEERS; i++) if (!peers[i].used) { p = &peers[i]; break; }
    if (!p) return nullptr; // tabela cheia
    p->used = true; p->banned = false; p->authorized = !isHost; // se nao ha host, todos livres
    p->id = id;
  }
  if (name.length()) p->name = name;
  p->lastSeen = millis();
  p->rssi = rssi;
  return p;
}
bool isBanned(uint16_t id) { PeerNode* p = findPeer(id); return p && p->banned; }

// ===========================================================================
//                         HISTORICO DE CHAT
// ===========================================================================
struct ChatLine {
  uint32_t ts;       // millis no momento de receber/enviar
  uint16_t from;     // nodeId de origem
  String   name;     // nome do utilizador
  String   text;     // texto
  bool     dm;       // privado?
  uint16_t to;       // destino (se dm)
};
#define MAX_HISTORY 120
ChatLine history[MAX_HISTORY];
int historyCount = 0;
int historyHead = 0;

void pushHistory(uint16_t from, const String& name, const String& text, bool dm, uint16_t to) {
  ChatLine &c = history[historyHead];
  c.ts = millis(); c.from = from; c.name = name; c.text = text; c.dm = dm; c.to = to;
  historyHead = (historyHead + 1) % MAX_HISTORY;
  if (historyCount < MAX_HISTORY) historyCount++;
}

// ===========================================================================
//                    FORWARD DECLARATIONS
// ===========================================================================
void loraSend(const String& payload);
void sendBeacon();
void sendChat(const String& name, const String& text, bool dm, uint16_t to);
void broadcastWsState();
void broadcastWsNotice(const String& text);
void broadcastWsChat(uint16_t from, const String& name, const String& text, bool dm, uint16_t to);
void broadcastWsPeers();
String peersJson();
void handleLoRaPacket(const String& data, int rssi);
void oledStatus(const String& l1, const String& l2, const String& l3);
#if !USE_OLED
void powerDownOled();
#endif

// ===========================================================================
//                         PAGINA HTML (servida do flash)
// ===========================================================================
// Uma unica pagina; o JS controla "niveis"/telas. Comunicacao via WebSocket.
const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html><html lang="pt"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LoRa Mesh Chat</title>
<style>
  :root{--bg:#0b1020;--card:#141b2e;--accent:#ff3b30;--accent2:#0a84ff;--txt:#e6e9f0;--mut:#8b93a7;}
  *{box-sizing:border-box;font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif}
  body{margin:0;background:var(--bg);color:var(--txt);height:100vh;display:flex;flex-direction:column}
  header{padding:12px 16px;background:var(--card);display:flex;align-items:center;gap:10px;border-bottom:1px solid #222b44}
  header b{font-size:18px}
  .dot{width:10px;height:10px;border-radius:50%;background:#33d17a}
  .screen{flex:1;display:none;flex-direction:column;padding:16px;overflow:auto}
  .screen.active{display:flex}
  .center{align-items:center;justify-content:center;text-align:center;gap:16px}
  button{background:var(--accent2);color:#fff;border:0;border-radius:12px;padding:14px 18px;font-size:16px;cursor:pointer}
  button.red{background:var(--accent)}
  button.ghost{background:#222b44}
  button:active{transform:scale(.98)}
  .bigbtns{display:flex;gap:16px;flex-wrap:wrap;justify-content:center}
  .bigbtns button{min-width:160px;min-height:120px;font-size:20px;border-radius:18px}
  input[type=text]{width:100%;padding:14px;border-radius:12px;border:1px solid #2a3656;background:#0e1426;color:var(--txt);font-size:16px}
  .card{background:var(--card);border:1px solid #222b44;border-radius:14px;padding:14px;margin:8px 0}
  .peer{display:flex;justify-content:space-between;align-items:center;gap:8px}
  .peer small{color:var(--mut)}
  #chatBox{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:8px;padding:4px}
  .msg{max-width:80%;padding:8px 12px;border-radius:14px;background:#1c2540;align-self:flex-start;word-wrap:break-word}
  .msg.me{align-self:flex-end;background:#13407a}
  .msg .who{font-size:12px;color:var(--mut);margin-bottom:2px}
  .msg.dm{border:1px dashed var(--accent)}
  .row{display:flex;gap:8px;margin-top:8px}
  .row input{flex:1}
  .tabs{display:flex;gap:8px;margin-bottom:8px;flex-wrap:wrap}
  .tab{padding:8px 12px;border-radius:10px;background:#222b44;cursor:pointer;font-size:14px}
  .tab.active{background:var(--accent2)}
  .badge{font-size:11px;background:var(--accent);padding:2px 6px;border-radius:8px;margin-left:6px}
  .muted{color:var(--mut);font-size:13px}
  .adminbar{display:flex;gap:6px;flex-wrap:wrap}
  .adminbar button{padding:6px 10px;font-size:13px;border-radius:8px}
  a.dl{display:inline-block;margin:6px 8px 0 0;color:var(--accent2)}
</style></head>
<body>
<header>
  <span class="dot" id="netdot"></span>
  <b>LoRa Mesh Chat</b>
  <span class="muted" id="hdrInfo" style="margin-left:auto"></span>
</header>

<!-- ============ NIVEL 0: ESCOLHA ============ -->
<section class="screen active center" id="scHome">
  <h2>O que deseja fazer?</h2>
  <div class="bigbtns">
    <button class="red" onclick="goConnect()">CONEXAO<br><small>entrar numa malha</small></button>
    <button onclick="goHost()">HOSPEDAR<br><small>ser administrador</small></button>
  </div>
  <p class="muted" id="homeId"></p>
</section>

<!-- ============ NIVEL 1: SCAN / CONEXAO ============ -->
<section class="screen" id="scScan">
  <h2>Placas LoRa por perto</h2>
  <p class="muted">A escutar beacons LoRa... ligue a outra placa.</p>
  <div id="peerList"></div>
  <div class="row">
    <button class="ghost" onclick="rescan()">Procurar de novo</button>
    <button class="ghost" onclick="show('scHome')">Voltar</button>
  </div>
</section>

<!-- ============ NIVEL 1b: HOST CONFIRMA ============ -->
<section class="screen center" id="scHost">
  <h2>Modo Administrador</h2>
  <p class="muted">Esta placa anuncia-se como host da malha. Pode autorizar, bloquear e expulsar placas.</p>
  <button class="red" onclick="confirmHost()">Confirmar e hospedar</button>
  <button class="ghost" onclick="show('scHome')">Voltar</button>
</section>

<!-- ============ NIVEL 2: NOME ============ -->
<section class="screen center" id="scName">
  <h2>Qual o seu nome?</h2>
  <input type="text" id="nameInput" maxlength="20" placeholder="Seu nome">
  <button onclick="saveName()">Entrar no chat</button>
</section>

<!-- ============ NIVEL 3: CHAT ============ -->
<section class="screen" id="scChat">
  <div class="tabs" id="tabs">
    <div class="tab active" data-dst="ALL" onclick="selTab(this)">Geral</div>
  </div>
  <div id="adminPanel" class="card" style="display:none">
    <b>Painel do Administrador</b> <span class="muted">(voce hospeda esta malha)</span>
    <div id="adminPeers"></div>
  </div>
  <div id="chatBox"></div>
  <div class="row">
    <input type="text" id="chatInput" maxlength="200" placeholder="Mensagem (max 200)">
    <button onclick="sendMsg()">Enviar</button>
  </div>
  <div>
    <a class="dl" href="/history.txt" target="_blank">Baixar historico (.txt)</a>
  </div>
</section>

<script>
var ws, myName="", myId="", curDst="ALL", amHost=false, peers={}, authorized=true;

function show(id){document.querySelectorAll('.screen').forEach(s=>s.classList.remove('active'));document.getElementById(id).classList.add('active');}

function connectWs(){
  ws=new WebSocket('ws://'+location.hostname+'/ws');
  ws.onopen=()=>{document.getElementById('netdot').style.background='#33d17a';};
  ws.onclose=()=>{document.getElementById('netdot').style.background='#ff3b30';setTimeout(connectWs,1500);};
  ws.onmessage=(e)=>{handle(JSON.parse(e.data));};
}
function send(o){ if(ws&&ws.readyState==1) ws.send(JSON.stringify(o)); }

function handle(m){
  if(m.t=='state'){
    myId=m.myId; amHost=m.isHost; authorized=m.authorized;
    document.getElementById('homeId').innerText='ID desta placa: '+m.myId;
    document.getElementById('hdrInfo').innerText='ID '+m.myId+(m.hostId&&m.hostId!='0000'?' · host '+m.hostId:'')+(amHost?' · VOCE HOSPEDA':'');
    if(amHost){document.getElementById('adminPanel').style.display='block';}
  }
  else if(m.t=='peers'){
    peers={}; m.list.forEach(p=>peers[p.id]=p);
    renderPeers(); renderTabs(); renderAdmin();
  }
  else if(m.t=='chat'){
    addMsg(m);
    pushHist(m);
  }
  else if(m.t=='notice'){
    addNotice(m.bd);
  }
}

// ---------- NIVEL 0 ----------
function goConnect(){ show('scScan'); send({c:'scan'}); }
function goHost(){ show('scHost'); }
function confirmHost(){ send({c:'host'}); show('scName'); }

// ---------- NIVEL 1: scan ----------
function rescan(){ send({c:'scan'}); }
function renderPeers(){
  if(!document.getElementById('scScan').classList.contains('active')) return;
  var d=document.getElementById('peerList'); d.innerHTML='';
  var arr=Object.values(peers);
  if(arr.length==0){d.innerHTML='<p class="muted">Nenhuma placa encontrada ainda...</p>';return;}
  arr.forEach(p=>{
    var el=document.createElement('div'); el.className='card peer';
    el.innerHTML='<div><b>'+(p.name||('Placa '+p.id))+'</b><br><small>ID '+p.id+' · RSSI '+p.rssi+' dBm</small></div>';
    var b=document.createElement('button'); b.innerText='Conectar';
    b.onclick=()=>{ send({c:'join',dst:p.id}); show('scName'); };
    el.appendChild(b); d.appendChild(el);
  });
}

// ---------- NIVEL 2: nome ----------
function saveName(){
  var n=document.getElementById('nameInput').value.trim();
  if(!n)return; myName=n; send({c:'name',name:n}); show('scChat'); renderTabs();
}

// ---------- NIVEL 3: chat ----------
function renderTabs(){
  var t=document.getElementById('tabs');
  // mantem "Geral" e recria abas por placa
  t.innerHTML='<div class="tab '+(curDst=='ALL'?'active':'')+'" data-dst="ALL" onclick="selTab(this)">Geral</div>';
  Object.values(peers).forEach(p=>{
    var d=document.createElement('div'); d.className='tab'+(curDst==p.id?' active':'');
    d.dataset.dst=p.id; d.innerText=(p.name||('Placa '+p.id));
    d.onclick=function(){selTab(this);};
    t.appendChild(d);
  });
}
function selTab(el){curDst=el.dataset.dst;document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));el.classList.add('active');renderChat();}

var allMsgs=[];
function pushHist(m){ allMsgs.push(m); }
function addMsg(m){
  // so mostra se pertence a aba atual
  var belongs = (curDst=='ALL' && !m.dm) || (m.dm && (m.from==curDst || m.to==curDst));
  if(!belongs) return;
  renderOne(m);
}
function renderChat(){
  var box=document.getElementById('chatBox'); box.innerHTML='';
  allMsgs.forEach(m=>{
    var belongs = (curDst=='ALL' && !m.dm) || (m.dm && (m.from==curDst || m.to==curDst));
    if(belongs) renderOne(m);
  });
}
function renderOne(m){
  var box=document.getElementById('chatBox');
  var div=document.createElement('div');
  var mine = (m.from==myId);
  div.className='msg'+(mine?' me':'')+(m.dm?' dm':'');
  div.innerHTML='<div class="who">'+(m.name||('Placa '+m.from))+(m.dm?' (privado)':'')+'</div>'+escapeHtml(m.bd);
  box.appendChild(div); box.scrollTop=box.scrollHeight;
}
function addNotice(txt){
  var box=document.getElementById('chatBox');
  var div=document.createElement('div'); div.className='muted'; div.style.alignSelf='center';
  div.innerText='— '+txt+' —'; box.appendChild(div); box.scrollTop=box.scrollHeight;
}
function sendMsg(){
  var i=document.getElementById('chatInput'); var v=i.value.trim(); if(!v)return;
  send({c:'msg',dst:curDst,bd:v}); i.value='';
}

// ---------- Painel admin ----------
function renderAdmin(){
  if(!amHost)return;
  var d=document.getElementById('adminPeers'); d.innerHTML='';
  Object.values(peers).forEach(p=>{
    var el=document.createElement('div'); el.className='peer'; el.style.marginTop='8px';
    el.innerHTML='<div><b>'+(p.name||('Placa '+p.id))+'</b> <small class="muted">ID '+p.id+'</small>'+(p.banned?'<span class="badge">BLOQUEADA</span>':'')+'</div>';
    var bar=document.createElement('div'); bar.className='adminbar';
    var auth=document.createElement('button'); auth.className='ghost'; auth.innerText='Autorizar';
    auth.onclick=()=>send({c:'admin',act:'AUTH',dst:p.id});
    var kick=document.createElement('button'); kick.innerText='Expulsar';
    kick.onclick=()=>send({c:'admin',act:'KICK',dst:p.id});
    var ban=document.createElement('button'); ban.className='red'; ban.innerText=p.banned?'Desbloquear':'Bloquear';
    ban.onclick=()=>send({c:'admin',act:(p.banned?'UNBAN':'BAN'),dst:p.id});
    bar.appendChild(auth);bar.appendChild(kick);bar.appendChild(ban);
    el.appendChild(bar); d.appendChild(el);
  });
}

function escapeHtml(s){return s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}

window.onload=connectWs;
</script>
</body></html>
)HTMLPAGE";

// ===========================================================================
//                    UTIL: identidade da placa
// ===========================================================================
void computeIdentity() {
  uint64_t mac = ESP.getEfuseMac();
  myNodeId = (uint16_t)((mac >> 24) ^ (mac & 0xFFFF)); // 16 bits estaveis
  if (myNodeId == 0) myNodeId = 0xABCD;
  char buf[6]; sprintf(buf, "%04X", myNodeId);
  myNodeIdHex = String(buf);
  myDeviceName = "Placa-" + myNodeIdHex;
  apSSID = "LoRaMesh-" + myNodeIdHex;
}

// ===========================================================================
//                    LoRa: enviar / receber
// ===========================================================================
void loraSend(const String& payload) {
  // RadioLib: transmite e volta para recepcao.
  // Algumas versoes do RadioLib pedem String& (nao-const), por isso copiamos.
  String tx = payload;
  int st = radio.transmit(tx);
  radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] TX falhou: %d\n", st);
  }
}

void sendBeacon() {
  StaticJsonDocument<160> d;
  d["t"]   = "BEACON";
  d["src"] = myNodeIdHex;
  d["nm"]  = myDeviceName;
  d["host"]= isHost ? 1 : 0;
  if (isHost) d["hc"] = myHostClaim;   // host claim: quanto menor, mais antigo
  String s; serializeJson(d, s);
  loraSend(s);
}

void sendJoin(uint16_t dst) {
  StaticJsonDocument<160> d;
  d["t"]   = "JOIN";
  d["src"] = myNodeIdHex;
  char b[6]; sprintf(b,"%04X",dst); d["dst"]=b;
  d["id"]  = makeMsgId();
  d["ttl"] = DEFAULT_TTL;
  d["nm"]  = myDeviceName;
  String s; serializeJson(d, s); loraSend(s);
}

void sendWelcome(uint16_t dst, bool authorized) {
  StaticJsonDocument<160> d;
  d["t"]="WELCOME"; d["src"]=myNodeIdHex;
  char b[6]; sprintf(b,"%04X",dst); d["dst"]=b;
  d["id"]=makeMsgId(); d["ttl"]=DEFAULT_TTL;
  d["host"]=myNodeIdHex; d["ok"]=authorized?1:0;
  String s; serializeJson(d,s); loraSend(s);
}

void sendChat(const String& name, const String& text, bool dm, uint16_t to) {
  StaticJsonDocument<320> d;
  d["t"]   = dm ? "DM" : "MSG";
  d["src"] = myNodeIdHex;
  if (dm) { char b[6]; sprintf(b,"%04X",to); d["dst"]=b; } else d["dst"]="ALL";
  d["id"]  = makeMsgId();
  d["ttl"] = DEFAULT_TTL;
  d["nm"]  = name;
  d["bd"]  = text;
  String s; serializeJson(d, s);
  loraSend(s);
  // ecoa localmente no historico/UI
  pushHistory(myNodeId, name, text, dm, to);
  broadcastWsChat(myNodeId, name, text, dm, to);
}

void sendAdmin(const String& act, uint16_t dst) {
  StaticJsonDocument<160> d;
  d["t"]="ADMIN"; d["src"]=myNodeIdHex;
  char b[6]; sprintf(b,"%04X",dst); d["dst"]=b;
  d["id"]=makeMsgId(); d["ttl"]=DEFAULT_TTL; d["act"]=act;
  String s; serializeJson(d,s); loraSend(s);
}

uint16_t hexToId(const char* h) { return (uint16_t)strtol(h, nullptr, 16); }

void maybeForward(StaticJsonDocument<512>& d, uint32_t id) {
  // Flood controlado: decrementa ttl e reenvia se ainda houver saltos
  int ttl = d["ttl"] | 0;
  if (ttl <= 1) return;
  d["ttl"] = ttl - 1;
  String s; serializeJson(d, s);
  loraSend(s);
}

void handleLoRaPacket(const String& data, int rssi) {
  StaticJsonDocument<512> d;
  DeserializationError err = deserializeJson(d, data);
  if (err) return;

  const char* t = d["t"] | "";
  uint16_t src = hexToId(d["src"] | "0000");
  if (src == myNodeId) return; // ignora ecos proprios

  // BEACON: atualiza lista de peers (scan)
  if (!strcmp(t, "BEACON")) {
    String nm = d["nm"] | "";
    bool theyHost = (int)(d["host"] | 0) == 1;
    PeerNode* p = addOrUpdatePeer(src, nm, rssi);

    if (theyHost) {
      uint32_t theirClaim = d["hc"] | 0xFFFFFFFF;

      // Regra "quem hospedou primeiro vence":
      // Se EU sou host e o outro reivindicou ANTES de mim (claim menor, ou igual
      // mas com nodeId menor para desempate), eu cedo o papel de host.
      if (isHost) {
        bool theyOlder = (theirClaim < myHostClaim) ||
                         (theirClaim == myHostClaim && src < myNodeId);
        if (theyOlder) {
          isHost = false;
          myHostClaim = 0;
          hostId = src;
          hostClaimSeen = theirClaim;
          joinedMesh = true;
          broadcastWsState();
          broadcastWsNotice("Outra placa ja hospedava antes. Voce deixou de ser admin.");
          oledStatus("Cedi o host", "Host: " + String(d["src"] | "?"), "");
        }
      } else {
        // Nao sou host: adoto o host mais antigo que conhecer
        if (hostId == 0 || theirClaim < hostClaimSeen) {
          hostId = src;
          hostClaimSeen = theirClaim;
        }
      }
    }
    broadcastWsPeers();
    return;
  }

  // Deduplicacao para tipos com id (flood)
  uint32_t id = d["id"] | 0;
  if (id && alreadySeen(id)) return;
  if (id) markSeen(id);

  // Esta mensagem e para mim ou para todos?
  const char* dst = d["dst"] | "ALL";
  bool toAll = !strcmp(dst, "ALL");
  uint16_t dstId = toAll ? 0 : hexToId(dst);
  bool forMe = toAll || (dstId == myNodeId);

  if (!strcmp(t, "JOIN")) {
    String nm = d["nm"] | "";
    addOrUpdatePeer(src, nm, rssi);
    if (isHost) {
      bool ok = !isBanned(src);
      PeerNode* p = findPeer(src); if (p) p->authorized = ok;
      sendWelcome(src, ok);
    }
    broadcastWsPeers();
    maybeForward(d, id);
    return;
  }

  if (!strcmp(t, "WELCOME") && forMe) {
    hostId = hexToId(d["host"] | "0000");
    bool ok = (int)(d["ok"] | 0) == 1;
    joinedMesh = ok;
    broadcastWsState();
    return;
  }

  if (!strcmp(t, "ADMIN")) {
    const char* act = d["act"] | "";
    PeerNode* p = addOrUpdatePeer(src, "", rssi); // origem (host)
    if (forMe) {
      // comando dirigido a mim
      if (!strcmp(act, "KICK")) { joinedMesh = false; broadcastWsState(); }
      else if (!strcmp(act, "BAN")) { joinedMesh = false; broadcastWsState(); }
    }
    // Atualiza estado local sobre o alvo (para o painel admin de todos verem)
    broadcastWsPeers();
    maybeForward(d, id);
    return;
  }

  if (!strcmp(t, "MSG") || !strcmp(t, "DM")) {
    bool dm = !strcmp(t, "DM");
    if (isBanned(src)) { maybeForward(d, id); return; } // bloqueada: nao mostra
    String nm = d["nm"] | "";
    String bd = d["bd"] | "";
    addOrUpdatePeer(src, nm, rssi);
    if (forMe) {
      pushHistory(src, nm, bd, dm, dm ? myNodeId : 0);
      broadcastWsChat(src, nm, bd, dm, dm ? myNodeId : 0);
    }
    maybeForward(d, id); // continua a propagar na malha
    return;
  }
}

// ===========================================================================
//                    WebSocket -> navegador
// ===========================================================================
String peersJson() {
  StaticJsonDocument<2048> d;
  d["t"] = "peers";
  JsonArray a = d.createNestedArray("list");
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) continue;
    JsonObject o = a.createNestedObject();
    char b[6]; sprintf(b,"%04X",peers[i].id);
    o["id"]   = b;
    o["name"] = peers[i].name;
    o["rssi"] = peers[i].rssi;
    o["banned"] = peers[i].banned;
  }
  String s; serializeJson(d, s); return s;
}

void broadcastWsPeers() { ws.textAll(peersJson()); }

void broadcastWsNotice(const String& text) {
  StaticJsonDocument<256> d;
  d["t"] = "notice";
  d["bd"] = text;
  String s; serializeJson(d, s); ws.textAll(s);
}

void broadcastWsState() {
  StaticJsonDocument<256> d;
  d["t"]="state"; d["myId"]=myNodeIdHex;
  char hb[6]; sprintf(hb,"%04X",hostId); d["hostId"]=hb;
  d["isHost"]=isHost; d["authorized"]=joinedMesh || hostId==0 || isHost;
  String s; serializeJson(d,s); ws.textAll(s);
}

void broadcastWsChat(uint16_t from, const String& name, const String& text, bool dm, uint16_t to) {
  StaticJsonDocument<512> d;
  d["t"]="chat";
  char fb[6]; sprintf(fb,"%04X",from); d["from"]=fb;
  char tb[6]; sprintf(tb,"%04X",to);   d["to"]=tb;
  d["name"]=name; d["bd"]=text; d["dm"]=dm;
  String s; serializeJson(d,s); ws.textAll(s);
}

// Cada cliente WiFi pode ter o seu proprio nome (varios telemoveis na placa).
// Guardamos o nome por cliente WS no campo de cada conexao.
struct WsClientInfo { uint32_t cid; String name; bool used; };
#define MAX_WSCLIENTS 12
WsClientInfo wsClients[MAX_WSCLIENTS];
String getWsName(uint32_t cid) {
  for (int i=0;i<MAX_WSCLIENTS;i++) if (wsClients[i].used && wsClients[i].cid==cid) return wsClients[i].name;
  return myDeviceName;
}
void setWsName(uint32_t cid, const String& nm) {
  for (int i=0;i<MAX_WSCLIENTS;i++) if (wsClients[i].used && wsClients[i].cid==cid){wsClients[i].name=nm;return;}
  for (int i=0;i<MAX_WSCLIENTS;i++) if (!wsClients[i].used){wsClients[i].used=true;wsClients[i].cid=cid;wsClients[i].name=nm;return;}
}

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType type,
               void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    broadcastWsState(); broadcastWsPeers();
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!(info->final && info->index == 0 && info->len == len)) return;
    String msg; msg.reserve(len+1);
    for (size_t i=0;i<len;i++) msg += (char)data[i];

    StaticJsonDocument<320> d;
    if (deserializeJson(d, msg)) return;
    const char* cmd = d["c"] | "";

    if (!strcmp(cmd, "scan")) {
      // limpa peers antigos do scan e pede beacons agora
      broadcastWsPeers();
      sendBeacon();
    }
    else if (!strcmp(cmd, "host")) {
      isHost = true; hostId = myNodeId; joinedMesh = true;
      // marca o instante da reivindicacao (millis). Quem reivindicou antes vence.
      myHostClaim = millis();
      hostClaimSeen = myHostClaim;
      // autoriza todos os peers atuais por defeito
      for (int i=0;i<MAX_PEERS;i++) if (peers[i].used) peers[i].authorized = !peers[i].banned;
      broadcastWsState(); broadcastWsPeers();
      sendBeacon();
      oledStatus("MODO HOST", "Admin: " + myNodeIdHex, "");
    }
    else if (!strcmp(cmd, "join")) {
      uint16_t dst = hexToId(d["dst"] | "0000");
      hostId = dst;
      sendJoin(dst);
      broadcastWsState();
    }
    else if (!strcmp(cmd, "name")) {
      setWsName(c->id(), String((const char*)(d["name"] | "")));
      broadcastWsState();
    }
    else if (!strcmp(cmd, "msg")) {
      String name = getWsName(c->id());
      const char* dst = d["dst"] | "ALL";
      String body = String((const char*)(d["bd"] | ""));
      if (!strcmp(dst, "ALL")) sendChat(name, body, false, 0);
      else sendChat(name, body, true, hexToId(dst));
    }
    else if (!strcmp(cmd, "admin")) {
      if (isHost) {
        const char* act = d["act"] | "";
        uint16_t dst = hexToId(d["dst"] | "0000");
        PeerNode* p = findPeer(dst);
        if (p) {
          if (!strcmp(act,"BAN")) p->banned = true;
          else if (!strcmp(act,"UNBAN")) p->banned = false;
          else if (!strcmp(act,"AUTH")) { p->authorized = true; p->banned = false; }
        }
        sendAdmin(act, dst);
        broadcastWsPeers();
      }
    }
  }
}

// ===========================================================================
//                    Endpoints HTTP de download
// ===========================================================================
String historyAsTxt() {
  String out = "LoRa Mesh Chat - Historico\n";
  out += "Placa: " + myDeviceName + " (ID " + myNodeIdHex + ")\n";
  out += "------------------------------------------\n";
  int idx = (historyCount < MAX_HISTORY) ? 0 : historyHead;
  for (int n = 0; n < historyCount; n++) {
    ChatLine &c = history[(idx + n) % MAX_HISTORY];
    char tb[6]; sprintf(tb,"%04X",c.to);
    out += "[" + String(c.ts/1000) + "s] ";
    out += c.name + (c.dm ? (" -> " + String(tb) + " (privado)") : "");
    out += ": " + c.text + "\n";
  }
  return out;
}

// ===========================================================================
//                    OLED
// ===========================================================================
#if !USE_OLED
#include <Wire.h>
// Desliga o ecra OLED por completo para poupar bateria.
void powerDownOled() {
  // Heltec V3 OLED (SSD1306) em I2C: SDA=17, SCL=18, RST=21, endereco 0x3C.
  const uint8_t OLED_ADDR = 0x3C;
  const int OLED_SDA = 17, OLED_SCL = 18, OLED_RST = 21, VEXT_CTRL = 36;

  // Garante que o painel esta alimentado um instante para aceitar o comando.
  pinMode(VEXT_CTRL, OUTPUT);
  digitalWrite(VEXT_CTRL, LOW);   // LOW = Vext LIGADO (segundo doc Heltec)
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW); delay(5);
  digitalWrite(OLED_RST, HIGH); delay(5);

  Wire.begin(OLED_SDA, OLED_SCL);
  // Comando 0xAE = display OFF (entra em sleep, consumo minimo do controlador)
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);   // control byte: comando
  Wire.write(0xAE);   // DISPLAY OFF
  Wire.endTransmission();

  // Agora corta a alimentacao externa para zerar o consumo do painel.
  digitalWrite(VEXT_CTRL, HIGH);  // HIGH = Vext DESLIGADO
  // Mantem RST baixo para nao deixar o painel num estado indefinido.
  digitalWrite(OLED_RST, LOW);
}
#endif

void oledStatus(const String& l1, const String& l2, const String& l3) {
#if USE_OLED
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 12, l1.c_str());
  oled.drawStr(0, 28, l2.c_str());
  oled.drawStr(0, 44, l3.c_str());
  oled.setFont(u8g2_font_5x7_tf);
  String ip = "http://192.168.1.1";
  oled.drawStr(0, 60, ip.c_str());
  oled.sendBuffer();
#endif
}

// ===========================================================================
//                              SETUP
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  computeIdentity();

#if USE_OLED
  // Heltec V3: ligar Vext/alimentacao do OLED (GPIO36 controla Vext em algumas
  // versoes; o reset do painel e tratado pela U8g2 via pino 21).
  oled.begin();
  oledStatus("LoRa Mesh Chat", "ID: " + myNodeIdHex, "A iniciar...");
#else
  // OLED DESLIGADO (poupar bateria). Fazemos duas coisas:
  // 1) Mandar o controlador SSD1306 para sleep via I2C (apaga o painel mesmo
  //    que a alimentacao continue ligada).
  // 2) Cortar a alimentacao externa (Vext) via GPIO36. Na doc da Heltec, Vext
  //    LIGA com nivel BAIXO; portanto, para DESLIGAR pomos em nivel ALTO.
  powerDownOled();
#endif

  // ---- WiFi AP ----
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apGW, apMASK);
  WiFi.softAP(apSSID.c_str(), apPASSWORD);
  Serial.printf("[WiFi] AP '%s'  ->  http://192.168.1.1\n", apSSID.c_str());

  // ---- LoRa (SX1262) ----
  spiLoRa.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER, LORA_PREAMBLE);
  if (st == RADIOLIB_ERR_NONE) {
    Serial.println("[LoRa] SX1262 OK");
  } else {
    Serial.printf("[LoRa] FALHA init: %d (verifique pinos/antena)\n", st);
    oledStatus("LoRa FALHOU", "erro: " + String(st), "verifique antena");
  }
  radio.setDio2AsRfSwitch(true);
  radio.setDio1Action(onLoRaDio1);
  radio.startReceive();

  // ---- HTTP ----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html; charset=UTF-8", INDEX_HTML);
  });
  server.on("/history.txt", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(200, "text/plain; charset=UTF-8", historyAsTxt());
    r->addHeader("Content-Disposition", "attachment; filename=chat_history.txt");
    req->send(r);
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();

  oledStatus("Pronto. ID " + myNodeIdHex, "SSID:", apSSID);
  Serial.println("[Sistema] Pronto.");
}

// ===========================================================================
//                              LOOP
// ===========================================================================
void loop() {
  // Recepcao LoRa via flag de IRQ
  if (loraGotPacket) {
    loraGotPacket = false;
    String data;
    int st = radio.readData(data);
    if (st == RADIOLIB_ERR_NONE) {
      int rssi = (int)radio.getRSSI();
      handleLoRaPacket(data, rssi);
    }
    radio.startReceive();
  }

  // Beacon periodico (anuncio de presenca p/ scan e mesh)
  if (millis() - lastBeacon > BEACON_EVERY) {
    lastBeacon = millis();
    sendBeacon();
  }

  ws.cleanupClients();
}
