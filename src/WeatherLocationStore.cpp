#include "WeatherLocationStore.h"

void WeatherLocationStore::toJson(JsonDocument& doc) const {
  doc["locationName"] = locationName;
  doc["latitude"] = latitude;
  doc["longitude"] = longitude;
  doc["locationSet"] = locationSet;
}

bool WeatherLocationStore::fromJson(JsonVariantConst doc) {
  locationName = doc["locationName"] | "";
  latitude = doc["latitude"] | 0.0;
  longitude = doc["longitude"] | 0.0;
  locationSet = doc["locationSet"] | false;
  return true;
}

void WeatherLocationStore::setLocation(const std::string& name, const double lat, const double lon) {
  locationName = name;
  latitude = lat;
  longitude = lon;
  locationSet = true;
  saveToFile();
}

void WeatherLocationStore::clearLocation() {
  locationName.clear();
  latitude = 0.0;
  longitude = 0.0;
  locationSet = false;
  saveToFile();
}
