#include "util/HomeStatusService.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cmath>
#include <ctime>
#include <string>
#include <sys/time.h>
#include <vector>

#include "CrossPointSettings.h"
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
// True once tick() has seeded the in-memory clock from
// CrossPointSettings::lastSyncedEpoch at the start of this boot, and still
// true until a real sync (trySyncClock() succeeding) supersedes it. See
// seedClockFromPersisted() and HomeStatusService.h's header comment.
bool clockSeededFromPersisted = false;
bool weatherValid = false;
int cachedTempF = 0;
std::string cachedCondition;
long cachedUtcOffsetSeconds = 0;
// Gates refreshLocationAndClockOnce() to a single attempt per boot,
// regardless of outcome -- see that function and wouldAttemptBootRefresh().
bool bootLocationRefreshAttempted = false;

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
// returns true immediately if something else already has WiFi up.
//
// Called from an explicit, user-initiated action (geocodeLocation()/
// locateByIp(), triggered from Settings > Weather Location, or from
// refreshLocationAndClockOnce()'s own one-time-per-boot call into
// locateByIp()) -- NEVER from tick()'s own background refresh. An earlier
// version of this file called this from tick() on every idle cycle of the
// home screen whenever the clock hadn't synced yet, and on a flaky WiFi
// network that meant a 10-20 second UI freeze recurring every few minutes
// forever (every button press, tap and screen render stalls for as long as
// this blocking connect-and-poll loop runs). tick() now only ever
// piggybacks a connection something else already opened (see
// wifiAlreadyConnected() below) -- the same rule ContinueMetadataEnricher
// already follows for exactly this reason -- and this function stays
// reserved for the handful of cases where blocking is actually expected:
// the person just tapped something (with a "Looking up..."/"Detecting..."
// toast already up), or the one-time boot refresh (with its own toast up
// before this ever runs -- see AppShellActivity::switchTab()).
bool ensureWifiConnectedActive() {
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
    // Fully tear down any previous attempt before starting the next one --
    // stacking WiFi.begin() calls without an intervening disconnect is the
    // specific pattern that wedged the WiFi stack during testing.
    WiFi.disconnect(true, true);
    delay(100);
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
  // Every attempt failed -- leave the radio fully disconnected rather than
  // mid-association, so whatever runs next starts clean.
  WiFi.disconnect(true, true);
  return false;
}

// The ONLY WiFi check tick() itself makes: is a connection already up for
// some other reason (reading a book that triggered a KOSync push, metadata
// cleanup, File Transfer, or Settings > Weather Location having just run
// its own explicit connect). Never opens one -- a plain status check, so
// this can never block or stall anything.
bool wifiAlreadyConnected() { return WiFi.status() == WL_CONNECTED; }

bool trySyncClock() {
  // UTC epoch only -- local time is derived at display time using whatever
  // UTC offset the last weather fetch supplied (see getStatusLineText).
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) return false;
  clockSynced = true;
  // A real sync always supersedes a boot-time guess, whether or not one was
  // ever made this boot.
  clockSeededFromPersisted = false;
  lastClockSyncMs = millis();

  // Persist so the NEXT boot can seed the clock immediately from this,
  // instead of showing nothing until network happens again -- see
  // seedClockFromPersisted() below and HomeStatusService.h's header comment.
  SETTINGS.lastSyncedEpoch = static_cast<uint32_t>(time(nullptr));
  SETTINGS.saveToFile();
  return true;
}

// Runs once, at the very start of the first tick() this boot. A no-op if
// there's nothing persisted yet (fresh install, or a full battery-dead /
// hard-reset that lost CrossPointSettings along with everything else -- see
// this file's header comment), or if a real sync has already happened this
// boot by the time this runs.
void seedClockFromPersisted() {
  if (clockSynced || clockSeededFromPersisted) return;
  if (SETTINGS.lastSyncedEpoch == 0) return;
  struct timeval tv {};
  tv.tv_sec = static_cast<time_t>(SETTINGS.lastSyncedEpoch);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  clockSeededFromPersisted = true;
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
  // Weather Location happens to be visited first this session. The clock's
  // own boot-time seed (see seedClockFromPersisted()) piggybacks on this
  // same one-time gate -- neither needs network, so both run immediately on
  // the very first call, not just once WiFi shows up.
  static bool bootInitDone = false;
  if (!bootInitDone) {
    bootInitDone = true;
    WEATHER_LOCATION.loadFromFile();
    seedClockFromPersisted();
  }

  const uint32_t now = millis();
  if (lastTickMs != 0 && now - lastTickMs < TICK_MIN_GAP_MS) return;
  lastTickMs = now;

  // Piggyback only -- see wifiAlreadyConnected()'s comment. This is what
  // makes tick() safe to call every frame from the home screen's loop():
  // when nothing else has WiFi up, this is one status flag read and tick()
  // returns immediately, no matter how often or how long the clock/weather
  // have been waiting to sync.
  if (!wifiAlreadyConnected()) return;

  const bool clockDue = !clockSynced || (now - lastClockSyncMs > CLOCK_RESYNC_INTERVAL_MS);
  const bool weatherDue =
      WEATHER_LOCATION.hasLocation() && (!weatherValid || (now - lastWeatherFetchMs > WEATHER_REFRESH_INTERVAL_MS));
  if (!clockDue && !weatherDue) return;

  if (clockDue) trySyncClock();
  if (weatherDue) tryFetchWeather();
}

bool isReady() { return clockSynced || clockSeededFromPersisted; }

namespace {
// Whatever UTC offset the clock should display with right now: a fresh
// weather fetch is authoritative when there is one (DST-correct, resolved
// from the actual configured location); otherwise the manually-set
// Settings > Weather Location > Set Time Zone offset, the same
// quarter-hour-steps-biased-by-48 encoding the X3 status-bar clock already
// uses (ClockOffsetActivity), reused here rather than inventing a second
// timezone setting. Defaults to UTC+0 (biased value 48) until set. Note
// this means weatherValid (always false at the start of a boot, until a
// fetch actually succeeds) is what decides the offset, not clockSynced /
// clockSeededFromPersisted -- so a freshly booted, seeded-but-not-yet-synced
// clock still shows in the manually configured zone, exactly like a boot
// with no persisted time at all.
long currentUtcOffsetSeconds() {
  if (weatherValid) return cachedUtcOffsetSeconds;
  return (static_cast<long>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60;
}
}  // namespace

bool getStatusLineText(std::string& out) {
  if (!clockSynced && !clockSeededFromPersisted) return false;

  const time_t nowUtc = time(nullptr);
  const time_t localT = nowUtc + currentUtcOffsetSeconds();
  struct tm tmLocal;
  gmtime_r(&localT, &tmLocal);
  char timeBuf[16];
  strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &tmLocal);
  std::string timeText = timeBuf;
  // No leading zero on the hour ("2:45 PM", not "02:45 PM") -- matches how
  // a phone's own status bar clock reads.
  if (timeText.size() > 1 && timeText.front() == '0') timeText.erase(timeText.begin());

  // Weather only when a fresh fetch has actually supplied one -- the clock
  // above shows on its own otherwise, per how this was asked to behave:
  // weather when reachable, always at least the clock when it isn't.
  if (!weatherValid) {
    out = timeText;
    return true;
  }

  // Degree sign (U+00B0): the builtin fonts here are generated from iA
  // Writer Mono S's own glyph set with no codepoint restriction, which
  // covers Latin-1 Supplement, so this renders rather than showing a
  // missing-glyph box -- verified against the same font conversion used
  // for every other builtin UI string.
  out = timeText + " | " + cachedCondition + ", " + std::to_string(cachedTempF) + "\xC2\xB0";
  return true;
}

bool clockMinuteChanged() {
  if (!clockSynced && !clockSeededFromPersisted) return false;
  // Purely local: time(), a division and a comparison -- no network, no
  // storage I/O, nothing that could ever repeat the earlier problems. Bounds
  // itself to firing at most once per real-world minute rollover.
  static long lastMinuteBucket = -1;
  const time_t nowUtc = time(nullptr);
  const time_t localT = nowUtc + currentUtcOffsetSeconds();
  const long minuteBucket = static_cast<long>(localT / 60);
  if (minuteBucket == lastMinuteBucket) return false;
  lastMinuteBucket = minuteBucket;
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
  if (!ensureWifiConnectedActive()) return false;
  if (insufficientHeap()) return false;
  // Piggyback the clock sync onto this connection while it's open -- gets
  // the title-bar clock working immediately after setting up a location,
  // rather than waiting for tick() to next happen to see WiFi already up.
  if (!clockSynced) trySyncClock();

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
  if (!ensureWifiConnectedActive()) return false;
  if (insufficientHeap()) return false;
  if (!clockSynced) trySyncClock();

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

bool wouldAttemptBootRefresh() { return !bootLocationRefreshAttempted && !WEATHER_LOCATION.isManualLocation(); }

void refreshLocationAndClockOnce() {
  if (bootLocationRefreshAttempted) return;
  bootLocationRefreshAttempted = true;

  // Never silently override a location the person typed in themselves --
  // see WeatherLocationStore::isManualLocation()'s comment.
  if (WEATHER_LOCATION.isManualLocation()) return;

  std::string name;
  double lat = 0.0, lon = 0.0;
  // locateByIp() opens WiFi itself (bounded the same way as everywhere else
  // in this file) and, as a side effect, syncs the clock too if it wasn't
  // already synced this boot.
  if (locateByIp(name, lat, lon)) {
    WEATHER_LOCATION.setLocation(name, lat, lon, /*isManual=*/false);
    tryFetchWeather();
  }
  // A failed lookup leaves whatever location (if any) was already stored
  // untouched -- same behavior as a failed "Use Current Location" in
  // Settings -- and this still won't be retried again until next boot.
}

}  // namespace HomeStatusService
