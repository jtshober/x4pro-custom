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
//
//    Boot-time seeding: a real NTP sync also persists its resulting time to
//    CrossPointSettings::lastSyncedEpoch. At the start of the very next
//    boot, tick() seeds the in-memory clock from that persisted value
//    (paired with the manual Settings > Weather Location > Set Time Zone
//    offset, since weather hasn't run yet this boot) so the title bar shows
//    a clock immediately instead of staying blank until network happens.
//    That seeded time is a guess extrapolated from whenever the device last
//    synced -- accurate to the second only if it wasn't off for long -- and
//    is silently replaced the moment a real sync succeeds again this boot.
//
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
//
// tick() ITSELF never opens a connection, though -- it only piggybacks one
// that's already up (see wifiAlreadyConnected() in the .cpp for why, and the
// history of the freeze that rule exists to prevent). The one deliberate,
// bounded exception is refreshLocationAndClockOnce() below, which
// AppShellActivity calls at most once per boot.
namespace HomeStatusService {

// Cheap on almost every call (a couple of millis() comparisons); does real
// work only when a refresh is actually due. Safe to call every frame --
// same pattern as ContinueMetadataEnricher::tryEnrichIfOnline().
void tick();

// True once the clock has a time to show this boot -- either a real NTP
// sync, or (until one happens) the persisted-time boot seed described above.
// The clock and weather are independent: the clock shows as soon as it has a
// time and SOME UTC offset (a fresh weather fetch's if there is one, else
// the manually-set Settings > Weather Location > Set Time Zone offset --
// see the .cpp), and weather rides alongside it only when a location is
// configured and actually reachable.
bool isReady();

// "2:45 PM | Mostly Cloudy, 72°" when weather is available, or just
// "2:45 PM" when it isn't (no location configured, or the last fetch
// failed) -- the clock always shows on its own once isReady() is true.
// Returns false (leaving out untouched) only if the clock has neither
// synced nor been seeded from a persisted previous sync this boot. The
// temperature is always Fahrenheit and never carries a trailing unit
// letter, per how this was asked to be shown.
bool getStatusLineText(std::string& out);

// True at most once per real-world minute rollover (and only once the
// clock has synced or been seeded) -- purely a local time() check, no
// network, no SD access. Meant to be called from the shell's own loop() so
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

// True as long as the one-time automatic per-boot location/clock/weather
// refresh below hasn't been attempted yet this boot AND the current
// location (if any) wasn't typed in by hand -- see
// WeatherLocationStore::isManualLocation(). AppShellActivity checks this
// BEFORE calling refreshLocationAndClockOnce(), since that call blocks for
// up to several seconds and a toast should be on screen before it starts,
// not after.
bool wouldAttemptBootRefresh();

// The one deliberate exception to "tick() never opens its own connection":
// meant to be called once, the first time the person does something more
// than glance at the Continue tab this boot (AppShellActivity calls this
// right as they switch to Books/Book Server/Settings). Opens WiFi if
// needed (same bounded budget ensureWifiConnectedActive() already uses
// elsewhere for Settings' own explicit actions -- worst case a handful of
// seconds, not the endless retry loop that used to cause a freeze), then
// while that connection is open: re-detects location by IP (skipped
// entirely if the saved location is manual), syncs the clock, and fetches
// weather. Safe to call more than once -- every call after the first this
// boot is an instant no-op regardless of whether the first attempt
// succeeded.
void refreshLocationAndClockOnce();

}  // namespace HomeStatusService
