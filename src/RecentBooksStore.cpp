#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>

namespace {
// Filename with directories and extension stripped -- same small helper
// ContinueMetadataEnricher carries for its own use, duplicated here rather
// than shared across a lib/src boundary for one four-line function.
std::string filenameStem(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  return (dot == std::string::npos) ? name : name.substr(0, dot);
}
}  // namespace

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
    obj["metadataEnriched"] = book.metadataEnriched;
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'books' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  recentBooks.reserve(std::min(arr.size(), static_cast<size_t>(MAX_RECENT_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_RECENT_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    book.metadataEnriched = obj["metadataEnriched"] | false;
    recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", getCount());
  return true;
}

void RecentBooksStore::addBook(const std::string& path, const std::string& embeddedTitle,
                               const std::string& embeddedAuthor, const std::string& coverBmpPath) {
  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });

  // Priority order for what's shown/searched: fetched (Open Library) metadata
  // > filename > the book's own embedded metadata. A path that's already
  // metadataEnriched keeps its fetched title/author forever (until a rename
  // gives it a new path and thus a fresh, unenriched entry) -- simply
  // reopening the book must never clobber a good fetched title back to the
  // filename or embedded metadata.
  if (it != recentBooks.end() && it->metadataEnriched) {
    RecentBook enriched = *it;
    if (!coverBmpPath.empty()) enriched.coverBmpPath = coverBmpPath;
    recentBooks.erase(it);
    recentBooks.insert(recentBooks.begin(), enriched);
    if (recentBooks.size() > MAX_RECENT_BOOKS) recentBooks.resize(MAX_RECENT_BOOKS);
    saveToFile();
    return;
  }

  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Not enriched yet: the filename is trusted over whatever title happens to
  // be baked into the book's own metadata -- if the person renamed the file,
  // this is what makes the new name show up immediately, and it's also
  // exactly what ContinueMetadataEnricher looks up next (see
  // looksLikeRawFilename()'s title-equals-filename check). Falls back to the
  // embedded title only in the near-impossible case of an empty stem.
  const std::string stem = filenameStem(path);
  const std::string title = stem.empty() ? embeddedTitle : stem;
  recentBooks.insert(recentBooks.begin(), RecentBook{path, title, embeddedAuthor, coverBmpPath, false});

  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    book.metadataEnriched = true;
    saveToFile();
  }
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}
