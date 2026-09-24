#pragma once

struct RecentBook;

// Best-effort cleanup of "Author - Title" text -- and, when a book has no
// cover at all, its cover art too -- for the Continue carousel, for books
// whose embedded EPUB metadata is missing or looks like a raw filename (an
// all-caps abbreviation, an empty author, etc.) rather than real metadata.
// Looks the book up on Open Library (free, no API key) and, on a match,
// overwrites the entry in RecentBooksStore with the cleaned text -- so it's
// a one-time enrichment of data already being persisted there, not a
// separate cache to keep in sync. A cover is only ever added when the book
// had none; an existing cover is never replaced, since a messy title
// doesn't mean the cover itself is wrong.
//
// Deliberately NOT wired into the KOSync auto-sync path: that stays exactly
// as fast as it already is. This piggybacks ONLY on a connection some other
// action (a sync, browsing the OPDS server, etc.) already brought up --
// it never opens Wi-Fi itself, so most of the time it's a single cheap
// status check and nothing more.
//
// Real network request, not backgrounded: the first time this actually
// fires for a given book (Wi-Fi already up, that book's metadata looks
// dirty, not already attempted), expect a brief pause -- a second or so --
// while it runs. After that, every book is attempted at most once ever
// (tracked on disk), success or failure, so this cost is never paid twice
// for the same book.
namespace ContinueMetadataEnricher {

// Call from the Continue tab's own loop(), for whichever book is currently
// centered. Cheap to call every frame: does nothing at all unless Wi-Fi is
// already connected, this book hasn't been attempted before, and its
// current title/author look like they came from a filename rather than
// real metadata.
//
// Returns true if it actually rewrote the book's entry in RecentBooksStore
// (a confident match was found) -- false in every other case, including
// every early skip. A caller that wants to tell the person something
// changed (a brief "Metadata updated" toast, say) should key off this
// return value rather than assuming a call always does something.
bool tryEnrichIfOnline(const RecentBook& book);

}  // namespace ContinueMetadataEnricher
