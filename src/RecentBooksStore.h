#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;

  // True once ContinueMetadataEnricher has successfully replaced title/author
  // with a confident Open Library match for this exact path. Title/author
  // priority is: fetched metadata (this flag true) > filename > the book's
  // own embedded metadata -- see RecentBooksStore::addBook(). A rename gives
  // the book a new path, which starts this at false again, so a manually
  // edited filename is always eligible to be looked up again.
  bool metadataEnriched = false;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore : public PersistableStore<RecentBooksStore> {
 private:
  std::vector<RecentBook> recentBooks;

  static constexpr int MAX_RECENT_BOOKS = 10;

  RecentBooksStore() = default;
  ~RecentBooksStore() = default;

  friend class PersistableStore<RecentBooksStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/recent.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Add a book to the recent list (moves to front if already exists).
  // embeddedTitle/embeddedAuthor are the book's own metadata (EPUB/XTC
  // embedded, or a filename-derived title for txt/md) -- used only as the
  // last-resort display value. Whenever a fetched (Open Library) title is
  // already on file for this exact path (see metadataEnriched above), that
  // is kept instead; otherwise the path's own filename (not embeddedTitle)
  // is shown until ContinueMetadataEnricher next has a chance to look it up.
  void addBook(const std::string& path, const std::string& embeddedTitle, const std::string& embeddedAuthor,
               const std::string& coverBmpPath);

  // Called only by ContinueMetadataEnricher on a confident match -- marks the
  // entry metadataEnriched so a later addBook() (simply reopening the book)
  // never clobbers this with the filename or embedded metadata again.
  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true if an entry was found and removed (no-op + false otherwise).
  // Persistence is best-effort: a failed save is logged, not reflected in the return.
  bool removeByPath(const std::string& path);

  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Persists on success. Keeps the entry's list position (does not reorder).
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  RecentBook getDataFromBook(std::string path) const;
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
