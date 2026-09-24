#include "util/ContinueMetadataEnricher.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WString.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "FsHelpers.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"

namespace {

constexpr char ATTEMPTED_LOG_PATH[] = "/.crosspoint/metadata_attempted.txt";

// Same heap floor KOReaderSyncClient gates its own TLS handshakes on --
// measured for this hardware's wolfSSL stack, not a guess specific to this
// feature. A failed handshake under this floor degrades to a quiet "skip",
// same as any other failure path here.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

bool insufficientHeap() {
  return ESP.getFreeHeap() < MIN_FREE_FOR_TLS || ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS;
}

// Filename with directories and extension stripped -- no FsHelpers helper
// for this, so a small local one.
std::string filenameStem(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  return (dot == std::string::npos) ? name : name.substr(0, dot);
}

// True for an author string that carries no real information -- not just
// literally empty, but the placeholder values EPUB metadata commonly uses
// in place of a real name. Treating "Unknown" as if it were a usable author
// was the actual bug behind a renamed file still failing to match: the
// query became "<bad title> Unknown" instead of falling back to the (good)
// filename.
bool isGenericAuthor(const std::string& author) {
  std::string lower = author;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return std::tolower(c); });
  return lower.empty() || lower == "unknown" || lower == "unknown author" || lower == "n/a" || lower == "anonymous" ||
        lower == "various" || lower == "unspecified";
}

// Title/author that look like they came from the filename rather than real
// EPUB metadata: no author at all, or a title that's literally just the
// filename, or a short no-space token (an acronym like "JPHEC"). Simple
// heuristics, not a guarantee -- a genuinely one-word or acronym-style real
// title would also trip this and get looked up unnecessarily, which is
// harmless (Open Library just won't find a confident match).
bool looksLikeRawFilename(const RecentBook& book) {
  if (isGenericAuthor(book.author)) return true;
  if (book.title == filenameStem(book.path)) return true;
  if (book.title.find(' ') == std::string::npos && book.title.size() < 24) return true;
  return false;
}

// The attempted-books log is tiny (recents caps at 10 books, so this never
// holds more than a handful of short paths), so it's read and rewritten
// whole each time via Storage's plain buffer/String helpers -- the same
// ones used elsewhere in this codebase -- rather than needing true append
// support from the storage layer.
constexpr size_t ATTEMPTED_LOG_MAX_BYTES = 4096;

std::string readAttemptedLog() {
  char buffer[ATTEMPTED_LOG_MAX_BYTES];
  const size_t read = Storage.readFileToBuffer(ATTEMPTED_LOG_PATH, buffer, sizeof(buffer) - 1);
  buffer[read] = '\0';
  return std::string(buffer, read);
}

// Every book is attempted at most once, success or failure -- one path per
// line in this log.
bool alreadyAttempted(const std::string& path) {
  if (!Storage.exists(ATTEMPTED_LOG_PATH)) return false;
  const std::string log = readAttemptedLog();
  size_t pos = 0;
  while (pos < log.size()) {
    const size_t eol = log.find('\n', pos);
    const std::string line = log.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    if (line == path) return true;
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return false;
}

void markAttempted(const std::string& path) {
  Storage.ensureDirectoryExists("/.crosspoint");
  std::string log = Storage.exists(ATTEMPTED_LOG_PATH) ? readAttemptedLog() : std::string();
  if (log.size() + path.size() + 1 > ATTEMPTED_LOG_MAX_BYTES) {
    log.clear();  // Log somehow grew past its budget -- start over rather than silently stop recording.
  }
  log += path;
  log += '\n';
  if (!Storage.writeFile(ATTEMPTED_LOG_PATH, String(log.c_str()))) {
    LOG_ERR("META", "Could not write %s to record attempt", ATTEMPTED_LOG_PATH);
  }
}

// Percent-encodes everything except unreserved characters -- plain, no
// dependency on a URL-encoding helper elsewhere in the codebase.
std::string urlEncode(const std::string& s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// A cover that decodes noticeably shorter than the target thumb height --
// James Potter's case: an old cover pulled straight from the EPUB at only
// about 3/4 the size it should be. Below this fraction of the target, it's
// treated the same as having no cover at all, and gets replaced by whatever
// Open Library has instead. Deliberately not exact-match (1.0f): a cover a
// few pixels short of the target from ordinary aspect-fit rounding is not
// what this is meant to catch.
constexpr float UNDERSIZED_COVER_THRESHOLD = 0.85f;

// True if book's existing cover decodes shorter than
// UNDERSIZED_COVER_THRESHOLD of the target thumb height. False (leave it
// alone) for anything that fails to open or parse -- this only ever
// broadens what counts as "needs a cover", never narrows the safety of the
// empty-path check it sits alongside.
bool isCoverUndersized(const RecentBook& book) {
  if (book.coverBmpPath.empty()) return false;  // "missing" is its own, separate case.
  const int targetHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  const std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, targetHeight);
  HalFile file;
  if (!Storage.openFileForRead("META", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getHeight() <= 0) return false;
  return bitmap.getHeight() < static_cast<int>(targetHeight * UNDERSIZED_COVER_THRESHOLD);
}

// One search against Open Library. Returns false on any failure (network,
// heap, no match, malformed response) -- every failure is treated the same
// by the caller: quietly give up, mark the book attempted, move on.
bool queryOpenLibrary(const std::string& query, std::string& outTitle, std::string& outAuthor, int& outCoverId) {
  if (insufficientHeap()) {
    LOG_DBG("META", "Skipping Open Library lookup: insufficient heap for TLS");
    return false;
  }

  const std::string url = "https://openlibrary.org/search.json?limit=1&fields=title,author_name&q=" +
                          urlEncode(query);
  LOG_DBG("META", "Looking up: %s", url.c_str());

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("META", "Bad URL: %s", url.c_str());
    return false;
  }
  http.addHeader("Accept", "application/json");
  const int httpCode = http.GET();
  if (httpCode < 200 || httpCode >= 300) {
    LOG_DBG("META", "Open Library response: %d", httpCode);
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, http.getString().c_str());
  http.end();
  if (error) {
    LOG_ERR("META", "JSON parse failed: %s", error.c_str());
    return false;
  }

  const JsonArrayConst docs = doc["docs"].as<JsonArrayConst>();
  if (docs.isNull() || docs.size() == 0) return false;
  const JsonObjectConst first = docs[0];
  const char* title = first["title"].as<const char*>();
  const JsonArrayConst authors = first["author_name"].as<JsonArrayConst>();
  if (!title || authors.isNull() || authors.size() == 0) return false;
  const char* author = authors[0].as<const char*>();
  if (!author) return false;

  outTitle = title;
  outAuthor = author;
  // 0 = "no cover on file at Open Library for this edition" -- not an error,
  // just nothing to fetch. Absent entirely, cover_i.as<int>() also reads 0.
  outCoverId = first["cover_i"].as<int>();
  return !outTitle.empty() && !outAuthor.empty();
}

// Downloads Open Library's cover for coverId and converts it into a BMP this
// firmware can actually display, using the exact pipeline real EPUB covers
// already go through (same converter, same 1-bit thumb format) -- so
// whatever this produces is guaranteed compatible with renderCoverBox and
// every other place a cover gets drawn, with no changes needed there.
//
// Written under its own path, separate from wherever a book's own embedded
// cover would normally be cached: this only ever runs for a book that had
// no cover of its own, so there's no existing scheme to match, and keeping
// downloaded covers in their own namespace means they're obviously
// distinguishable (and easy to bulk-clear) from real embedded ones later.
//
// Returns the new coverBmpPath template (with the literal "[HEIGHT]"
// placeholder RecentBooksStore entries use) on success, or an empty string
// on any failure -- network, conversion, heap, all treated the same: this
// book simply keeps having no cover, exactly as before the attempt.
std::string tryDownloadCover(const RecentBook& book, const int coverId) {
  if (coverId <= 0) return "";
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_DBG("META", "Skipping cover download: insufficient heap for TLS");
    return "";
  }

  const std::string url = "https://covers.openlibrary.org/b/id/" + std::to_string(coverId) + "-L.jpg";
  constexpr char TEMP_PATH[] = "/.crosspoint/tmp_cover_download.jpg";
  Storage.ensureDirectoryExists("/.crosspoint");
  LOG_DBG("META", "Downloading cover: %s", url.c_str());
  const auto result = HttpDownloader::downloadToFile(url, TEMP_PATH);
  if (result != HttpDownloader::OK) {
    LOG_DBG("META", "Cover download failed (%d)", static_cast<int>(result));
    return "";
  }

  char sanitizedStem[64];
  FsHelpers::sanitizePathComponentForFat32(filenameStem(book.path).c_str(), sanitizedStem, sizeof(sanitizedStem));
  const std::string coverDir = "/.crosspoint/covers/downloaded";
  const std::string templatePath = coverDir + "/" + sanitizedStem + "_[HEIGHT].bmp";
  const int height = UITheme::getInstance().getMetrics().homeCoverHeight;
  const std::string finalPath = UITheme::getCoverThumbPath(templatePath, height);

  Storage.ensureDirectoryExists(coverDir.c_str());
  bool converted = false;
  {
    HalFile jpegFile;
    HalFile bmpFile;
    if (Storage.openFileForRead("META", TEMP_PATH, jpegFile) && Storage.openFileForWrite("META", finalPath, bmpFile)) {
      // Same 1-bit thumb format Epub::generateThumbBmp uses for the "fast
      // home screen rendering" path -- matches what every other cover on
      // this screen already is.
      converted = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpegFile, bmpFile, height, height);
      bmpFile.flush();
    }
  }
  Storage.remove(TEMP_PATH);

  if (!converted) {
    LOG_DBG("META", "Cover conversion failed for %s", book.path.c_str());
    Storage.remove(finalPath.c_str());
    return "";
  }
  return templatePath;
}

}  // namespace

namespace ContinueMetadataEnricher {

bool tryEnrichIfOnline(const RecentBook& book) {
  if (WiFi.status() != WL_CONNECTED) return false;  // Never opens a connection itself.
  if (!looksLikeRawFilename(book)) return false;
  if (alreadyAttempted(book.path)) return false;

  // Prefer the raw filename as the search text whenever the author looks
  // generic OR the title itself looks like the problem (matches the
  // "Unknown - JPHEC" case): a real, renamed filename is a far better query
  // than pairing a bad title with a placeholder author like "Unknown".
  const bool titleLooksBad = book.title == filenameStem(book.path) ||
                             (book.title.find(' ') == std::string::npos && book.title.size() < 24);
  const std::string query =
      (isGenericAuthor(book.author) || titleLooksBad) ? filenameStem(book.path) : book.title + " " + book.author;

  std::string cleanTitle, cleanAuthor;
  int coverId = 0;
  const bool found = queryOpenLibrary(query, cleanTitle, cleanAuthor, coverId);
  markAttempted(book.path);  // Recorded either way -- a miss shouldn't retry every time Wi-Fi is up.

  if (!found) {
    LOG_DBG("META", "No confident match for: %s", query.c_str());
    return false;
  }

  // Fills in a cover that's completely missing, or replaces one that
  // decodes noticeably undersized (see isCoverUndersized) -- but otherwise
  // leaves an existing cover alone, since a messy title doesn't mean the
  // cover art itself is wrong, and a shaky text-search match is the wrong
  // reason to swap out a cover that was already a normal size.
  std::string coverBmpPath = book.coverBmpPath;
  if (coverBmpPath.empty() || isCoverUndersized(book)) {
    const std::string downloaded = tryDownloadCover(book, coverId);
    if (!downloaded.empty()) coverBmpPath = downloaded;
  }

  LOG_DBG("META", "Cleaned metadata: %s -> %s / %s", book.path.c_str(), cleanAuthor.c_str(), cleanTitle.c_str());
  RECENT_BOOKS.updateBook(book.path, cleanTitle, cleanAuthor, coverBmpPath);
  return true;
}

}  // namespace ContinueMetadataEnricher
