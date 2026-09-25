#pragma once
#include <string>

class GfxRenderer;

// Drives the shell's title-bar clock + weather: an "organic" (redraws only
// when something else already repaints the header -- never a forced timer
// refresh) readout fed by a lightweight periodic background check.
//
// This bundles two concerns:
//
//  - A software clock. The X4 Pro has no DS3231 hardware RTC -- that clock
//    feature (CrossPointSettings::statusBarClock, HalClock) is gated behind
//    halClock.isAvailable(), which is X3-only. Instead, this uses the
//    ESP32-S3's own always-on internal RTC timer (the same one <time.h>'s
//    time()/configTime() already use under the hood), synced from NTP the
//    first time WiFi is actually available, and periodically thereafter to
//    correct drift. That timer keeps running across sleep/wake as long as
//    the device has power; only a full battery-dead or hard-reset loses it,
//    at which point it simply re-syncs the next time WiFi comes up.
//
//    Deliberately no boot-time guess: the title bar shows nothing at all
//    until a real sync happens -- no clock, no weather -- per how this was
//    asked to behave. In practice that's essentially immediate: opening or
//    closing a book already opens WiFi to push a KOSync position (see
//    EpubReaderActivity), and Book Server browsing already needs the network
//    up too (see OpdsBookBrowserActivity) -- both piggyback an immediate
//    clock+weather sync onto that connection via refreshLocationAndClockOnce()
//    below.
//
//  - Current weather for whatever location Settings > Weather Location has
//    configured (see WeatherLocationStore), from Open-Meteo -- free, no API
//    key. Its response also carries the UTC offset for that location, which
//    doubles as this feature's timezone source, so the clock intentionally
//    doesn't show local time until a weather fetch has supplied one -- no
//    separate timezone lookup needed.
//
// This service NEVER opens a WiFi connection just to get a clock/weather
// reading -- not from tick(), not from refreshLocationAndClockOnce(), not
// from switching to the Books or Settings tab. Both of those only ever
// piggyback a connection something else already opened for its own reason
// (see wifiAlreadyConnected() in the .cpp for tick()'s own history of why:
// an earlier version opened its own connection and a flaky network turned
// that into a recurring 10-20 second UI freeze). The two genuine exceptions
// are geocodeLocation()/locateByIp()/refreshWeatherNow() below, which DO open
// their own short-lived connection -- but only ever in response to an
// explicit Settings > Weather Location action, never passively.
namespace HomeStatusService {

// Cheap on almost every call (a couple of millis() comparisons); does real
// work only when a refresh is actually due. Safe to call every frame --
// same pattern as ContinueMetadataEnricher::tryEnrichIfOnline().
void tick();

// True once the clock has actually synced via NTP this boot -- false the
// entire time between boot and the first real network sync (see this file's
// header comment: there is deliberately no boot-time guess). The clock and
// weather are independent: the clock shows as soon as it has synced and has
// SOME UTC offset (a fresh weather fetch's if there is one, else the
// manually-set Settings > Weather Location > Set Time Zone offset -- see the
// .cpp), and weather rides alongside it only when a location is configured
// and actually reachable.
bool isReady();

// "2:45 PM | Mostly Cloudy, 72°" when weather is available, or just
// "2:45 PM" when it isn't (no location configured, or the last fetch
// failed) -- the clock always shows on its own once isReady() is true.
// Returns false (leaving out untouched) only if the clock hasn't synced yet
// this boot. The temperature is always Fahrenheit and never carries a
// trailing unit letter, per how this was asked to be shown.
bool getStatusLineText(std::string& out);

// True at most once per real-world minute rollover (and only once the
// clock has synced) -- purely a local time() check, no network, no SD
// access. Meant to be called from the shell's own loop() so
// it can trigger a plain requestUpdate() when this returns true: a cheap,
// already-cached repaint (the same FAST_REFRESH pass a tab switch already
// does) is enough to keep the visible clock accurate to the minute even
// while the person isn't touching anything, without ever polling WiFi or
// doing real work on a timer. Returns false on every other call, so calling
// it every frame costs nothing beyond the occasional true.
bool clockMinuteChanged();

// Draws the status line (see getStatusLineText) at (x, y) in SMALL_FONT_ID
// -- the same font the header already uses for the battery percentage and
// the reader status bar's own clock, so this reads as the same family of
// chrome. Does nothing at all if isReady() is false: no placeholder, no
// flash -- it simply isn't there until there's something real to show,
// which is the "organic" behavior that was asked for.
//
// If rightAligned is true, x is the RIGHT edge to align the text against
// (for a header whose battery sits on the left, so this needs to sit on
// the right instead).
void drawTitleBarStatus(const GfxRenderer& renderer, int x, int y, bool rightAligned = false);

// Blocking lookups used by WeatherLocationActivity's Settings flow (never
// called from tick()): each briefly opens a WiFi connection if one isn't up
// already, the same way tick() does, then makes one HTTPS request. Return
// false on any failure (no saved network in range, request failed, no
// match) -- the caller should tell the user to try again rather than
// partially applying a location.
bool geocodeLocation(const std::string& query, std::string& outName, double& outLat, double& outLon);
bool locateByIp(std::string& outName, double& outLat, double& outLon);

// Forces an immediate weather fetch (and clock sync, if not already synced)
// for whatever location is currently configured. Meant to be called right
// after geocodeLocation()/locateByIp() sets a new location, while that same
// connection is still up -- without this, a freshly typed-in or re-detected
// location doesn't actually show its own weather/clock until the next
// passive tick() refresh happens to run (up to WEATHER_REFRESH_INTERVAL_MS
// later, and only then if WiFi happens to already be up for something else).
// Ignores the once-per-boot gate entirely -- this is an explicit action, not
// the passive auto-refresh, so it always re-fetches. A no-op if WiFi isn't
// actually up (caller already handled that failure).
void refreshWeatherNow();

// True as long as the one-time automatic per-boot location/clock/weather
// piggyback below hasn't succeeded yet this boot -- true for both auto and
// manual location, since a manual location still needs its clock and weather
// synced at least once. Purely informational (refreshLocationAndClockOnce()
// is already safe to call unconditionally); callers use it only to skip a
// pointless call once this boot's job is done.
bool wouldAttemptBootRefresh();

// Piggyback-only, same rule as tick(): NEVER opens a WiFi connection of its
// own, full stop -- a no-op whenever WiFi isn't already up for some other
// reason. Meant to be called opportunistically at every point that already
// has (or might have) a real network connection open for its own purpose --
// opening or closing a book (KOSync position push -- see
// EpubReaderActivity), Book Server browsing (see OpdsBookBrowserActivity) --
// NEVER from simply switching to the Books or Settings tab, which must stay
// entirely offline. While a connection happens to be up: re-detects location
// by IP (skipped entirely if the saved location is manual), syncs the clock,
// and fetches weather for whatever location (auto-detected or manually
// typed) ends up configured. Safe to call from anywhere, any number of
// times: an instant no-op both before WiFi is actually up and, once it
// succeeds, for the rest of the boot regardless of outcome.
void refreshLocationAndClockOnce();

}  // namespace HomeStatusService
