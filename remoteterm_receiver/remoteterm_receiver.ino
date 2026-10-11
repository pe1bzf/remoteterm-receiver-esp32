/*
 * ESP32-S3 RemoteTerm receiver
 * Firmware version: 1.4.11
 *
 * Connects to one of several configured WiFi networks, polls the RemoteTerm
 * JSON API, and shows the newest MeshCore message on the ILI9341 display.
 * This sketch is receive-only: it does not publish, send, or expose a web
 * server on the ESP32.
 *
 * 1.4.3: insertHistory() verdringt bij een volle buffer nu het oudste bericht
 *        in plaats van nieuwe berichten weg te gooien.
 * 1.4.4: status herstelt naar "Verbonden" zodra RemoteTerm weer bereikbaar
 *        is; scherm wordt alleen bij een statuswissel opnieuw getekend.
 * 1.4.5: displayText() verwerkt alle 2-byte UTF-8-tekens als één teken
 *        (geen dubbele ??), plus ° en Latin Extended-A (ł, ś, ž, č, ...).
 * 1.4.6: fetchChannels() bepaalt een kanaalwissel op key i.p.v. index.
 * 1.4.7: WiFi-wachtwoorden en RemoteTerm-adres verplaatst naar
 *        libraries/RemoteTermSecrets/remoteterm_secrets.h.
 * 1.4.8: HTTPS via Apache op de RPI4 met Basic Auth en vastgepind
 *        self-signed certificaat; verbinding wordt hergebruikt (keep-alive).
 * 1.4.9: JSON direct uit de stream met filter (geen grote buffer meer) en
 *        limit=20 bij de gewone poll; nieuwe berichten bleven anders weg.
 * 1.4.10: TimedBodyStream: wacht echt op de HTTPS-body (EmptyInput opgelost).
 * 1.4.11: WiFi via WiFiMulti: kiest het sterkste bekende netwerk in bereik en
 *         wisselt bij verlies; auto-reconnect uit (bleef op profiel 1 hangen).
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <LGFX_Fonts.hpp>

#define ILI9341_NAVY TFT_NAVY
#define ILI9341_WHITE TFT_WHITE
#define ILI9341_CYAN TFT_CYAN
#define ILI9341_BLACK TFT_BLACK
#define ILI9341_GREEN TFT_GREEN
#define ILI9341_YELLOW TFT_YELLOW
#define ILI9341_BLUE TFT_BLUE

const char* FIRMWARE_VERSION = "1.4.11";

// WiFi-profielen (wifiProfiles) en RemoteTerm-adres (REMOTETERM_HOST/PORT)
// staan in Arduino/libraries/RemoteTermSecrets/remoteterm_secrets.h,
// buiten de sketch, zodat deze code veilig gedeeld kan worden.
#include <remoteterm_secrets.h>

constexpr size_t WIFI_PROFILE_COUNT = sizeof(wifiProfiles) / sizeof(wifiProfiles[0]);
WiFiMulti wifiMulti;
size_t wifiProfileCount = 0;      // ingevulde profielen (zonder WIFI_-placeholders)
bool wifiWasConnected = false;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 8000;

const char* REMOTETERM_PATH = "/api/messages";
const char* REMOTETERM_CHANNELS_PATH = "/api/channels";

constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long API_POLL_MS = 5000;
constexpr unsigned long CHANNEL_POLL_MS = 60000;
constexpr size_t MAX_HISTORY = 120;
constexpr int POLL_LIMIT = 20;   // berichten per gewone poll (elke 5 s)
constexpr int FULL_LIMIT = 100;  // berichten bij kanaalwissel / terugbladeren

// LCDWiki ES3C28P/ES3N28P built-in MicroSD, used in 1-bit SDIO mode.
constexpr int SD_CLK = 38;
constexpr int SD_CMD = 40;
constexpr int SD_D0 = 39;

constexpr int LCD_CS = 10;
constexpr int LCD_DC = 46;
constexpr int LCD_SCK = 12;
constexpr int LCD_MOSI = 11;
constexpr int LCD_MISO = 13;
constexpr int LCD_BL = 45;
constexpr int TOUCH_SDA = 16;
constexpr int TOUCH_SCL = 15;
constexpr int TOUCH_RST = 18;
constexpr int TOUCH_INT = 17;
constexpr uint8_t TOUCH_ADDR = 0x38;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    auto busConfig = _bus.config();
    busConfig.spi_host = SPI2_HOST;
    busConfig.freq_write = 40000000;
    busConfig.freq_read = 16000000;
    busConfig.pin_sclk = LCD_SCK;
    busConfig.pin_mosi = LCD_MOSI;
    busConfig.pin_miso = LCD_MISO;
    busConfig.pin_dc = LCD_DC;
    _bus.config(busConfig);
    _panel.setBus(&_bus);

    auto panelConfig = _panel.config();
    panelConfig.pin_cs = LCD_CS;
    panelConfig.pin_rst = -1;
    panelConfig.pin_busy = -1;
    panelConfig.memory_width = 240;
    panelConfig.memory_height = 320;
    panelConfig.panel_width = 240;
    panelConfig.panel_height = 320;
    panelConfig.offset_x = 0;
    panelConfig.offset_y = 0;
    panelConfig.offset_rotation = 0;
    panelConfig.dlen_16bit = false;
    panelConfig.bus_shared = true;
    _panel.config(panelConfig);
    setPanel(&_panel);
  }
};

LGFX display;

unsigned long lastWifiAttempt = 0;
unsigned long lastApiPoll = 0;
String lastMessage = "Nog geen bericht";
int64_t lastMessageId = 0;
unsigned long lastChannelPoll = 0;
unsigned long lastTouch = 0;
bool remoteTermFailed = false;  // true zolang de laatste berichten-poll mislukte
bool fullFetchPending = true;   // volgende poll met FULL_LIMIT (na start/kanaalwissel)

struct ChannelInfo {
  String key;
  String name;
};

constexpr size_t MAX_CHANNELS = 12;
ChannelInfo channels[MAX_CHANNELS];
size_t channelCount = 0;
int selectedChannel = -1;
bool channelsLoaded = false;
bool channelMenuOpen = false;
bool sdReady = false;

struct MessageRecord {
  int64_t id;
  String sender;
  String channel;
  String text;
  String timestamp;
};

MessageRecord history[MAX_HISTORY];
size_t historyCount = 0;
size_t historyPosition = 0;

bool readTouch(int& x, int& y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, (uint8_t)5) != 5) return false;
  uint8_t touches = Wire.read();
  uint8_t xHigh = Wire.read(), xLow = Wire.read();
  uint8_t yHigh = Wire.read(), yLow = Wire.read();
  if (touches == 0 || touches > 2) return false;
  x = ((xHigh & 0x0F) << 8) | xLow;
  y = ((yHigh & 0x0F) << 8) | yLow;
  return x < 240 && y < 320;
}

// ASCII-vervanging voor Unicode U+0100..U+017F (Latin Extended-A:
// Pools, Tsjechisch, Slowaaks, Hongaars, Baltisch, Turks, ...).
static const char LATIN_EXT_A[] =
  "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg"   // U+0100..U+011F
  "GgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"   // U+0120..U+013F
  "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSs"   // U+0140..U+015F
  "SsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";  // U+0160..U+017F

// Vervanging voor een 2-byte UTF-8-teken (codepunt U+0080..U+07FF)
// dat niet in het C3-blok valt.
char latinFallback(uint16_t code) {
  if (code == 0xA0) return ' ';                  // harde spatie
  if (code == 0xB0 || code == 0xBA) return 'o';  // graden-teken, º
  if (code == 0xB5) return 'u';                  // micro
  if (code >= 0x100 && code <= 0x17F) return LATIN_EXT_A[code - 0x100];
  return '?';                                    // o.a. Cyrillisch, Grieks
}

String displayText(const String& input) {
  String output;
  for (unsigned int i = 0; i < input.length(); i++) {
    uint8_t c = static_cast<uint8_t>(input[i]);
    if (c < 0x80) {
      output += static_cast<char>(c);
      continue;
    }
    // Replace common UTF-8 European characters for the built-in GFX font.
    if (c == 0xC3 && i + 1 < input.length()) {
      uint8_t d = static_cast<uint8_t>(input[++i]);
      if (d >= 0x80 && d <= 0x86) output += 'A';
      else if (d == 0x87) output += 'C';
      else if (d >= 0x88 && d <= 0x8B) output += 'E';
      else if (d >= 0x8C && d <= 0x90) output += 'I';
      else if (d == 0x91) output += 'N';
      else if (d >= 0x92 && d <= 0x98) output += 'O';
      else if (d >= 0x99 && d <= 0x9D) output += 'U';
      else if (d == 0x9F) output += 's';
      else if (d >= 0xA0 && d <= 0xA6) output += 'a';
      else if (d == 0xA7) output += 'c';
      else if (d >= 0xA8 && d <= 0xAB) output += 'e';
      else if (d >= 0xAC && d <= 0xB0) output += 'i';
      else if (d == 0xB1) output += 'n';
      else if (d >= 0xB2 && d <= 0xB8) output += 'o';
      else if (d >= 0xB9 && d <= 0xBD) output += 'u';
      else output += '?';
    } else if (c >= 0xC0 && c <= 0xDF && i + 1 < input.length()) {
      // Overige 2-byte UTF-8-tekens: lead + vervolgbyte samen als één teken.
      uint8_t d = static_cast<uint8_t>(input[i + 1]);
      if ((d & 0xC0) == 0x80) {
        i++;
        uint16_t code = ((c & 0x1F) << 6) | (d & 0x3F);
        output += latinFallback(code);
      } else {
        output += '?';
      }
    } else if (c >= 0xF0 && i + 3 < input.length()) {
      output += input.substring(i, i + 4);
      i += 3;
    } else if (c >= 0xE0 && i + 2 < input.length()) {
      output += input.substring(i, i + 3);
      i += 2;
    } else output += '?';
  }
  return output;
}

int displayWidth(const String& text) {
  int width = 0;
  for (unsigned int i = 0; i < text.length(); i++) {
    uint8_t c = static_cast<uint8_t>(text[i]);
    if (c < 0x80) width++;
    else if ((c & 0xE0) == 0xC0) { width++; i++; }
    else if ((c & 0xF0) == 0xE0) { width++; i += 2; }
    else if ((c & 0xF8) == 0xF0) { width++; i += 3; }
  }
  return width;
}

void drawScreen(const char* state);

String channelFilePath() {
  if (selectedChannel < 0 || selectedChannel >= static_cast<int>(channelCount)) {
    return "/mesh_unselected.jsonl";
  }
  return String("/mesh_") + channels[selectedChannel].key + ".jsonl";
}

bool historyContains(int64_t id) {
  for (size_t i = 0; i < historyCount; i++) {
    if (history[i].id == id) return true;
  }
  return false;
}

void appendMessageToSD(const MessageRecord& record) {
  if (!sdReady || selectedChannel < 0) return;
  File file = SD_MMC.open(channelFilePath(), FILE_APPEND);
  if (!file) {
    Serial.println("SD: bestand openen voor schrijven mislukt");
    return;
  }
  JsonDocument doc;
  doc["id"] = record.id;
  doc["sender"] = record.sender;
  doc["channel"] = record.channel;
  doc["text"] = record.text;
  doc["timestamp"] = record.timestamp;
  serializeJson(doc, file);
  file.println();
  file.close();
}

void insertHistory(const MessageRecord& record, bool saveToSD) {
  if (record.id <= 0 || historyContains(record.id)) return;

  // Zoek de plek zodat history oplopend op id gesorteerd blijft.
  size_t insertAt = 0;
  while (insertAt < historyCount && history[insertAt].id < record.id) insertAt++;

  if (historyCount < MAX_HISTORY) {
    // Nog ruimte: alles vanaf insertAt een plek naar rechts schuiven.
    for (size_t i = historyCount; i > insertAt; i--) history[i] = history[i - 1];
    history[insertAt] = record;
    historyCount++;
  } else if (insertAt == 0) {
    // Buffer vol en het bericht is ouder dan alles wat we hebben
    // (terugbladeren): het nieuwste bericht valt eruit.
    for (size_t i = MAX_HISTORY - 1; i > 0; i--) history[i] = history[i - 1];
    history[0] = record;
  } else {
    // Buffer vol en het bericht is nieuwer: het oudste bericht (index 0)
    // valt eruit, de rest schuift een plek naar links.
    for (size_t i = 0; i + 1 < insertAt; i++) history[i] = history[i + 1];
    history[insertAt - 1] = record;
  }

  if (saveToSD) appendMessageToSD(record);
}

void clearHistory() {
  fullFetchPending = true;  // eerste poll na (her)laden: ruim ophalen
  historyCount = 0;
  historyPosition = 0;
  lastMessageId = 0;
  lastMessage = "Nog geen bericht";
}

void loadHistoryFromSD() {
  if (!sdReady || selectedChannel < 0) return;
  File file = SD_MMC.open(channelFilePath(), FILE_READ);
  if (!file) return;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
    MessageRecord record;
    record.id = doc["id"] | 0;
    record.sender = doc["sender"] | "onbekend";
    record.channel = doc["channel"] | "";
    record.text = doc["text"] | "";
    record.timestamp = doc["timestamp"] | "";
    insertHistory(record, false);
    if (record.id > lastMessageId) lastMessageId = record.id;
  }
  file.close();
  if (historyCount > 0) {
    historyPosition = historyCount - 1;
    lastMessage = displayText(history[historyPosition].sender + ": " + history[historyPosition].text);
  }
}

void showHistoryPosition(const char* state) {
  if (historyCount == 0) {
    lastMessage = "Nog geen bericht";
    drawScreen(state);
    return;
  }
  if (historyPosition >= historyCount) historyPosition = historyCount - 1;
  lastMessage = displayText(history[historyPosition].sender + ": " + history[historyPosition].text);
  drawScreen(state);
}

void drawWrappedMessage(const String& message, int y, int bottomY) {
  // At text size 1 normal characters are 6 pixels wide. LGFX_Fonts draws
  // selected Noto Emoji glyphs inline with the normal UTF-8 text.
  const int maxCells = 32;
  String line;
  String word;

  auto printLine = [&]() -> bool {
    if (line.length() == 0) return true;
    if (y > bottomY) return false;
    display.setCursor(20, y);
    display.print(line);
    y += 10;
    line = "";
    return y <= bottomY;
  };

  auto addWord = [&](const String& nextWord) -> bool {
    String remaining = nextWord;

    // Normally words are kept intact.  Only an exceptionally long word
    // (for example a URL) is split because it cannot fit on the display.
    while (displayWidth(remaining) > maxCells) {
      if (line.length() > 0 && !printLine()) return false;
      int cut = 0;
      int cells = 0;
      while (cut < static_cast<int>(remaining.length())) {
        uint8_t c = static_cast<uint8_t>(remaining[cut]);
        int bytes = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (cells + 1 > maxCells) break;
        cells++;
        cut += bytes;
      }
      line = remaining.substring(0, cut);
      remaining = remaining.substring(cut);
      if (!printLine()) return false;
    }

    if (remaining.length() == 0) return true;
    if (line.length() == 0) {
      line = remaining;
    } else if (displayWidth(line) + 1 + displayWidth(remaining) <= maxCells) {
      line += " ";
      line += remaining;
    } else {
      if (!printLine()) return false;
      line = remaining;
    }
    return true;
  };

  for (unsigned int i = 0; i <= message.length(); i++) {
    char c = (i < message.length()) ? message[i] : '\n';
    if (c == ' ' || c == '\n' || c == '\r') {
      if (word.length() > 0) {
        if (!addWord(word)) return;
        word = "";
      }
      if (c == '\n' && !printLine()) return;
    } else {
      word += c;
    }
  }
}

void drawScreen(const char* state) {
  display.fillScreen(ILI9341_NAVY);
  display.setTextColor(ILI9341_WHITE);
  display.setTextSize(2);
  display.setCursor(14, 12);
  display.println("MeshCore node");
  display.setTextSize(1);
  display.setCursor(14, 40);
  display.print("ESP32-S3 / RemoteTerm v");
  display.println(FIRMWARE_VERSION);
  display.drawLine(12, 56, 228, 56, ILI9341_CYAN);

  display.fillRoundRect(12, 62, 216, 30, 5, ILI9341_CYAN);
  display.setTextColor(ILI9341_BLACK);
  display.setTextSize(1);
  display.setCursor(20, 73);
  display.print("Kanaal: ");
  if (selectedChannel >= 0 && selectedChannel < static_cast<int>(channelCount)) {
    String name = channels[selectedChannel].name;
    if (name.length() > 25) name = name.substring(0, 25);
    display.println(name);
  } else {
    display.println("kanalen ophalen...");
  }

  display.setTextSize(2);
  display.setCursor(14, 102);
  display.setTextColor(ILI9341_GREEN);
  display.println(state);
  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE);
  if (WiFi.status() == WL_CONNECTED) {
    display.setCursor(14, 124);
    display.print("WiFi: ");
    display.print(WiFi.SSID());
    display.print("  ");
    display.println(WiFi.localIP());
  }

  display.drawRect(12, 136, 216, 126, ILI9341_CYAN);
  display.setCursor(20, 146);
  display.setTextColor(ILI9341_YELLOW);
  display.println("Laatste bericht:");
  display.setTextColor(ILI9341_WHITE);
  drawWrappedMessage(lastMessage, 166, 246);

  display.fillRoundRect(12, 275, 48, 32, 5, ILI9341_CYAN);
  display.fillRoundRect(66, 275, 108, 32, 5, ILI9341_CYAN);
  display.fillRoundRect(180, 275, 48, 32, 5, ILI9341_CYAN);
  display.setTextColor(ILI9341_BLACK);
  display.setTextSize(2);
  display.setCursor(28, 283);
  display.println("<");
  display.setTextSize(1);
  display.setCursor(84, 288);
  display.println("ALLEEN RX");
  display.setTextSize(2);
  display.setCursor(196, 283);
  display.println(">");

  if (channelMenuOpen) {
    display.fillRect(8, 58, 224, 204, ILI9341_BLACK);
    display.drawRect(8, 58, 224, 204, ILI9341_CYAN);
    display.setTextColor(ILI9341_YELLOW);
    display.setTextSize(1);
    display.setCursor(16, 66);
    display.println("Kies kanaal");
    const int rowHeight = 28;
    const size_t visibleChannels = channelCount < 6 ? channelCount : 6;
    for (size_t i = 0; i < visibleChannels; i++) {
      int rowY = 82 + static_cast<int>(i) * rowHeight;
      if (static_cast<int>(i) == selectedChannel) {
        display.fillRect(12, rowY - 3, 216, rowHeight - 2, ILI9341_BLUE);
      }
      display.setTextColor(ILI9341_WHITE);
      display.setCursor(18, rowY + 5);
      String name = channels[i].name;
      if (name.length() > 30) name = name.substring(0, 30);
      display.println(name);
    }
  }
}

String apiUrl(const char* path) {
  return String("https://") + REMOTETERM_HOST + ":" + String(REMOTETERM_PORT) + path;
}

// Eén HTTPS-verbinding die tussen de polls open blijft (keep-alive), zodat
// niet elke 5 s een nieuwe TLS-handshake nodig is.
WiFiClientSecure secureClient;
HTTPClient http;

// Leesbuffer rond de TLS-verbinding die écht wacht op data.
// Reden: in ESP32-core 3.3 geeft NetworkClientSecure::read() -1 terug als er
// nog geen data is, en NetworkClient::readBytes() (die ArduinoJson gebruikt)
// ziet -1 als fout en stopt direct -> EmptyInput zodra de body iets later
// binnenkomt dan de headers. Deze klasse leest via Stream::timedRead(), dat
// wel tot de time-out wacht, en leest nooit meer dan Content-Length bytes.
class TimedBodyStream : public Stream {
 public:
  TimedBodyStream(Client& client, int contentLength, unsigned long timeoutMs)
      : client_(client), remaining_(contentLength) {
    setTimeout(timeoutMs);
  }
  int available() override {
    if (pos_ < len_) return len_ - pos_;
    if (remaining_ <= 0) return 0;
    int a = client_.available();
    return a > remaining_ ? remaining_ : a;
  }
  int read() override {
    if (pos_ >= len_ && !fill()) return -1;  // geen data (nog): timedRead wacht
    return buf_[pos_++];
  }
  int peek() override {
    if (pos_ >= len_ && !fill()) return -1;
    return buf_[pos_];
  }
  size_t write(uint8_t) override { return 0; }

 private:
  bool fill() {
    if (remaining_ <= 0) return false;
    int n = client_.available();
    if (n <= 0) return false;
    if (n > static_cast<int>(sizeof(buf_))) n = sizeof(buf_);
    if (n > remaining_) n = remaining_;
    int r = client_.read(buf_, n);
    if (r <= 0) return false;
    remaining_ -= r;
    pos_ = 0;
    len_ = r;
    return true;
  }
  Client& client_;
  int remaining_;
  uint8_t buf_[512];
  int pos_ = 0;
  int len_ = 0;
};

// Haalt url op via HTTPS met Basic Auth en zet het antwoord direct in doc.
// Het antwoord wordt rechtstreeks uit de verbinding geparsed (geen grote
// tekstbuffer); filter bepaalt welke velden bewaard worden, zodat ook een
// groot antwoord weinig geheugen kost.
// Bij een verbindingsfout (bijv. een door de server gesloten keep-alive-
// verbinding) wordt één keer opnieuw geprobeerd met een verse verbinding.
bool httpsGetJson(const String& url, JsonDocument& doc, JsonDocument& filter, const char* label) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!http.begin(secureClient, url)) {
      Serial.printf("%s: begin() mislukt\n", label);
      return false;
    }
    http.setAuthorization(REMOTETERM_USER, REMOTETERM_PASS);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      DeserializationError error;
      if (http.getSize() >= 0) {
        // Bekende lengte (Content-Length): direct uit de verbinding parsen,
        // via TimedBodyStream zodat er gewacht wordt op de rest van de body.
        TimedBodyStream body(secureClient, http.getSize(), 5000);
        error = deserializeJson(doc, body, DeserializationOption::Filter(filter));
      } else {
        // Chunked antwoord: de ruwe stream bevat chunk-koppen, dus via buffer.
        String body = http.getString();
        error = deserializeJson(doc, body, DeserializationOption::Filter(filter));
      }
      http.end();
      if (error) {
        Serial.printf("%s JSON-fout: %s (vrij geheugen %u bytes)\n", label, error.c_str(),
                      static_cast<unsigned>(ESP.getFreeHeap()));
        // Niet-uitgelezen restdata zou de volgende request verstoren:
        // verbinding sluiten zodat de volgende poll vers begint.
        secureClient.stop();
        return false;
      }
      return true;
    }
    http.end();

    if (httpCode > 0) {
      // Server antwoordde, maar niet met 200 (bijv. 401 of 502).
      Serial.printf("%s HTTP-fout: %d\n", label, httpCode);
      if (httpCode == HTTP_CODE_UNAUTHORIZED) {
        Serial.println("  -> controleer REMOTETERM_USER/REMOTETERM_PASS");
      }
      return false;
    }

    // Geen HTTP-antwoord: verbindings- of TLS-fout.
    char tlsError[100] = "";
    int tlsCode = secureClient.lastError(tlsError, sizeof(tlsError));
    Serial.printf("%s verbinding mislukt (%d: %s), TLS %d: %s\n", label, httpCode,
                  HTTPClient::errorToString(httpCode).c_str(), tlsCode, tlsError);
    secureClient.stop();
  }
  return false;
}

bool fetchChannels() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String previousKey;
  if (selectedChannel >= 0 && selectedChannel < static_cast<int>(channelCount)) {
    previousKey = channels[selectedChannel].key;
  }

  JsonDocument filter;
  filter[0]["key"] = true;
  filter[0]["name"] = true;
  JsonDocument doc;
  if (!httpsGetJson(apiUrl(REMOTETERM_CHANNELS_PATH), doc, filter, "RemoteTerm kanalen")) return false;

  channelCount = 0;
  JsonArray channelArray = doc.as<JsonArray>();
  for (JsonObject channel : channelArray) {
    if (channelCount >= MAX_CHANNELS) break;
    channels[channelCount].key = channel["key"] | "";
    channels[channelCount].name = channel["name"] | "onbekend";
    channelCount++;
  }

  selectedChannel = -1;
  for (size_t i = 0; i < channelCount; i++) {
    if (channels[i].key == previousKey && previousKey.length() > 0) {
      selectedChannel = static_cast<int>(i);
      break;
    }
  }
  if (selectedChannel < 0 && channelCount > 0) selectedChannel = 0;
  // Vergelijk op kanaal-key, niet op index: een andere volgorde in de lijst
  // van RemoteTerm is geen kanaalwissel.
  String newKey = selectedChannel >= 0 ? channels[selectedChannel].key : String();
  bool selectionChanged = previousKey.length() == 0 || newKey != previousKey;
  if (selectionChanged) {
    clearHistory();
    loadHistoryFromSD();
  }
  channelsLoaded = true;
  Serial.print("RemoteTerm kanalen geladen: ");
  Serial.println(channelCount);
  drawScreen("Kanalen geladen");
  return true;
}

// WiFi via WiFiMulti: scant welke bekende netwerken in bereik zijn en
// verbindt met het sterkste. Vervangt de oude rotatie met WiFi.begin(),
// die bleef hangen op profiel 1: auto-reconnect bleef dat (onbereikbare)
// netwerk proberen, en zolang de ESP32 "aan het verbinden" is weigert hij
// een nieuwe configuratie (WiFi.begin voor profiel 2/3 mislukte stil).
void setupWiFiProfiles() {
  wifiProfileCount = 0;
  for (size_t i = 0; i < WIFI_PROFILE_COUNT; i++) {
    if (String(wifiProfiles[i].ssid).startsWith("WIFI_")) continue;  // placeholder
    wifiMulti.addAP(wifiProfiles[i].ssid, wifiProfiles[i].password);
    wifiProfileCount++;
  }
}

void connectWiFi() {
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.printf("WiFi verbonden met %s (%s, %d dBm)\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      secureClient.stop();  // oude HTTPS-verbinding hoort bij het vorige netwerk
      drawScreen("WiFi verbonden");
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi-verbinding verloren, zoeken naar bekend netwerk...");
  }
  if (millis() - lastWifiAttempt < WIFI_RETRY_MS) return;
  lastWifiAttempt = millis();

  if (wifiProfileCount == 0) {
    Serial.println("Geen ingevuld WiFi-profiel gevonden");
    drawScreen("WiFi niet ingesteld");
    return;
  }

  drawScreen("WiFi zoeken...");
  // Blokkeert kort: scan (~2-3 s) plus maximaal WIFI_CONNECT_TIMEOUT_MS.
  if (wifiMulti.run(WIFI_CONNECT_TIMEOUT_MS) != WL_CONNECTED) {
    Serial.println("Geen bekend WiFi-netwerk bereikbaar, opnieuw over 10 s");
    drawScreen("Geen WiFi in bereik");
  }
}

bool fetchMessages(bool older = false) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (selectedChannel < 0 || selectedChannel >= static_cast<int>(channelCount)) return false;

  // RemoteTerm negeert after_id/before_id en geeft altijd de nieuwste
  // berichten terug. Bij de gewone poll volstaan dus de laatste 20; alleen
  // bij terugbladeren (en na een kanaalwissel) vragen we er 100.
  const bool smallPoll = !older && !fullFetchPending;
  String url = apiUrl(REMOTETERM_PATH);
  url += smallPoll ? "?limit=" + String(POLL_LIMIT) : "?limit=" + String(FULL_LIMIT);
  url += "&type=CHAN&conversation_key=";
  url += channels[selectedChannel].key;
  if (older && historyCount > 0) {
    url += "&before_id=" + String(static_cast<long long>(history[0].id));
  } else if (lastMessageId > 0) {
    url += "&after_id=" + String(static_cast<long long>(lastMessageId));
  }

  // Alleen de velden die de sketch gebruikt; paths, rssi enz. vallen weg.
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["sender_name"] = true;
  filter[0]["sender_key"] = true;
  filter[0]["channel_name"] = true;
  filter[0]["text"] = true;
  filter[0]["sender_timestamp"] = true;
  JsonDocument doc;
  if (!httpsGetJson(url, doc, filter, "RemoteTerm berichten")) return false;

  JsonArray messages = doc.as<JsonArray>();
  int64_t newestId = lastMessageId;
  bool added = false;
  bool received = false;
  for (JsonObject message : messages) {
    int64_t id = message["id"] | 0;
    if (!older && id <= lastMessageId) continue;
    if (id > newestId) newestId = id;

    String senderName = message["sender_name"] | "";
    String senderKey = message["sender_key"] | "";
    String sender = senderName.length() > 0 ? senderName : senderKey;
    if (sender.length() == 0) sender = "onbekend";
    String channel = message["channel_name"] | "";
    String text = message["text"] | "";
    MessageRecord record;
    record.id = id;
    record.sender = sender;
    record.channel = channel;
    record.text = text;
    record.timestamp = message["sender_timestamp"] | "";
    bool wasKnown = historyContains(id);
    insertHistory(record, !wasKnown);
    added = added || !wasKnown;
    received = true;
    Serial.print("RemoteTerm bericht: ");
    Serial.println(displayText(sender + ": " + text));
  }

  if (!older) fullFetchPending = false;
  if (newestId > lastMessageId) {
    lastMessageId = newestId;
  }
  if (received && added) {
    if (older) {
      historyPosition = 0;
      showHistoryPosition("Oudere berichten");
    } else {
      historyPosition = historyCount > 0 ? historyCount - 1 : 0;
      showHistoryPosition("Bericht ontvangen");
    }
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ESP32-S3 RemoteTerm receiver gestart");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  SPI.begin(LCD_SCK, LCD_MISO, LCD_MOSI, LCD_CS);
  display.init();
  display.setRotation(2);

  // Load the common Noto Emoji groups into the LovyanGFX UTF-8 renderer.
  // Only these selected groups are compiled into the firmware.
  const emoji_png_set_t emojiSubgroups[] = {
    subgroup_FACE_SMILING_24x24,
    subgroup_FACE_SLEEPY_24x24,
    subgroup_FACE_AFFECTION_24x24,
    subgroup_HEART_24x24,
    subgroup_HAND_FINGERS_CLOSED_24x24,
    subgroup_HANDS_24x24,
    subgroup_SKY_WEATHER_24x24,
    subgroup_EVENT_24x24,
    subgroup_TRANSPORT_AIR_24x24
  };
  if (!lgfx::LGFX_Emojis::loadEmojis(&display, emojiSubgroups)) {
    Serial.println("Emoji-font laden mislukt");
  } else {
    Serial.println("Noto Emoji-font geladen");
  }

  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  sdReady = SD_MMC.begin("/sdcard", true);
  if (sdReady) {
    Serial.println("SD-kaart beschikbaar");
  } else {
    Serial.println("SD-kaart niet gevonden; RemoteTerm blijft werken zonder lokale opslag");
  }

  WiFi.mode(WIFI_STA);
  // Geen auto-reconnect: die blijft het oude (mogelijk onbereikbare) netwerk
  // proberen. WiFiMulti kiest bij verlies opnieuw het beste bekende netwerk.
  WiFi.setAutoReconnect(false);
  setupWiFiProfiles();

  // HTTPS: alleen de RPI4 met precies dit certificaat wordt vertrouwd.
  secureClient.setCACert(REMOTETERM_CA_CERT);
  http.setReuse(true);
  http.setTimeout(5000);
  http.setConnectTimeout(5000);

  // Juiste tijd voor de certificaatcontrole; synchroniseert vanzelf zodra
  // de WiFi verbonden is.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
  drawScreen("Opstarten...");
}

void loop() {
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED && !channelsLoaded && millis() - lastChannelPoll >= 1000) {
    lastChannelPoll = millis();
    fetchChannels();
  }
  if (WiFi.status() == WL_CONNECTED && channelsLoaded && millis() - lastChannelPoll >= CHANNEL_POLL_MS) {
    lastChannelPoll = millis();
    fetchChannels();
  }

  if (WiFi.status() == WL_CONNECTED && channelsLoaded && millis() - lastApiPoll >= API_POLL_MS) {
    lastApiPoll = millis();
    bool ok = fetchMessages();
    if (!ok && !remoteTermFailed) {
      // Overgang naar storing: één keer tekenen, niet elke 5 s.
      remoteTermFailed = true;
      Serial.println("RemoteTerm niet bereikbaar");
      drawScreen("RemoteTerm niet bereikbaar");
    } else if (ok && remoteTermFailed) {
      // Verbinding hersteld: foutmelding weghalen.
      remoteTermFailed = false;
      Serial.println("RemoteTerm weer bereikbaar");
      drawScreen("Verbonden");
    }
  }

  int touchX, touchY;
  if (millis() - lastTouch >= 300 && readTouch(touchX, touchY)) {
    lastTouch = millis();
    if (channelMenuOpen) {
      const int rowHeight = 28;
      if (touchX >= 8 && touchX <= 232 && touchY >= 82 && touchY < 82 + 6 * rowHeight) {
        size_t index = static_cast<size_t>((touchY - 82) / rowHeight);
        if (index < channelCount && index < 6) {
          selectedChannel = static_cast<int>(index);
          channelMenuOpen = false;
          clearHistory();
          loadHistoryFromSD();
          drawScreen("Kanaal geselecteerd");
          fetchMessages();
        }
      } else {
        channelMenuOpen = false;
        drawScreen("Kanaal actief");
      }
    } else if (touchX >= 12 && touchX <= 228 && touchY >= 62 && touchY <= 94) {
      channelMenuOpen = true;
      drawScreen("Kanaal kiezen");
    } else if (touchY >= 270 && touchY <= 315) {
      if (touchX >= 12 && touchX <= 62) {
        if (historyPosition > 0) {
          historyPosition--;
          showHistoryPosition("Vorig bericht");
        } else if (fetchMessages(true) && historyCount > 0) {
          historyPosition = 0;
          showHistoryPosition("Oudere berichten");
        }
      } else if (touchX >= 178 && touchX <= 228 && historyPosition + 1 < historyCount) {
        historyPosition++;
        showHistoryPosition("Volgend bericht");
      }
    }
  }

  delay(20);
}
