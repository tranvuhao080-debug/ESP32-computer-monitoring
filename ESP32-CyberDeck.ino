#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ================== CẤU HÌNH WIFI ==================
const char* WIFI_SSID = "nubia";
const char* WIFI_PASS = "0366233903";
const uint16_t TCP_PORT = 5000;
// Muốn IP cố định (khỏi phải dò IP) thì bỏ comment 3 dòng dưới và sửa cho đúng mạng:
// IPAddress STATIC_IP(192, 168, 1, 50), GATEWAY(192, 168, 1, 1), SUBNET(255, 255, 255, 0);
// =====================================================

WiFiServer server(TCP_PORT);
WiFiClient client;

#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    21
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_SPI_HZ 40000000   // nếu màn bị nhiễu hạt thì hạ xuống 27000000

const int BUTTON_PIN = 16;

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);   // SPI phần cứng

#define CYBER_BG     0x0000
#define CYBER_CYAN   0x07FF
#define CYBER_PINK   0xF81F
#define CYBER_GREEN  0x07E0
#define CYBER_YELLOW 0xFFE0
#define CYBER_GREY   0x2104

int currentScreen = 0;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

StaticJsonDocument<512> globalDoc;
bool hasData = false;
bool linkUp = false;

const int HISTORY_SIZE = 36;
float cpuHistory[HISTORY_SIZE] = {0};
float ramHistory[HISTORY_SIZE] = {0};
float gpuHistory[HISTORY_SIZE] = {0};
float netDownHistory[HISTORY_SIZE] = {0};
float netUpHistory[HISTORY_SIZE] = {0};

// ---------------- Giao thức nhị phân ----------------
// 0xFF | type(1B) | length(2B big-endian) | payload | checksum(1B)
#define MAGIC     0xFF
#define TYPE_JSON 1
#define TYPE_IMG  2
#define IMG_W 144
#define IMG_H 74
#define IMG_BYTES (IMG_W * IMG_H * 2)   // 21312 bytes
#define JSON_MAX  480

enum RxState { WAIT_MAGIC, WAIT_TYPE, WAIT_LEN_H, WAIT_LEN_L, WAIT_PAYLOAD, WAIT_CSUM };
RxState rxState = WAIT_MAGIC;
uint8_t rxType = 0;
uint16_t rxLen = 0;
uint16_t rxPos = 0;
unsigned long rxLastByte = 0;
uint8_t rxSum = 0;
uint32_t goodFrames = 0, badFrames = 0;

static char jsonBuf[JSON_MAX + 1];
static uint16_t imgBuf[IMG_W * IMG_H];

void pushHistory(float val, float hist[]) {
  for (int i = 0; i < HISTORY_SIZE - 1; i++) hist[i] = hist[i + 1];
  hist[HISTORY_SIZE - 1] = val;
}

// Báo màn hình hiện tại cho PC: PC chỉ chụp + stream ảnh khi đang ở màn 6.
// Qua TCP việc này an toàn nên bật.
void reportScreen() {
  if (client && client.connected()) client.write((uint8_t)currentScreen);
}

void drawLinkLine() {
  if (currentScreen != 0) return;
  tft.fillRect(6, 96, 148, 8, CYBER_BG);
  tft.setCursor(6, 96);
  tft.setTextSize(1);
  if (linkUp) {
    tft.setTextColor(CYBER_GREEN, CYBER_BG);
    tft.print("PC LINKED");
  } else if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(CYBER_PINK, CYBER_BG);
    tft.print("IP "); tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(CYBER_PINK, CYBER_BG);
    tft.print("WIFI...");
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(500);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(50);
  digitalWrite(TFT_RST, LOW);
  delay(50);
  digitalWrite(TFT_RST, HIGH);
  delay(150);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(TFT_SPI_HZ);   // nếu báo lỗi biên dịch: xóa dòng này (mặc định ESP32 đã nhanh)
  tft.setRotation(1);
  tft.fillScreen(CYBER_BG);
  drawStaticUI();
  drawLinkLine();

  // ---- WiFi ----
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // QUAN TRỌNG: tắt tiết kiệm điện WiFi để giảm trễ/giật
  WiFi.setAutoReconnect(true);
  // WiFi.config(STATIC_IP, GATEWAY, SUBNET);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
    MDNS.begin("cyberdeck");     // truy cập được bằng cyberdeck.local nếu PC hỗ trợ mDNS
  } else {
    Serial.println("WiFi chua ket noi duoc (se tu thu lai)");
  }

  server.begin();
  server.setNoDelay(true);
  drawLinkLine();
}

void drawBar(int x, int y, int w, int h, float percent, uint16_t color) {
  tft.drawRect(x, y, w, h, CYBER_GREY);
  int filledWidth = (int)((w - 2) * (percent / 100.0));
  if (filledWidth < 0) filledWidth = 0;
  if (filledWidth > w - 2) filledWidth = w - 2;

  tft.fillRect(x + 1, y + 1, w - 2, h - 2, CYBER_BG);
  tft.fillRect(x + 1, y + 1, filledWidth, h - 2, color);
}

void drawBtopChart(int x, int y, int w, int h, float hist[], float maxVal, uint16_t color) {
  tft.fillRect(x + 1, y + 1, w - 2, h - 2, CYBER_BG);
  tft.drawRect(x, y, w, h, CYBER_GREY);

  for (int i = 0; i < HISTORY_SIZE; i++) {
    float val = hist[i];
    if (val < 0) val = 0;
    if (val > maxVal) val = maxVal;

    int barH = (int)((val / maxVal) * (h - 4));
    int bx = x + 2 + (i * 4);

    if (bx + 3 < x + w) {
      tft.fillRect(bx, y + h - 2 - barH, 3, barH, color);
    }
  }
}

void drawStaticUI() {
  tft.fillScreen(CYBER_BG);
  tft.drawRect(0, 0, 160, 128, CYBER_PINK);
  tft.drawRect(1, 1, 158, 126, CYBER_GREY);

  tft.setTextColor(CYBER_CYAN, CYBER_BG);
  tft.setTextSize(1);
  tft.setCursor(6, 4);

  if (currentScreen == 0) {
    tft.print("DELL-3520 // OVERVIEW");
    tft.drawFastHLine(4, 14, 152, CYBER_PINK);
    tft.setCursor(6, 17);  tft.setTextColor(CYBER_GREEN, CYBER_BG); tft.print("CPU:");
    tft.setCursor(6, 47);  tft.setTextColor(CYBER_GREEN, CYBER_BG); tft.print("RAM:");
    tft.setCursor(6, 77);  tft.setTextColor(CYBER_GREEN, CYBER_BG); tft.print("iGPU:");
    tft.drawFastHLine(4, 104, 152, CYBER_GREY);
    tft.setCursor(6, 110); tft.setTextColor(CYBER_CYAN, CYBER_BG);  tft.print("SCR: MAIN [1/6]");
  }
  else if (currentScreen == 1) {
    tft.print("CPU HISTORY // BTOP");
    tft.drawFastHLine(4, 14, 152, CYBER_GREEN);
    tft.drawFastHLine(4, 104, 152, CYBER_GREY);
    tft.setCursor(6, 110); tft.setTextColor(CYBER_GREEN, CYBER_BG);  tft.print("SCR: CPU  [2/6]");
  }
  else if (currentScreen == 2) {
    tft.print("RAM HISTORY // BTOP");
    tft.drawFastHLine(4, 14, 152, CYBER_PINK);
    tft.drawFastHLine(4, 104, 152, CYBER_GREY);
    tft.setCursor(6, 110); tft.setTextColor(CYBER_PINK, CYBER_BG);   tft.print("SCR: RAM  [3/6]");
  }
  else if (currentScreen == 3) {
    tft.print("iGPU HISTORY // BTOP");
    tft.drawFastHLine(4, 14, 152, CYBER_CYAN);
    tft.drawFastHLine(4, 104, 152, CYBER_GREY);
    tft.setCursor(6, 110); tft.setTextColor(CYBER_CYAN, CYBER_BG);  tft.print("SCR: iGPU [4/6]");
  }
  else if (currentScreen == 4) {
    tft.print("NET TRAFFIC // BTOP");
    tft.drawFastHLine(4, 14, 152, CYBER_YELLOW);
    tft.drawRect(4, 17, 152, 42, CYBER_GREY);
    tft.drawRect(4, 61, 152, 42, CYBER_GREY);
    tft.drawFastHLine(4, 104, 152, CYBER_GREY);
    tft.setCursor(6, 110); tft.setTextColor(CYBER_YELLOW, CYBER_BG); tft.print("SCR: NET  [5/6]");
  }
  else if (currentScreen == 5) {
    tft.print("ARCH LINUX // DESKTOP");
    tft.drawFastHLine(4, 14, 152, CYBER_PINK);
    tft.drawRect(6, 21, 148, 78, CYBER_GREY);
    tft.drawFastHLine(4, 104, 152, CYBER_GREY);
    tft.setCursor(6, 110); tft.setTextColor(CYBER_PINK, CYBER_BG); tft.print("SCR: DSKS [6/6]");
  }
}

void updateScreenData() {
  float cpu = globalDoc["cpu"] | 0.0;
  float cpuFreq = globalDoc["cpu_freq"] | 2.0;
  float cpuPower = globalDoc["cpu_power"] | 0.0;
  float ram = globalDoc["ram"] | 0.0;
  float ramUsed = globalDoc["ram_used"] | 0.0;
  float ramTotal = globalDoc["ram_total"] | 0.0;
  float gpu = globalDoc["gpu"] | 0.0;
  float temp = globalDoc["temp"] | 0.0;
  float netDown = globalDoc["net_down"] | 0.0;
  float netUp = globalDoc["net_up"] | 0.0;

  pushHistory(cpu, cpuHistory);
  pushHistory(ram, ramHistory);
  pushHistory(gpu, gpuHistory);
  pushHistory(netDown, netDownHistory);
  pushHistory(netUp, netUpHistory);

  if (currentScreen == 0) {
    tft.fillRect(35, 17, 122, 10, CYBER_BG);
    tft.setCursor(35, 17); tft.setTextSize(1); tft.setTextColor(CYBER_YELLOW, CYBER_BG);
    tft.print(cpu, 1); tft.print("% | "); tft.print(temp, 0); tft.print("C ");
    tft.setTextColor(CYBER_CYAN, CYBER_BG); tft.print(cpuPower, 0); tft.print("W");
    drawBar(6, 29, 148, 6, cpu, CYBER_CYAN);

    tft.fillRect(35, 47, 120, 10, CYBER_BG);
    tft.setCursor(35, 47); tft.setTextColor(CYBER_YELLOW, CYBER_BG);
    tft.print(ramUsed, 1); tft.print("/"); tft.print(ramTotal, 1); tft.print("GiB ");
    tft.print(ram, 0); tft.print("%");
    drawBar(6, 59, 148, 6, ram, CYBER_PINK);

    tft.fillRect(40, 77, 115, 10, CYBER_BG);
    tft.setCursor(40, 77); tft.setTextColor(CYBER_YELLOW, CYBER_BG);
    tft.print(gpu, 1); tft.print("%");
    drawBar(6, 89, 148, 6, gpu, CYBER_GREEN);
  }
  else if (currentScreen == 1) {
    tft.fillRect(6, 17, 148, 16, CYBER_BG);
    tft.setCursor(6, 17); tft.setTextSize(1); tft.setTextColor(CYBER_GREEN, CYBER_BG);
    tft.print("CPU: "); tft.print(cpu, 1); tft.print("% ");
    tft.setTextColor(CYBER_CYAN, CYBER_BG); tft.print(cpuFreq); tft.print("GHz ");
    tft.setTextColor(CYBER_PINK, CYBER_BG); tft.print(temp, 0); tft.print("C");
    drawBtopChart(6, 36, 148, 64, cpuHistory, 100.0, CYBER_GREEN);
  }
  else if (currentScreen == 2) {
    tft.fillRect(6, 17, 148, 16, CYBER_BG);
    tft.setCursor(6, 17); tft.setTextSize(1); tft.setTextColor(CYBER_PINK, CYBER_BG);
    tft.print("RAM: "); tft.print(ramUsed, 1); tft.print("/"); tft.print(ramTotal, 1); tft.print("G (");
    tft.print(ram, 0); tft.print("%)");
    drawBtopChart(6, 36, 148, 64, ramHistory, 100.0, CYBER_PINK);
  }
  else if (currentScreen == 3) {
    tft.fillRect(6, 17, 148, 16, CYBER_BG);
    tft.setCursor(6, 17); tft.setTextSize(1); tft.setTextColor(CYBER_CYAN, CYBER_BG);
    tft.print("iGPU Intel Iris: "); tft.print(gpu, 1); tft.print("%");
    drawBtopChart(6, 36, 148, 64, gpuHistory, 100.0, CYBER_CYAN);
  }
  else if (currentScreen == 4) {
    tft.fillRect(6, 19, 148, 11, CYBER_BG);
    tft.setCursor(6, 19); tft.setTextSize(1); tft.setTextColor(CYBER_CYAN, CYBER_BG);
    tft.print("DOWN: ");
    if (netDown > 1024) { tft.print(netDown / 1024.0, 2); tft.print(" MB/s"); }
    else { tft.print(netDown, 1); tft.print(" KB/s"); }
    drawBtopChart(6, 31, 148, 26, netDownHistory, 5000.0, CYBER_CYAN);

    tft.fillRect(6, 63, 148, 11, CYBER_BG);
    tft.setCursor(6, 63); tft.setTextSize(1); tft.setTextColor(CYBER_YELLOW, CYBER_BG);
    tft.print("UP: ");
    if (netUp > 1024) { tft.print(netUp / 1024.0, 2); tft.print(" MB/s"); }
    else { tft.print(netUp, 1); tft.print(" KB/s"); }
    drawBtopChart(6, 75, 148, 26, netUpHistory, 1000.0, CYBER_YELLOW);
  }
}

void drawStats() {
  if (currentScreen != 5) return;
  tft.fillRect(100, 110, 56, 10, CYBER_BG);
  tft.setCursor(100, 110); tft.setTextSize(1);
  tft.setTextColor(CYBER_GREEN, CYBER_BG); tft.print(goodFrames);
  tft.setTextColor(CYBER_GREY, CYBER_BG);  tft.print("/");
  tft.setTextColor(CYBER_PINK, CYBER_BG);  tft.print(badFrames);
}

void handleFrame() {
  goodFrames++;
  if (rxType == TYPE_JSON) {
    jsonBuf[rxLen] = '\0';
    DeserializationError error = deserializeJson(globalDoc, jsonBuf);
    if (!error) {
      hasData = true;
      if (currentScreen != 5) updateScreenData();
    }
  }
  else if (rxType == TYPE_IMG && rxLen == IMG_BYTES) {
    if (currentScreen == 5) {
      // Dữ liệu đã là big-endian đúng định dạng màn hình -> đẩy thẳng cả khối 1 lần
      tft.startWrite();
      tft.setAddrWindow(8, 23, IMG_W, IMG_H);
      tft.writePixels(imgBuf, IMG_W * IMG_H, true, true);
      tft.endWrite();
      if ((goodFrames & 15) == 0) drawStats();   // cập nhật số đếm thưa thôi cho đỡ tốn thời gian
    }
  }
}

// Máy trạng thái đọc không chặn. Payload được đọc THEO KHỐI thay vì từng byte.
void processIncoming() {
  if (rxState != WAIT_MAGIC && millis() - rxLastByte > 1000) {
    rxState = WAIT_MAGIC;
  }

  int guard = 0;
  while (client.available() && guard++ < 256) {
    rxLastByte = millis();

    if (rxState == WAIT_PAYLOAD) {
      int need = rxLen - rxPos;
      int avail = client.available();
      int n = need < avail ? need : avail;
      uint8_t* dst = (rxType == TYPE_JSON) ? (uint8_t*)jsonBuf : (uint8_t*)imgBuf;
      int got = client.read(dst + rxPos, n);
      if (got > 0) {
        for (int i = 0; i < got; i++) rxSum += dst[rxPos + i];
        rxPos += got;
        if (rxPos >= rxLen) rxState = WAIT_CSUM;
      }
      continue;
    }

    int ib = client.read();
    if (ib < 0) break;
    uint8_t b = (uint8_t)ib;

    switch (rxState) {
      case WAIT_MAGIC:
        if (b == MAGIC) rxState = WAIT_TYPE;
        break;

      case WAIT_TYPE:
        if (b == TYPE_JSON || b == TYPE_IMG) { rxType = b; rxState = WAIT_LEN_H; }
        else rxState = (b == MAGIC) ? WAIT_TYPE : WAIT_MAGIC;
        break;

      case WAIT_LEN_H:
        rxLen = (uint16_t)b << 8;
        rxState = WAIT_LEN_L;
        break;

      case WAIT_LEN_L:
        rxLen |= b;
        rxPos = 0;
        rxSum = 0;
        if ((rxType == TYPE_JSON && rxLen > 0 && rxLen <= JSON_MAX) ||
            (rxType == TYPE_IMG && rxLen == IMG_BYTES)) {
          rxState = WAIT_PAYLOAD;
        } else {
          rxState = WAIT_MAGIC;
        }
        break;

      case WAIT_CSUM:
        rxState = WAIT_MAGIC;
        if (b == rxSum) {
          handleFrame();
        } else {
          badFrames++;
          drawStats();
        }
        break;

      default:
        break;
    }
  }
}

void loop() {
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static int prevState = HIGH;
    if (reading == LOW && prevState == HIGH) {
      currentScreen = (currentScreen + 1) % 6;
      Serial.printf("button -> screen %d\n", currentScreen);
      drawStaticUI();
      drawLinkLine();
      reportScreen();
      if (hasData && currentScreen != 5) {
        updateScreenData();
      }
    }
    prevState = reading;
  }
  lastButtonState = reading;

  // ---- Quản lý kết nối TCP ----
  bool connected = client && client.connected();
  if (!connected) {
    WiFiClient c = server.available();
    if (c) {
      client.stop();
      client = c;
      client.setNoDelay(true);
      rxState = WAIT_MAGIC;
      connected = true;
      reportScreen();
    }
  }

  if (connected != linkUp) {
    linkUp = connected;
    drawLinkLine();
  }

  if (connected) {
    processIncoming();
  } else {
    static unsigned long lastLine = 0;
    if (millis() - lastLine > 1000) {
      lastLine = millis();
      drawLinkLine();
    }
  }

  // Còn dữ liệu thì đọc tiếp ngay, chỉ nhường CPU khi rảnh
  if (!connected || !client.available()) delay(1);
}