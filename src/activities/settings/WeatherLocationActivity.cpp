#include "WeatherLocationActivity.h"

#include <HalDisplay.h>

#include <vector>

#include "ClockOffsetActivity.h"
#include "CrossPointSettings.h"
#include "WeatherLocationStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/HomeStatusService.h"

WeatherLocationActivity::WeatherLocationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("WeatherLocation", renderer, mappedInput) {}

void WeatherLocationActivity::onEnter() {
  Activity::onEnter();

  // Settings can be the very first screen opened this boot (before Continue
  // ever ran HomeStatusService::tick(), which is where this would otherwise
  // get loaded) -- load explicitly here too so a location saved on a
  // previous boot always shows correctly.
  WEATHER_LOCATION.loadFromFile();

  // Not yet in the localization tables -- same deliberate English-only
  // deferral as the shell's tab labels and the File Browser's Edit option.
  const bool hasLocation = WEATHER_LOCATION.hasLocation();
  std::string title = "Weather Location";
  if (hasLocation) {
    title += ": " + WEATHER_LOCATION.getLocationName();
  } else {
    title += ": Not Set";
  }
  const int maxWidth = renderer.getScreenWidth() - 80;
  title = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), maxWidth, EpdFontFamily::BOLD);

  // "Set Time Zone" is always offered, independent of whether weather is
  // configured: the clock (Continue/Books/Book Server/Settings title bar)
  // works off NTP + this offset on its own, and only prefers a fresh
  // weather fetch's own (DST-aware) offset when one is available. This is
  // what keeps the clock showing even with weather unreachable or unset.
  static const std::vector<std::string> options = {"Type a Location", "Use Current Location",
                                                    "Set Time Zone (for offline clock)"};
  std::vector<std::string> shownOptions = options;
  if (hasLocation) shownOptions.push_back("Turn Off Weather");
  std::vector<const char*> optionPtrs;
  optionPtrs.reserve(shownOptions.size());
  for (const auto& opt : shownOptions) optionPtrs.push_back(opt.c_str());

  menu.show(title.c_str(), optionPtrs.data(), static_cast<int>(optionPtrs.size()), 0, [this](const int idx) {
    if (idx == 0) {
      handleTypeLocation();
    } else if (idx == 1) {
      handleUseCurrentLocation();
    } else if (idx == 2) {
      // Reuses the same offset the X3 status-bar clock stores
      // (SETTINGS.clockUtcOffsetQ) -- one timezone setting, not two.
      startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), [this](const ActivityResult&) {
        // An explicit manual choice: from here on, HomeStatusService's
        // clock uses this offset unconditionally, even over a "successful"
        // weather fetch -- see CrossPointSettings::clockOffsetForced. This
        // is what lets someone whose auto-detected location is reliably
        // wrong (e.g. certain carriers always resolving to one fixed city)
        // actually get a correct clock. Cleared again by picking a location
        // below.
        SETTINGS.clockOffsetForced = 1;
        SETTINGS.saveToFile();
        finish();
      });
    } else {
      // "Turn Off Weather" -- only reachable when hasLocation was true, so
      // index 3 always means this option.
      WEATHER_LOCATION.clearLocation();
      ActivityResult res;
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
    }
  });

  requestUpdate(true);
}

void WeatherLocationActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (menu.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void WeatherLocationActivity::loop() {
  if (menu.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Dismissed without a choice (Back or tap outside): cancel, same fallback
  // ConfirmationActivity/FileOptionsMenuActivity use.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}

void WeatherLocationActivity::showToast(const char* message, const bool isSuccess) {
  {
    RenderLock lock(*this);
    GUI.drawPopup(renderer, message);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  // Success needs only a glance; a failure (usually "couldn't find that" or
  // "check WiFi") has to stay readable long enough to actually read, same
  // split used for every other toast in this app.
  delay(isSuccess ? 700 : 1600);
}

void WeatherLocationActivity::handleTypeLocation() {
  const std::string prefill = WEATHER_LOCATION.hasLocation() ? WEATHER_LOCATION.getLocationName() : "";
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "City, State or Zip", prefill,
                                              /*maxLength=*/64, InputType::Text),
      [this](const ActivityResult& editRes) {
        if (editRes.isCancelled) {
          finish();
          return;
        }
        std::string query = std::get<KeyboardResult>(editRes.data).text;
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        while (!query.empty() && query.back() == ' ') query.pop_back();
        if (query.empty()) {
          finish();
          return;
        }

        showToast("Looking up location...", true);
        std::string name;
        double lat = 0.0, lon = 0.0;
        if (HomeStatusService::geocodeLocation(query, name, lat, lon)) {
          // Typed in by hand -- marked Manual so HomeStatusService's own
          // once-per-boot auto-refresh (see refreshLocationAndClockOnce())
          // never silently overwrites it with an IP-based guess later.
          WEATHER_LOCATION.setLocation(name, lat, lon, /*isManual=*/true);
          // Picking a location again is a vote of confidence in auto/weather
          // -- release any earlier "Set Time Zone" override so weather's own
          // (DST-correct) offset can take over again.
          SETTINGS.clockOffsetForced = 0;
          SETTINGS.saveToFile();
          showToast(("Location: " + name).c_str(), true);
        } else {
          showToast("Location Not Found", false);
        }
        finish();
      });
}

void WeatherLocationActivity::handleUseCurrentLocation() {
  showToast("Detecting location...", true);
  std::string name;
  double lat = 0.0, lon = 0.0;
  if (HomeStatusService::locateByIp(name, lat, lon)) {
    // Detected, not typed -- marked Auto, so it stays eligible for
    // HomeStatusService's once-per-boot auto-refresh going forward.
    WEATHER_LOCATION.setLocation(name, lat, lon, /*isManual=*/false);
    // Same vote-of-confidence release as the typed-location path above.
    SETTINGS.clockOffsetForced = 0;
    SETTINGS.saveToFile();
    showToast(("Location: " + name).c_str(), true);
  } else {
    showToast("Location Not Found", false);
  }
  finish();
}
