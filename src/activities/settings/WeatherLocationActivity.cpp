#include "WeatherLocationActivity.h"

#include <HalDisplay.h>

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

  static const char* optionsWithClear[] = {"Type a Location", "Use Current Location", "Turn Off Weather"};
  static const char* optionsNoClear[] = {"Type a Location", "Use Current Location"};

  menu.show(title.c_str(), hasLocation ? optionsWithClear : optionsNoClear, hasLocation ? 3 : 2, 0,
            [this](const int idx) {
              if (idx == 0) {
                handleTypeLocation();
              } else if (idx == 1) {
                handleUseCurrentLocation();
              } else {
                // "Turn Off Weather" -- only reachable when hasLocation was
                // true, so index 2 always means this option.
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
          WEATHER_LOCATION.setLocation(name, lat, lon);
          showToast(("Location set: " + name).c_str(), true);
        } else {
          showToast("Couldn't find that location -- check WiFi and try again", false);
        }
        finish();
      });
}

void WeatherLocationActivity::handleUseCurrentLocation() {
  showToast("Detecting location...", true);
  std::string name;
  double lat = 0.0, lon = 0.0;
  if (HomeStatusService::locateByIp(name, lat, lon)) {
    WEATHER_LOCATION.setLocation(name, lat, lon);
    showToast(("Location set: " + name).c_str(), true);
  } else {
    showToast("Couldn't detect location -- check WiFi and try again", false);
  }
  finish();
}
