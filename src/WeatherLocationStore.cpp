#include "WeatherLocationStore.h"

void WeatherLocationStore::toJson(JsonDocument& doc) const {
  doc["locationName"] = locationName;
  doc["latitude"] = latitude;
  doc["longitude"] = longitude;
  doc["locationSet"] = locationSet;
  doc["locationIsManual"] = locationIsManual;
}

bool WeatherLocationStore::fromJson(JsonVariantConst doc) {
  locationName = doc["locationName"] | "";
  latitude = doc["latitude"] | 0.0;
  longitude = doc["longitude"] | 0.0;
  locationSet = doc["locationSet"] | false;
  // Absent on files written before this field existed -- false (auto) is the
  // safe default there, since every location set before now came from either
  // "Use Current Location" or a typed entry a person can simply re-type once
  // if they'd rather it be sticky.
  locationIsManual = doc["locationIsManual"] | false;
  return true;
}

void WeatherLocationStore::setLocation(const std::string& name, const double lat, const double lon,
                                       const bool isManual) {
  locationName = name;
  latitude = lat;
  longitude = lon;
  locationSet = true;
  locationIsManual = isManual;
  saveToFile();
}

void WeatherLocationStore::clearLocation() {
  locationName.clear();
  latitude = 0.0;
  longitude = 0.0;
  locationSet = false;
  locationIsManual = false;
  saveToFile();
}
