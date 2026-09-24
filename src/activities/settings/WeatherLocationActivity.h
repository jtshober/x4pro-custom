#pragma once
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Settings > Weather Location. A raw Activity hosting its own OptionPopup --
// same structural twin of ConfirmationActivity/FileOptionsMenuActivity used
// elsewhere -- that chains into KeyboardEntryActivity for a typed location,
// or does a blocking IP lookup for "Use Current Location", then writes the
// result to WeatherLocationStore and finishes back to Settings. There is no
// separate result payload Settings needs back (the store itself is the
// result), so this always finishes with a plain cancelled/not-cancelled
// ActivityResult.
class WeatherLocationActivity : public Activity {
 public:
  WeatherLocationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&& lock) override;
  void loop() override;

 private:
  OptionPopup menu;

  void showToast(const char* message, bool isSuccess);
  void handleTypeLocation();
  void handleUseCurrentLocation();
};
