#pragma once
#include <Epub.h>
#include <GfxRenderer.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * Silent, non-interactive KOReader-sync used by the automatic hooks (book
 * open, book close, device sleep / screen timeout) layered on top of the
 * existing manual KOReaderSyncActivity flow.
 *
 * Unlike KOReaderSyncActivity, this NEVER:
 *  - shows an interactive Wi-Fi network picker
 *  - replaces the current activity or restarts the device
 *  - releases/resets the caller's Epub or Section
 *
 * It DOES match the manual flow's actual reliability mechanism: scan first,
 * then try every saved network that's actually in range (not just guess at
 * the last-connected one and give up).
 *
 * With checkRemoteFirst=true (used for the book-open hook), it also mirrors
 * KOReaderSyncActivity's "smart sync" decision: fetch the remote position
 * first: if remote and local are within 0.1 percentage points, do nothing;
 * if local is further along, push it; if remote is further along, tell the
 * caller to reposition instead of pushing (a push would regress the remote's
 * progress). This lib target has no src/ dependencies, so the actual
 * reposition (EpubReaderUtils::saveProgress + ActivityManager::goToReader)
 * happens in the caller -- push() only computes what position to reposition
 * to and hands it back.
 *
 * This is a lib/ target, so it deliberately has NO dependency on anything
 * under src/ (RenderLock, the theme/UI system, WifiCredentialStore all live
 * there and are not visible to a PlatformIO library). Everything src/-side
 * is supplied by the caller instead: the full list of saved Wi-Fi networks
 * as plain strings, and on-screen feedback via callbacks the caller
 * implements with RenderLock/GUI itself.
 */
class KOReaderAutoSync {
 public:
  enum class Result {
    Skipped,  // No credentials configured, or no saved network found in range.
    Success,
    Failed
  };

  // Which step is being announced, so onWaitMessage/onResultMessage never
  // need this lib to resolve translated text itself (that's an I18n.h
  // dependency, and this lib target has repeatedly proven fragile to give
  // any dependency LDF has to auto-detect through a chain of libraries
  // rather than directly from src/ -- see the RenderLock/WifiCredentialStore
  // history in this same file's callback design). The caller already
  // legitimately includes I18n.h and maps each value to its own translated
  // string itself.
  enum class Message { Syncing, Connecting, Checking, Repositioning, Uploading, Success, Failed };

  struct SavedNetwork {
    std::string ssid;
    std::string password;
  };

  struct PushResult {
    Result result = Result::Skipped;
    // Only ever true when checkRemoteFirst was passed as true (the open
    // hook) and the fetch found remote progress meaningfully ahead of
    // local. When true, the push itself was skipped -- the caller should
    // reposition to (remoteSpineIndex, remotePageNumber) instead via
    // EpubReaderUtils::saveProgress() + ActivityManager::goToReader(),
    // exactly mirroring KOReaderSyncActivity::saveProgressAndReturn().
    bool shouldApplyRemote = false;
    int remoteSpineIndex = 0;
    int remotePageNumber = 0;
    bool hasVisibleTextOffset = false;
    uint32_t visibleTextOffset = 0;
  };

  /**
   * Attempt to sync the current reading position for `epub` with the
   * configured KOReader-compatible sync server.
   *
   * @param epub                    The currently-open EPUB. Must be loaded;
   *                                 not released or modified by this call.
   * @param currentSpineIndex       Current spine (chapter) index.
   * @param currentPage             Current page within the spine item.
   * @param totalPagesInSpine       Total pages in the current spine item.
   * @param currentParagraphIndex   Optional 1-based paragraph index, same as
   *                                 used by the manual sync flow.
   * @param savedNetworks           Every network the caller has saved
   *                                 (WifiCredentialStore's full list). All of
   *                                 these are checked against a live scan;
   *                                 whichever ones are actually in range get
   *                                 tried, not just the last-connected one.
   * @param preferredSsid           The last-connected SSID, if any -- tried
   *                                 first when it's among the in-range
   *                                 candidates, purely as a priority hint.
   * @param renderer                Needed only for ProgressMapper's page-
   *                                 count estimation when checkRemoteFirst
   *                                 is true -- never drawn to directly.
   * @param checkRemoteFirst        true for the open hook: fetch remote
   *                                 first and possibly signal a reposition
   *                                 instead of pushing. false for close/
   *                                 sleep/timeout, where repositioning
   *                                 wouldn't mean anything (there's nothing
   *                                 on screen to jump) -- those always just
   *                                 push, skipping the extra round trip.
   * @param onWaitMessage           Called one or more times, synchronously,
   *                                 as each real step begins (connecting,
   *                                 checking, uploading) -- but only if
   *                                 non-empty AND a real attempt is actually
   *                                 made (never called on an early Skipped
   *                                 return). Pass an empty std::function to
   *                                 show nothing here (used for the book-open
   *                                 hook's initial "please wait" -- reading
   *                                 continues underneath until a reposition
   *                                 is actually needed). The caller maps each
   *                                 Message to text itself (translates the
   *                                 matching auto-sync string key).
   * @param onResultMessage         Called once, synchronously, after a real
   *                                 attempt (Success or Failed) with a
   *                                 message to show briefly. Never called
   *                                 when the result is Skipped, and never
   *                                 called when shouldApplyRemote is true --
   *                                 the caller's reposition + reload makes
   *                                 any further message pointless.
   * @param onConnected             Called at most once, synchronously, the
   *                                 moment Wi-Fi is confirmed connected --
   *                                 whether it was already up when push() was
   *                                 called, or push() just brought it up
   *                                 itself -- and always BEFORE any of the
   *                                 checks below that can tear the radio back
   *                                 down (already-in-sync, remote-ahead, or
   *                                 the ordinary push's own teardown after
   *                                 the upload). Never called on an early
   *                                 Skipped return (no credentials, or no
   *                                 saved network in range) -- there's no
   *                                 connection to piggyback on in that case.
   *                                 This exists because this function tears
   *                                 its own connection down again as soon as
   *                                 it's done with it whenever IT was the one
   *                                 that brought Wi-Fi up (the common case for
   *                                 the book open/close hooks, since Wi-Fi is
   *                                 normally off the rest of the time) -- by
   *                                 the time push() returns, WiFi.status() is
   *                                 very often already back to
   *                                 WL_DISCONNECTED, so a caller that only
   *                                 checks WiFi.status() after push() returns
   *                                 (as HomeStatusService::
   *                                 refreshLocationAndClockOnce() and
   *                                 ContinueMetadataEnricher::tryEnrichIfOnline()
   *                                 both do) will find it false almost every
   *                                 time and silently do nothing, even though
   *                                 a real connection genuinely was up for a
   *                                 few seconds. Any piggybacked work that
   *                                 itself needs the network belongs in this
   *                                 callback instead of after push() returns.
   *                                 Pass an empty std::function for none.
   */
  static PushResult push(const std::shared_ptr<Epub>& epub, int currentSpineIndex, int currentPage,
                         int totalPagesInSpine, std::optional<uint16_t> currentParagraphIndex,
                         const std::vector<SavedNetwork>& savedNetworks, const std::string& preferredSsid,
                         GfxRenderer& renderer, bool checkRemoteFirst,
                         const std::function<void(Message)>& onWaitMessage,
                         const std::function<void(Message)>& onResultMessage,
                         const std::function<void()>& onConnected = {});
};
