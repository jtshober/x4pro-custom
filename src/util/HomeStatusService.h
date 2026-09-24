#pragma once
#include <string>

class GfxRenderer;

// Drives the shell's title-bar clock + weather: an "organic" (redraws only
// when something else already repaints the header -- never a forced timer
// refresh) readout fed by a lightweight periodic background check.
//
// This bundles two concerns that both need the same "is WiFi up, and if
// not, should we open it" logic:
//
//  - A software clock. The X4 Pro has no DS3231 hardware RTC -- that clock
//    feature (CrossPointSettings::statusBarClock, HalClock) is gated behind
//    halClock.isAvailable(), which is X3-only. Instead, this uses the
//    ESP32-S3's own always-on internal RTC timer (the same one <time.h>'s
//    time()/configTime() already use under the hood), seeded once per boot
//    (and re-synced periodically to correct drift) from NTP. That timer
//    keeps running across sleep/wake as long as the device has power; only
//    a full battery-dead or hard-reset loses it, at which point it simply
//    re-syncs the next time WiFi comes up, same as any first boot.
//  - Current weather for whatever location Settings > Weather Location has
//    configured (see WeatherLocationStore), from Open-Meteo -- free, no API
//    key. Its response also carries the UTC offset for that location, which
//    doubles as this feature's timezone source, so the clock intentionally
//    doesn't show local time until a weather fetch has supplied one -- no
//    separate timezone lookup needed.
//
// Unlike ContinueMetadataEnricher (which only ever piggybacks a connection
// something else already opened), tick() is allowed to open its own
// short-lived WiFi connection using saved credentials -- the same idea
// KOReaderAutoSync's own push() already relies on for KOSync -- because a
// clock/weather readout that only ever updated when the device happened to
// already be online for some other reason would go stale for days at a
// time on a reader most people leave in standby.
namespace HomeStatusService {

// Cheap on almost every call (a couple of millis() comparisons); does real
// work only when a refresh is actually due. Safe to call every frame --
// same pattern as ContinueMetadataEnricher::tryEnrichIfOnline().
void tick();

// True once the clock has been NTP-synced at least once this boot. The
// clock and weather are independent: the clock shows as soon as it has a
// time and SOME UTC offset (a fresh weather fetch's if there is one, else
// the manually-set Settings > Weather Location > Set Time Zone offset --
// see the .cpp), and weather rides alongside it only when a location is
// configured and actually reachable.
bool isReady();

// "2:45 PM | Mostly Cloudy, 72°" when weather is available, or just
// "2:45 PM" when it isn't (no location configured, or the last fetch
// failed) -- the clock always shows on its own once isReady() is true.
// Returns false (leaving out untouched) only if the clock itself has never
// synced. The temperature is always Fahrenheit and never carries a
// trailing unit letter, per how this was asked to be shown.
bool getStatusLineText(std::string& out);

// True at most once per real-world minute rollover (and only once the
// clock has synced) -- purely a local time() check, no network, no SD
// access. Meant to be called from the shell's own loop() so it can trigger
// a plain requestUpdate() when this returns true: a cheap, already-cached
// repaint (the same FAST_REFRESH pass a tab switch already does) is enough
// to keep the visible clock accurate to the minute even while the person
// isn't touching anything, without ever polling WiFi or doing real work on
// a timer. Returns false on every other call, so calling it every frame
// costs nothing beyond the occasional true.
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

}  // namespace HomeStatusService
