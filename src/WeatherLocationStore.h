#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

// Persisted weather-location setting: either a manually typed place name
// (geocoded once via Open-Meteo's free geocoding API) or a one-time IP-based
// "current location" lookup. Both boil down to the same stored shape -- a
// display name plus a lat/lon pair -- which is all HomeStatusService needs
// to fetch weather. Re-resolved only when the user changes it from
// Settings > Weather Location, never on every boot, so this is the single
// source of truth for "where" the clock+weather feature is anchored.
//
// locationIsManual distinguishes the two ways a location gets here: true
// when the person typed it in ("Type a Location"), false when it came from
// an IP lookup ("Use Current Location", or HomeStatusService's own one-time
// per-boot auto-refresh -- see refreshLocationAndClockOnce()). That auto
// refresh checks this flag and skips entirely once it's true, so a manually
// typed location is never silently overwritten by a later IP guess. Going
// back to auto mode is a deliberate act: re-selecting "Use Current Location"
// in Settings, which sets this back to false.
class WeatherLocationStore : public PersistableStore<WeatherLocationStore> {
 private:
  std::string locationName;
  double latitude = 0.0;
  double longitude = 0.0;
  bool locationSet = false;
  bool locationIsManual = false;

  WeatherLocationStore() = default;
  friend class PersistableStore<WeatherLocationStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/weather_location.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool hasLocation() const { return locationSet; }
  const std::string& getLocationName() const { return locationName; }
  double getLatitude() const { return latitude; }
  double getLongitude() const { return longitude; }
  // True if the current location was typed in by hand rather than detected.
  // HomeStatusService's automatic per-boot location refresh checks this and
  // skips entirely when true -- see refreshLocationAndClockOnce().
  bool isManualLocation() const { return locationIsManual; }

  // isManual: true for "Type a Location", false for "Use Current Location"
  // and for HomeStatusService's own automatic IP-based refresh.
  void setLocation(const std::string& name, double lat, double lon, bool isManual);
  void clearLocation();
};

#define WEATHER_LOCATION WeatherLocationStore::getInstance()
