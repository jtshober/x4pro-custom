#include "util/HomeStatusService.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "WeatherLocationStore.h"
#include "WifiCredentialStore.h"
#include "fontIds.h"

namespace {

// How often a fully-valid reading is allowed to go stale before tick()
// bothers reconnecting for a fresh one. Weather doesn't change minute to
// minute; the clock resync is just drift correction (the ESP32's internal
// RTC crystal is not lab-grade), not a real re-sync of anything that moves.
constexpr uint32_t WEATHER_REFRESH_INTERVAL_MS = 30UL * 60 * 1000;
constexpr uint32_t CLOCK_RESYNC_INTERVAL_MS = 12UL * 60 * 60 * 1000;
// Throttles tick() itself so calling it every frame costs nothing beyond
// this many millis() reads between real attempts.
constexpr uint32_t TICK_MIN_GAP_MS = 5000;
// Per-network connect budget, and how many saved networks are worth trying
// before giving up for this tick -- bounds the worst case to a few seconds
// of stall on a screen that's otherwise idle, not the much longer full-scan
// KOReaderAutoSync can justify for an explicit sync action.
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 7000;
constexpr size_t MAX_NETWORKS_TRIED = 2;
// Same heap floor ContinueMetadataEnricher and HttpDownloader gate their own
// TLS handshakes on for this hardware's wolfSSL stack.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

uint32_t lastTickMs = 0;
uint32_t lastWeatherFetchMs = 0;
uint32_t lastClockSyncMs = 0;
bool clockSynced = false;
bool weatherValid = false;
int cachedTempF = 0;
std::string cachedCondition;
long cachedUtcOffsetSeconds = 0;

bool insufficientHeap() {
  return ESP.getFreeHeap() < MIN_FREE_FOR_TLS || ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS;
}

// Best-effort mapping from Open-Meteo's WMO weather codes to the same
// wording Apple's iPhone Weather app uses, for continuity with the user's
// other devices/apps (see Apple's own support page on weather icons).
// Open-Meteo doesn't expose Apple's icon set directly, so this is a
// hand-built table, not a live lookup -- reasonable coverage, not a perfect
// match for every one of Apple's more exotic conditions (hurricanes,
// sandstorms, etc.), which this device has no data source for anyway.
std::string weatherConditionText(const int code, const bool isDay) {
  switch (code) {
    case 0:
      return isDay ? "Sunny" : "Clear";
    case 1:
      return isDay ? "Mostly Sunny" : "Mostly Clear";
    case 2:
      return "Partly Cloudy";
    case 3:
      return "Cloudy";
    case 45:
    case 48:
      return "Foggy";
    case 51:
    case 53:
    case 55:
      return "Drizzle";
    case 56:
    case 57:
      return "Freezing Drizzle";
    case 61:
    case 63:
    case 80:
    case 81:
      return "Rain";
    case 65:
    case 82:
      return "Heavy Rain";
    case 66:
    case 67:
      return "Freezing Rain";
    case 71:
    case 73:
    case 85:
      return "Snow";
    case 75:
    case 86:
      return "Heavy Snow";
    case 77:
      return "Flurries";
    case 95:
      return "Thunderstorms";
    case 96:
    case 99:
      return "Strong Storms";
    default:
      return "Cloudy";
  }
}

// Percent-encodes everything except unreserved characters -- same small
// helper ContinueMetadataEnricher already carries for its own Open Library
// query, duplicated here rather than shared across a lib/src boundary for
// one four-line function.
std::string urlEncode(const std::string& s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Tries each saved network in turn (last-connected first), a bounded
// WIFI_CONNECT_TIMEOUT_MS each, up to MAX_NETWORKS_TRIED networks. No-op and
// returns true immediately if something else already has WiFi up. Returns
// false, quietly, if nothing is in range or no networks are saved at all --
// tick() just tries again next interval; this never surfaces an error
// anywhere, unlike an explicit user-initiated connect.
bool ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (insufficientHeap()) return false;

  WIFI_STORE.loadFromFile();
  std::vector<std::pair<std::string, std::string>> toTry;
  const std::string preferred = WIFI_STORE.getLastConnectedSsid();
  if (!preferred.empty()) {
    if (const auto cred = WIFI_STORE.findCredential(preferred)) {
      toTry.emplace_back(cred->ssid, cred->password);
    }
  }
  const size_t count = WIFI_STORE.getCredentialCount();
  for (size_t i = 0; i < count && toTry.size() < MAX_NETWORKS_TRIED; i++) {
    if (const auto cred = WIFI_STORE.getCredentialAt(i)) {
      if (cred->ssid != preferred) toTry.emplace_back(cred->ssid, cred->password);
    }
  }
  if (toTry.empty()) return false;

  for (const auto& network : toTry) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(network.first.c_str(), network.second.empty() ? nullptr : network.second.c_str());
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      WIFI_STORE.setLastConnectedSsid(network.first);
      return true;
    }
  }
  return false;
}

bool trySyncClock() {
  // UTC epoch only -- local time is derived at display time using whatever
  // UTC offset the last weather fetch supplied (see getStatusLineText).
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) return false;
  clockSynced = true;
  lastClockSyncMs = millis();
  return true;
}

// One GET against Open-Meteo's forecast endpoint for the configured
// location. timezone=auto makes it resolve and return utc_offset_seconds
// for us, DST-correct for the current date, with no separate timezone
// lookup of our own.
bool tryFetchWeather() {
  if (!WEATHER_LOCATION.hasLocation()) return false;
  if (insufficientHeap()) return false;

  char urlBuf[256];
  snprintf(urlBuf, sizeof(urlBuf),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day&temperature_unit=fahrenheit&timezone=auto",
           WEATHER_LOCATION.getLatitude(), WEATHER_LOCATION.getLongitude());

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(std::string(urlBuf))) return false;
  http.addHeader("Accept", "application/json");
  const int httpCode = http.GET();
  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getString().c_str());
  http.end();
  if (err) {
    LOG_ERR("WXCLK", "Weather JSON parse failed: %s", err.c_str());
    return false;
  }

  const JsonObjectConst current = doc["current"].as<JsonObjectConst>();
  if (current.isNull()) return false;
  const bool hasTemp = !current["temperature_2m"].isNull();
  const bool hasCode = !current["weather_code"].isNull();
  if (!hasTemp || !hasCode) return false;

  cachedTempF = static_cast<int>(std::lround(current["temperature_2m"].as<double>()));
  const int weatherCode = current["weather_code"].as<int>();
  const bool isDay = (current["is_day"] | 1) != 0;
  cachedCondition = weatherConditionText(weatherCode, isDay);
  cachedUtcOffsetSeconds = doc["utc_offset_seconds"] | 0L;
  weatherValid = true;
  lastWeatherFetchMs = millis();
  return true;
}

}  // namespace

namespace HomeStatusService {

void tick() {
  // WeatherLocationStore, like WifiCredentialStore, is never loaded at boot
  // on its own -- a location saved on a previous boot needs one explicit
  // loadFromFile() before hasLocation()/getLocationName() reflect it. Done
  // once per boot here, the very first time anything asks this service to
  // do work, so it's correct regardless of whether Continue or Settings >
  // Weather Location happens to be visited first this session.
  static bool locationLoaded = false;
  if (!locationLoaded) {
    WEATHER_LOCATION.loadFromFile();
    locationLoaded = true;
  }

  const uint32_t now = millis();
  if (lastTickMs != 0 && now - lastTickMs < TICK_MIN_GAP_MS) return;
  lastTickMs = now;

  const bool clockDue = !clockSynced || (now - lastClockSyncMs > CLOCK_RESYNC_INTERVAL_MS);
  const bool weatherDue =
      WEATHER_LOCATION.hasLocation() && (!weatherValid || (now - lastWeatherFetchMs > WEATHER_REFRESH_INTERVAL_MS));
  if (!clockDue && !weatherDue) return;

  if (!ensureWifiConnected()) return;  // Try again next tick; nothing to report anywhere for this.

  if (clockDue) trySyncClock();
  if (weatherDue) tryFetchWeather();
}

bool isReady() { return clockSynced && weatherValid; }

bool getStatusLineText(std::string& out) {
  if (!isReady()) return false;

  const time_t nowUtc = time(nullptr);
  const time_t localT = nowUtc + cachedUtcOffsetSeconds;
  struct tm tmLocal;
  gmtime_r(&localT, &tmLocal);
  char timeBuf[16];
  strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &tmLocal);
  std::string timeText = timeBuf;
  // No leading zero on the hour ("2:45 PM", not "02:45 PM") -- matches how
  // a phone's own status bar clock reads.
  if (timeText.size() > 1 && timeText.front() == '0') timeText.erase(timeText.begin());

  // Degree sign (U+00B0): the builtin fonts here are generated from iA
  // Writer Mono S's own glyph set with no codepoint restriction, which
  // covers Latin-1 Supplement, so this renders rather than showing a
  // missing-glyph box -- verified against the same font conversion used
  // for every other builtin UI string.
  out = timeText + " | " + cachedCondition + ", " + std::to_string(cachedTempF) + "\xC2\xB0";
  return true;
}

void drawTitleBarStatus(const GfxRenderer& renderer, const int x, const int y, const bool rightAligned) {
  std::string text;
  if (!getStatusLineText(text)) return;
  const int drawX = rightAligned ? x - renderer.getTextWidth(SMALL_FONT_ID, text.c_str()) : x;
  renderer.drawText(SMALL_FONT_ID, drawX, y, text.c_str());
}

bool geocodeLocation(const std::string& query, std::string& outName, double& outLat, double& outLon) {
  if (query.empty()) return false;
  if (!ensureWifiConnected()) return false;
  if (insufficientHeap()) return false;

  const std::string url =
      "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" + urlEncode(query);

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) return false;
  http.addHeader("Accept", "application/json");
  const int httpCode = http.GET();
  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getString().c_str());
  http.end();
  if (err) return false;

  const JsonArrayConst results = doc["results"].as<JsonArrayConst>();
  if (results.isNull() || results.size() == 0) return false;
  const JsonObjectConst first = results[0];
  const char* name = first["name"].as<const char*>();
  if (!name || first["latitude"].isNull() || first["longitude"].isNull()) return false;

  std::string display = name;
  if (const char* admin1 = first["admin1"].as<const char*>()) {
    display += ", ";
    display += admin1;
  } else if (const char* country = first["country"].as<const char*>()) {
    display += ", ";
    display += country;
  }

  outName = display;
  outLat = first["latitude"].as<double>();
  outLon = first["longitude"].as<double>();
  return true;
}

bool locateByIp(std::string& outName, double& outLat, double& outLon) {
  if (!ensureWifiConnected()) return false;
  if (insufficientHeap()) return false;

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin("https://ipapi.co/json/")) return false;
  http.addHeader("Accept", "application/json");
  const int httpCode = http.GET();
  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getString().c_str());
  http.end();
  if (err) return false;
  if (!doc["error"].isNull() && doc["error"].as<bool>()) return false;
  if (doc["latitude"].isNull() || doc["longitude"].isNull()) return false;

  const char* city = doc["city"].as<const char*>();
  const char* region = doc["region"].as<const char*>();
  std::string display = city ? city : "Current Location";
  if (region) {
    display += ", ";
    display += region;
  }

  outName = display;
  outLat = doc["latitude"].as<double>();
  outLon = doc["longitude"].as<double>();
  return true;
}

}  // namespace HomeStatusService
