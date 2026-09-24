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
class WeatherLocationStore : public PersistableStore<WeatherLocationStore> {
 private:
  std::string locationName;
  double latitude = 0.0;
  double longitude = 0.0;
  bool locationSet = false;

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

  void setLocation(const std::string& name, double lat, double lon);
  void clearLocation();
};

#define WEATHER_LOCATION WeatherLocationStore::getInstance()
