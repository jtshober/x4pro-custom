#include "KOReaderAutoSync.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cmath>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

namespace {

// Duplicated from src/util/TaskWatchdog.h rather than included: this lib/
// target can't see anything under src/ (see the header comment above), but
// this is a bare ESP-IDF wrapper with no project dependencies, so a local
// copy carries no real duplication risk. Every other long-blocking network
// path in this codebase (CrossPointWebServer, WebDAVHandler, the Calibre
// Connect and web-server activities) explicitly feeds the watchdog for
// exactly this reason: arduino-esp32's main loop task is watchdog-subscribed
// by default with a short window, and this function's scan+connect+HTTP
// sequence runs as ONE uninterrupted blocking call from that same task --
// unlike the manual sync flow, which spreads its scanning across many
// activity loop() ticks and so never blocks long enough in one go to need
// this. Without it, anything past a near-instant connect trips the
// watchdog and resets the device mid-sync, which reads as "always fails."
void feedWatchdogIfSubscribed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    esp_task_wdt_reset();
  }
}

// Per-candidate connect timeout, matching WifiSelectionActivity's own
// AUTO_CONNECTION_TIMEOUT_MS -- these are known to already be in range (we
// just scanned them), so this only needs to cover association + DHCP, not a
// speculative search.
constexpr unsigned long CANDIDATE_CONNECT_TIMEOUT_MS = 7000;
// Bounds worst-case blocking time (this can run right before the device
// sleeps): try the best few candidates, not every saved network ever added.
constexpr int MAX_CANDIDATES_TO_TRY = 3;
// Matches KOReaderSyncActivity's own "smart sync" threshold exactly: within
// 0.1 percentage points counts as already in sync, neither push nor pull.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

std::string documentHashForPath(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

struct Candidate {
  KOReaderAutoSync::SavedNetwork network;
  int32_t rssi;
  bool preferred;
};

// Scan (blocking) and match against every saved network, exactly mirroring
// WifiSelectionActivity::tryNextSavedNetworkFromScan()'s reasoning: a single
// blind WiFi.begin() at the last-connected SSID fails whenever THAT specific
// network isn't reachable right now, even when a different saved network is
// sitting in range. Scanning first and trying all of them is what actually
// makes the manual flow succeed more often.
//
// WiFi.scanNetworks() has a known ESP32 quirk: called too soon after a mode
// change (especially right after this same code's own WiFi.mode(WIFI_OFF) from a
// prior push, even 30+ seconds earlier -- the radio needs a moment after
// WiFi.mode() before a scan reliably returns anything), it can silently
// return zero results on a network that's genuinely in range. One retry
// after a short settle costs at most ~1s and meaningfully improves on that.
std::vector<Candidate> scanForSavedNetworks(const std::vector<KOReaderAutoSync::SavedNetwork>& saved,
                                            const std::string& preferredSsid) {
  int found = WiFi.scanNetworks();
  feedWatchdogIfSubscribed();
  if (found <= 0) {
    WiFi.scanDelete();
    delay(400);
    feedWatchdogIfSubscribed();
    found = WiFi.scanNetworks();
    feedWatchdogIfSubscribed();
  }
  std::vector<Candidate> candidates;
  if (found <= 0) {
    WiFi.scanDelete();
    return candidates;
  }
  for (const auto& net : saved) {
    for (int i = 0; i < found; i++) {
      if (WiFi.SSID(i) == net.ssid.c_str()) {
        candidates.push_back({net, WiFi.RSSI(i), net.ssid == preferredSsid});
        break;
      }
    }
  }
  WiFi.scanDelete();

  // Preferred (last-connected) first, then strongest signal.
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.preferred != b.preferred) return a.preferred;
    return a.rssi > b.rssi;
  });
  if (candidates.size() > static_cast<size_t>(MAX_CANDIDATES_TO_TRY)) {
    candidates.resize(MAX_CANDIDATES_TO_TRY);
  }
  return candidates;
}

bool connectToCandidate(const KOReaderAutoSync::SavedNetwork& net) {
  WiFi.begin(net.ssid.c_str(), net.password.c_str());
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < CANDIDATE_CONNECT_TIMEOUT_MS) {
    delay(100);
    feedWatchdogIfSubscribed();
  }
  return WiFi.status() == WL_CONNECTED;
}

}  // namespace

KOReaderAutoSync::PushResult KOReaderAutoSync::push(
    const std::shared_ptr<Epub>& epub, int currentSpineIndex, int currentPage, int totalPagesInSpine,
    std::optional<uint16_t> currentParagraphIndex, const std::vector<SavedNetwork>& savedNetworks,
    const std::string& preferredSsid, GfxRenderer& renderer, bool checkRemoteFirst,
    const std::function<void(Message)>& onWaitMessage, const std::function<void(Message)>& onResultMessage,
    const std::function<void()>& onConnected) {
  if (!epub || !KOREADER_STORE.hasCredentials() || savedNetworks.empty()) {
    return {Result::Skipped};
  }

  if (onWaitMessage) {
    onWaitMessage(Message::Syncing);
  }

  bool weConnected = false;
  if (WiFi.status() != WL_CONNECTED) {
    // Only tear the radio back down afterward if we were the ones who
    // brought it up -- e.g. the file-transfer web server may already have
    // Wi-Fi active, and this hook must not disturb that.
    weConnected = true;
    WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);  // Abort any in-progress SDK auto-connect and clear NVS-saved SSID
    delay(300);  // let the radio settle after the mode change before scanning -- too short a gap is a known cause of a scan silently returning nothing
    feedWatchdogIfSubscribed();

    const auto candidates = scanForSavedNetworks(savedNetworks, preferredSsid);
    if (candidates.empty()) {
      // Scanned, but none of our saved networks are actually in range right
      // now. Still tell the user something happened (or didn't) -- once
      // "Syncing..." has been shown, silence afterward reads as "did this do
      // anything at all?", not as "nothing to do here."
      LOG_DBG("KOSync", "Auto-sync: no saved network found in range");
      WiFi.mode(WIFI_OFF);  // matches this codebase's own WiFi teardown convention (see finishWifiSessionWithoutRestart() in main.cpp) -- esp_wifi_stop() bypasses the Arduino WiFi library's state tracking and repeated use was a likely cause of scans later returning nothing
      if (onResultMessage) {
        onResultMessage(Message::Failed);
      }
      return {Result::Skipped};
    }

    bool connected = false;
    if (onWaitMessage) {
      onWaitMessage(Message::Connecting);
    }
    for (const auto& c : candidates) {
      if (connectToCandidate(c.network)) {
        connected = true;
        break;
      }
      WiFi.disconnect(true);
    }

    if (!connected) {
      LOG_DBG("KOSync", "Auto-sync: %u in-range saved network(s) found, none connected",
              static_cast<unsigned>(candidates.size()));
      WiFi.mode(WIFI_OFF);
      if (onResultMessage) {
        onResultMessage(Message::Failed);
      }
      return {Result::Failed};
    }
    // Keep the station fully awake for the short sync transaction, same as
    // the manual sync flow -- modem sleep can turn this into a multi-second
    // stall that reads as a timeout.
    WiFi.setSleep(false);
  }

  // Wi-Fi is guaranteed connected from here on -- either it already was
  // (weConnected == false), or the block above just connected it and would
  // have already returned Skipped/Failed otherwise. Every path from here
  // downward that hands control back to the caller either returns directly
  // or first tears this connection back down (see the comment on
  // onConnected in the header for why that ordering matters) -- so this is
  // the one moment a caller can safely piggyback its own network use on this
  // connection.
  if (onConnected) {
    onConnected();
  }

  const std::string documentHash = documentHashForPath(epub->getPath());

  CrossPointPosition pos{};
  pos.spineIndex = currentSpineIndex;
  pos.pageNumber = currentPage;
  pos.totalPages = totalPagesInSpine;
  const SavedProgressPosition localProgress = ProgressMapper::toSavedProgress(epub, pos);

  // Book-open only: check whether the remote is further along before
  // pushing anything, exactly mirroring KOReaderSyncActivity's "smart sync"
  // decision (see saveProgressAndReturn()/the delta check in performSync()).
  // Close/sleep/timeout skip this entirely -- there's no reader on screen to
  // reposition, so it would just be a wasted round trip.
  if (checkRemoteFirst) {
    if (onWaitMessage) {
      onWaitMessage(Message::Checking);
    }
    feedWatchdogIfSubscribed();
    KOReaderProgress remoteProgress;
    const auto fetchResult = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
    feedWatchdogIfSubscribed();

    if (fetchResult == KOReaderSyncClient::OK) {
      const float delta = localProgress.percentage - remoteProgress.percentage;
      if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
        // Already in sync -- nothing to push, nothing to reposition to.
        if (weConnected) {
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        }
        if (onResultMessage) {
          onResultMessage(Message::Success);
        }
        return {Result::Success};
      }

      if (delta < 0.0f) {
        // Remote is further along than local: resolve its xpath to a
        // position and hand it back instead of pushing (a push here would
        // regress the remote's progress). No result toast -- the caller's
        // reposition + reload is the visible confirmation this worked.
        if (onWaitMessage) {
          onWaitMessage(Message::Repositioning);
        }
        SavedProgressPosition remoteSaved{remoteProgress.progress, remoteProgress.percentage};
        CrossPointPosition remotePos =
            ProgressMapper::toCrossPoint(epub, remoteSaved, renderer, currentSpineIndex, totalPagesInSpine);
        if (!remotePos.hasVisibleTextOffset && remoteProgress.position.has_value()) {
          const bool sameXPath = remoteProgress.position->xpath == remoteProgress.progress;
          if (const auto richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer,
                                                                        sameXPath)) {
            remotePos = *richMapped;
          }
        }
        if (weConnected) {
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        }
        PushResult out{Result::Success};
        out.shouldApplyRemote = true;
        out.remoteSpineIndex = remotePos.spineIndex;
        out.remotePageNumber = remotePos.pageNumber;
        out.hasVisibleTextOffset = remotePos.hasVisibleTextOffset;
        out.visibleTextOffset = remotePos.visibleTextOffset;
        return out;
      }
      // delta > 0: local is further along -- fall through to the normal
      // push below, same as the NOT_FOUND/error cases.
    }
    // NOT_FOUND (first sync ever) or a fetch error: fall through and push
    // local, same as the manual flow's NOT_FOUND handling.
  }

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition rich;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    rich.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    rich.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    rich.pageNumber = static_cast<uint16_t>(currentPage);
    rich.totalPages = static_cast<uint16_t>(totalPagesInSpine > 0 ? totalPagesInSpine : 1);
    rich.paragraphIndex = currentParagraphIndex;
    rich.xpath = localProgress.xpath;
    progress.position = std::move(rich);
  }

  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const std::string path = epub->getPath();
    const auto lastSlash = path.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    meta.title = epub->getTitle();
    meta.authors = epub->getAuthor();
    progress.metadata = std::move(meta);
  }

  if (onWaitMessage) {
    onWaitMessage(Message::Uploading);
  }
  feedWatchdogIfSubscribed();  // the HTTP round trip below is the other long uninterrupted stretch
  const auto result = KOReaderSyncClient::updateProgress(progress);
  feedWatchdogIfSubscribed();

  if (weConnected) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);  // matches this codebase's own WiFi teardown convention, not the raw esp_wifi_stop()
  }

  if (onResultMessage) {
    onResultMessage(result == KOReaderSyncClient::OK ? Message::Success : Message::Failed);
  }

  return {result == KOReaderSyncClient::OK ? Result::Success : Result::Failed};
}
