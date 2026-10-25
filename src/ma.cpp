#include "ma.h"
#include "app_log.h"
#include "config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <algorithm>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <time.h>

namespace {

constexpr size_t MAX_ART_BYTES = 400 * 1024;
constexpr uint32_t POLL_MS = 2000;
constexpr uint32_t COMMAND_REPOLL_MS = 600;
constexpr int PLAYLIST_PAGE = 50;
const char *LIST_TMP_PATH = "/list.json";

WiFiClient sPlain;
WiFiClientSecure sTls;

// TLS is not certificate-pinned: the connection is encrypted but the server identity is not verified.
WiFiClient &sClient() { return g_config.maTls ? static_cast<WiFiClient &>(sTls) : sPlain; }

void stopClients() {
    sPlain.stop();
    sTls.stop();
}

SemaphoreHandle_t netLock;
SemaphoreHandle_t stateLock;
QueueHandle_t cmdQueue;
bool taskStarted = false;

PlayerState shared;
SemaphoreHandle_t artLock;
volatile bool artReady = false;
uint32_t lastPollMs = 0;
volatile uint32_t lastOkMs = 0;
volatile bool everOk = false;

struct CmdMsg {
    uint8_t cmd;
    char arg[128];
};

ListData lists[4];

volatile bool authLost = false;
String lastArtUrl;
String queueId;
String nerdText;
String nerdTrack;
uint8_t artFailures = 0;

struct HttpResult {
    int code = -1;
    String body;
    int savedBytes = 0;
    time_t serverTime = 0;
};

bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

String describeError(const String &body, int code) {
    if (code == 401 || code == 403) return "Access denied. Check the token under Music Assistant in the web setup.";
    if (code > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
            const char *msg = doc["error"] | doc["message"] | "";
            if (msg[0]) return msg;
        }
        return "Music Assistant error (HTTP " + String(code) + ")";
    }
    return "No connection to Music Assistant. Check the server address.";
}

HttpResult httpOnce(const char *method, const String &path, const String &body, const String &bearer, const char *savePath) {
    HttpResult r;
    HTTPClient http;
    http.setReuse(true);
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    const char *keys[] = {"Date"};
    http.collectHeaders(keys, 1);

    if (!http.begin(sClient(), g_config.maHost, g_config.maPort, g_config.maPath + path, g_config.maTls)) return r;
    if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
    if (body.length()) http.addHeader("Content-Type", "application/json");

    uint32_t t0 = millis();
    r.code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
    if (r.code == 200 && savePath) {
        File file = LittleFS.open(savePath, "w");
        if (file) {
            r.savedBytes = http.writeToStream(&file);
            file.close();
        }
        if (r.savedBytes <= 0) LittleFS.remove(savePath);
    } else if (r.code > 0 && r.code != 204 && http.getSize() != 0) {
        r.body = http.getString();
    }
    if (r.code > 0 && http.hasHeader("Date")) {
        struct tm tmv = {};
        if (strptime(http.header("Date").c_str(), "%a, %d %b %Y %H:%M:%S", &tmv)) r.serverTime = mktime(&tmv);
    }
    http.end();

    if (r.code < 0) {
        app_log(LL_WARN, "%s %s failed: %s (8-bit heap free=%u, block=%u)", method, path.c_str(), HTTPClient::errorToString(r.code).c_str(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    } else if (r.code >= 400) {
        app_log(LL_WARN, "%s %s -> %d: %.160s", method, path.c_str(), r.code, r.body.c_str());
    } else {
        app_log(LL_DEBUG, "%s %s -> %d (%u ms, %u B)", method, path.c_str(), r.code, (unsigned)(millis() - t0), (unsigned)(r.body.length() + r.savedBytes));
    }
    return r;
}

HttpResult httpDo(const char *method, const String &path, const String &body, const String &bearer, const char *savePath = nullptr) {
    bool reused = sClient().connected();
    HttpResult r = httpOnce(method, path, body, bearer, savePath);
    // A kept-alive connection the server already closed shows up as a failure.
    if (r.code < 0 && reused) {
        sClient().stop();
        r = httpOnce(method, path, body, bearer, savePath);
    }
    return r;
}

void setMessage(const String &msg) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    shared.message = msg;
    xSemaphoreGive(stateLock);
}

HttpResult apiCall(const char *command, JsonDocument *args = nullptr, const char *savePath = nullptr) {
    HttpResult r;
    if (!ma_configured()) {
        authLost = true;
        return r;
    }
    JsonDocument doc;
    doc["command"] = command;
    if (args) doc["args"] = *args;
    else doc["args"].to<JsonObject>();
    String body;
    serializeJson(doc, body);

    r = httpDo("POST", "/api", body, g_config.maToken, savePath);
    if (r.code == 401 || r.code == 403) {
        app_log(LL_ERROR, "Music Assistant rejected the token");
        authLost = true;
    }
    return r;
}

// Art always goes through the configured server, whatever host the URL names.
String artPathFor(const char *imageUrl) {
    String url = imageUrl;
    int start = url.indexOf("://");
    if (start < 0) return "";
    int pathStart = url.indexOf('/', start + 3);
    if (pathStart < 0) return "";
    int query = url.indexOf('?', pathStart);
    String path = query < 0 ? url.substring(pathStart) : url.substring(pathStart, query);
    return path + "?size=" + String(g_config.artSize) + "&fmt=jpg";
}

bool parsePlayer(const String &body, time_t serverTime, PlayerState &out) {
    JsonDocument filter;
    filter["playback_state"] = true;
    filter["volume_level"] = true;
    filter["display_name"] = true;
    filter["active_source"] = true;
    JsonObject media = filter["current_media"].to<JsonObject>();
    media["uri"] = true;
    media["title"] = true;
    media["artist"] = true;
    media["album"] = true;
    media["image_url"] = true;
    media["duration"] = true;
    media["elapsed_time"] = true;
    media["elapsed_time_last_updated"] = true;
    media["queue_item_id"] = true;
    media["source_id"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        app_log(LL_ERROR, "Player JSON error: %s (%u B): %.150s", err.c_str(), (unsigned)body.length(), body.c_str());
        return false;
    }

    out.playing = strcmp(doc["playback_state"] | "", "playing") == 0;
    out.volume = doc["volume_level"] | -1;
    out.deviceName = (const char *)(doc["display_name"] | "");

    queueId = (const char *)(doc["active_source"] | "");
    if (queueId.isEmpty()) queueId = g_config.playerId;

    JsonObject it = doc["current_media"];
    if (it.isNull() || !(it["title"] | "")[0]) return true;

    const char *source = it["source_id"] | "";
    if (source[0]) queueId = source;

    out.active = true;
    out.title = (const char *)(it["title"] | "");
    out.artist = (const char *)(it["artist"] | "");
    out.album = (const char *)(it["album"] | "");
    const char *item = it["queue_item_id"] | "";
    out.trackId = item[0] ? String(item) : String((const char *)(it["uri"] | ""));
    out.durationMs = (uint32_t)((it["duration"] | 0.0f) * 1000);

    double elapsed = it["elapsed_time"] | 0.0;
    double updated = it["elapsed_time_last_updated"] | 0.0;
    if (out.playing && serverTime > 0 && updated > 0 && serverTime > updated) elapsed += serverTime - updated;
    if (elapsed < 0) elapsed = 0;
    out.progressMs = (uint32_t)(elapsed * 1000);
    if (out.durationMs && out.progressMs > out.durationMs) out.progressMs = out.durationMs;

    const char *image = it["image_url"] | "";
    if (image[0]) out.artUrl = artPathFor(image);
    return true;
}

bool downloadArt(const String &path) {
    xSemaphoreTake(artLock, portMAX_DELAY);
    app_log(LL_DEBUG, "Art: %s (free=%u, max=%u)", path.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    bool ok = false;
    HttpResult r = httpDo("GET", path, "", g_config.maToken, MA_ART_PATH);
    if (r.code != 200) {
        app_log(LL_WARN, "Art: HTTP %d", r.code);
    } else if (r.savedBytes <= 0 || r.savedBytes > (int)MAX_ART_BYTES) {
        app_log(LL_WARN, "Art: unusable image (%d bytes)", r.savedBytes);
        LittleFS.remove(MA_ART_PATH);
    } else {
        ok = true;
    }
    if (ok) artReady = true;
    xSemaphoreGive(artLock);
    return ok;
}

void pollNerd(const String &trackId) {
    if (trackId == nerdTrack || queueId.isEmpty()) return;
    nerdTrack = trackId;
    nerdText = "";

    JsonDocument args;
    args["queue_id"] = queueId;
    HttpResult r = apiCall("player_queues/get", &args);
    if (r.code != 200) return;

    JsonDocument filter;
    for (const char *k : {"shuffle_enabled", "repeat_mode", "current_index"}) filter[k] = true;
    JsonObject ci = filter["current_item"].to<JsonObject>();
    JsonObject sd = ci["streamdetails"].to<JsonObject>();
    sd["provider"] = true;
    sd["stream_type"] = true;
    sd["size"] = true;
    sd["loudness"] = true;
    sd["volume_normalization_gain_correct"] = true;
    sd["audio_format"] = true;
    JsonObject mi = ci["media_item"].to<JsonObject>();
    for (const char *k : {"track_number", "disc_number", "favorite", "play_count", "year", "isrc", "uri"}) mi[k] = true;
    mi["metadata"]["genres"] = true;
    mi["metadata"]["release_date"] = true;
    mi["metadata"]["label"] = true;
    mi["metadata"]["explicit"] = true;
    mi["album"]["year"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, r.body, DeserializationOption::Filter(filter))) return;

    String t;
    auto line = [&t](const char *label, const String &value) {
        if (value.length()) t += String(label) + ": " + value + "\n";
    };
    JsonObject f = doc["current_item"]["streamdetails"]["audio_format"];
    if (!f.isNull()) {
        String codec = (const char *)(f["content_type"] | "");
        codec.toUpperCase();
        String fmt = codec;
        if (f["sample_rate"].is<int>()) fmt += " " + String((f["sample_rate"] | 0) / 1000.0, 1) + " kHz";
        if (f["bit_depth"].is<int>()) fmt += " " + String(f["bit_depth"] | 0) + "-bit";
        if ((f["bit_rate"] | 0) > 0) fmt += " " + String(f["bit_rate"] | 0) + " kbps";
        line("Format", fmt);
    }
    JsonObject s = doc["current_item"]["streamdetails"];
    String src = (const char *)(s["provider"] | "");
    int dash = src.indexOf("--");
    if (dash > 0) src = src.substring(0, dash);
    String st = (const char *)(s["stream_type"] | "");
    if (st.length()) src += " (" + st + ")";
    line("Source", src);
    if ((s["size"] | 0) > 0) line("Size", String((s["size"] | 0.0) / 1048576.0, 1) + " MB");
    if (!s["loudness"].isNull()) {
        String l = String(s["loudness"] | 0.0, 1) + " LUFS";
        if (!s["volume_normalization_gain_correct"].isNull()) l += " (gain " + String(s["volume_normalization_gain_correct"] | 0.0, 1) + " dB)";
        line("Loudness", l);
    }

    JsonObject m = doc["current_item"]["media_item"];
    JsonArray genres = m["metadata"]["genres"];
    String g;
    for (JsonVariant v : genres) g += String(g.length() ? ", " : "") + (const char *)(v | "");
    line("Genre", g);
    String rel = (const char *)(m["metadata"]["release_date"] | "");
    if (rel.isEmpty() && (m["year"] | 0) > 0) rel = String(m["year"] | 0);
    if (rel.isEmpty() && (m["album"]["year"] | 0) > 0) rel = String(m["album"]["year"] | 0);
    line("Released", rel.substring(0, 10));
    line("Label", String((const char *)(m["metadata"]["label"] | "")));
    String tn;
    if ((m["track_number"] | 0) > 0) tn = "Track " + String(m["track_number"] | 0);
    if ((m["disc_number"] | 0) > 1) tn += ", disc " + String(m["disc_number"] | 0);
    line("Position", tn);
    if ((m["play_count"] | 0) > 0) line("Plays", String(m["play_count"] | 0));
    if (m["favorite"] | false) line("Favorite", "yes");
    if (m["metadata"]["explicit"] | false) line("Explicit", "yes");
    line("ISRC", String((const char *)(m["isrc"] | "")));
    line("URI", String((const char *)(m["uri"] | "")));
    line("Queue", "item " + String((doc["current_index"] | 0) + 1) + ", shuffle " +
                      String((doc["shuffle_enabled"] | false) ? "on" : "off") + ", repeat " +
                      String((const char *)(doc["repeat_mode"] | "off")));
    nerdText = t;
}

void pollPlayer() {
    if (g_config.playerId.isEmpty()) {
        xSemaphoreTake(stateLock, portMAX_DELAY);
        shared = PlayerState();
        shared.message = "No player selected. Tap the list icon to choose one.";
        xSemaphoreGive(stateLock);
        return;
    }

    JsonDocument args;
    args["player_id"] = g_config.playerId;
    uint32_t t0 = millis();
    HttpResult r = apiCall("players/get", &args);
    lastPollMs = millis() - t0;

    if (r.code == 200) {
        lastOkMs = millis();
        everOk = true;

        PlayerState s;
        if (!parsePlayer(r.body, r.serverTime, s)) {
            setMessage("Unexpected response from Music Assistant");
            return;
        }
        s.stamp = millis();
        if (g_config.nerdMode && s.active) {
            pollNerd(s.trackId);
            s.nerd = nerdText;
        } else {
            nerdTrack = "";
        }
        xSemaphoreTake(stateLock, portMAX_DELAY);
        shared = s;
        xSemaphoreGive(stateLock);
        app_log(LL_DEBUG, "Now: %s - %s (%s)", s.artist.c_str(), s.title.c_str(), s.playing ? "playing" : "paused");

        if (s.artUrl.length() && s.artUrl != lastArtUrl) {
            r.body = String();
            bool got = downloadArt(s.artUrl);
            if (got || ++artFailures >= 3) {
                lastArtUrl = s.artUrl;
                artFailures = 0;
            }
        }
    } else if (!authLost) {
        setMessage(describeError(r.body, r.code));
    }
}

void finishList(ListKind kind, ListStatus status, const String &error, std::vector<ListItem> &&items,
                uint32_t offset = 0, uint32_t next = 0, uint32_t total = 0) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    ListData &l = lists[kind];
    l.status = status;
    l.error = error;
    l.items = std::move(items);
    l.offset = offset;
    l.next = next;
    l.total = total;
    l.version++;
    xSemaphoreGive(stateLock);
}

int listKindOfCmd(uint8_t cmd) {
    switch (cmd) {
    case CMD_LIST_PLAYERS:
        return LIST_PLAYERS;
    case CMD_LIST_PLAYLISTS:
        return LIST_PLAYLISTS;
    case CMD_LIST_WEB_PLAYLISTS:
        return LIST_WEB_PLAYLISTS;
    case CMD_LIST_WEB_PLAYERS:
        return LIST_WEB_PLAYERS;
    default:
        return -1;
    }
}

void fetchPlayers(ListKind kind) {
    HttpResult r = apiCall("players/all", nullptr, LIST_TMP_PATH);
    if (r.code != 200 || r.savedBytes <= 0) {
        finishList(kind, LIST_ERROR, describeError(r.body, r.code), {});
        return;
    }

    JsonDocument filter;
    filter[0]["player_id"] = true;
    filter[0]["display_name"] = true;
    filter[0]["available"] = true;
    filter[0]["enabled"] = true;
    filter[0]["hide_in_ui"] = true;
    filter[0]["playback_state"] = true;
    JsonDocument doc;
    File file = LittleFS.open(LIST_TMP_PATH, "r");
    DeserializationError err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
    file.close();
    LittleFS.remove(LIST_TMP_PATH);
    if (err) {
        app_log(LL_ERROR, "Player list JSON error: %s", err.c_str());
        finishList(kind, LIST_ERROR, "Unexpected response from Music Assistant", {});
        return;
    }

    std::vector<ListItem> items;
    for (JsonObject o : doc.as<JsonArray>()) {
        const char *id = o["player_id"] | "";
        if (!id[0] || !(o["available"] | false) || !(o["enabled"] | true) || (o["hide_in_ui"] | false)) continue;
        ListItem item;
        item.id = id;
        item.name = (const char *)(o["display_name"] | id);
        item.active = item.id == g_config.playerId;
        item.owner = strcmp(o["playback_state"] | "", "playing") == 0 ? "playing" : "";
        items.push_back(std::move(item));
    }
    std::stable_partition(items.begin(), items.end(), [](const ListItem &i) { return i.owner == "playing"; });
    uint32_t count = items.size();
    finishList(kind, LIST_READY, "", std::move(items), 0, count, count);
}

void fetchPlaylists(ListKind kind, uint32_t offset) {
    JsonDocument args;
    args["limit"] = PLAYLIST_PAGE;
    args["offset"] = offset;
    args["order_by"] = "name";
    HttpResult r = apiCall("music/playlists/library_items", &args, LIST_TMP_PATH);
    if (r.code != 200 || r.savedBytes <= 0) {
        finishList(kind, LIST_ERROR, describeError(r.body, r.code), {}, offset);
        return;
    }

    JsonDocument filter;
    filter[0]["uri"] = true;
    filter[0]["name"] = true;
    filter[0]["owner"] = true;
    filter[0]["available"] = true;
    JsonDocument doc;
    File file = LittleFS.open(LIST_TMP_PATH, "r");
    DeserializationError err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
    file.close();
    LittleFS.remove(LIST_TMP_PATH);
    if (err) {
        app_log(LL_ERROR, "Playlist JSON error: %s", err.c_str());
        finishList(kind, LIST_ERROR, "Unexpected response from Music Assistant", {}, offset);
        return;
    }

    JsonArray array = doc.as<JsonArray>();
    std::vector<ListItem> items;
    for (JsonObject o : array) {
        const char *uri = o["uri"] | "";
        if (!uri[0] || strlen(uri) >= 120 || !(o["available"] | true)) continue;
        ListItem item;
        item.id = uri;
        item.name = (const char *)(o["name"] | "");
        item.owner = (const char *)(o["owner"] | "");
        items.push_back(std::move(item));
    }
    // The server reports no total, so a full page means there may be more.
    uint32_t next = offset + array.size();
    uint32_t total = array.size() < (size_t)PLAYLIST_PAGE ? next : next + 1;
    finishList(kind, LIST_READY, "", std::move(items), offset, next, total);
}

void runCommand(const CmdMsg &msg) {
    PlayerState snap;
    ma_get_state(snap);
    const String player = g_config.playerId;

    HttpResult r;
    JsonDocument args;
    int newVolume = snap.volume;
    switch (msg.cmd) {
    case CMD_PLAY_PAUSE:
        args["player_id"] = player;
        r = apiCall("players/cmd/play_pause", &args);
        break;
    case CMD_NEXT:
        args["player_id"] = player;
        r = apiCall("players/cmd/next", &args);
        break;
    case CMD_PREV:
        args["player_id"] = player;
        r = apiCall("players/cmd/previous", &args);
        break;
    case CMD_VOL_UP:
    case CMD_VOL_DOWN:
        if (snap.volume < 0) return;
        newVolume = constrain(snap.volume + (msg.cmd == CMD_VOL_UP ? 10 : -10), 0, 100);
        args["player_id"] = player;
        args["volume_level"] = newVolume;
        r = apiCall("players/cmd/volume_set", &args);
        break;
    case CMD_LIST_PLAYERS:
        fetchPlayers(LIST_PLAYERS);
        return;
    case CMD_LIST_PLAYLISTS:
        fetchPlaylists(LIST_PLAYLISTS, strtoul(msg.arg, nullptr, 10));
        return;
    case CMD_LIST_WEB_PLAYERS:
        fetchPlayers(LIST_WEB_PLAYERS);
        return;
    case CMD_LIST_WEB_PLAYLISTS:
        fetchPlaylists(LIST_WEB_PLAYLISTS, strtoul(msg.arg, nullptr, 10));
        return;
    case CMD_SEEK:
        args["queue_id"] = queueId.length() ? queueId : player;
        args["position"] = strtoul(msg.arg, nullptr, 10) / 1000;
        r = apiCall("player_queues/seek", &args);
        break;
    case CMD_PLAY_PLAYLIST:
        args["queue_id"] = queueId.length() ? queueId : player;
        args["media"] = (const char *)msg.arg;
        args["option"] = "replace";
        r = apiCall("player_queues/play_media", &args);
        break;
    default:
        return;
    }

    if (r.code >= 200 && r.code < 300) {
        xSemaphoreTake(stateLock, portMAX_DELAY);
        if (msg.cmd == CMD_PLAY_PAUSE) shared.playing = !snap.playing;
        if (msg.cmd == CMD_VOL_UP || msg.cmd == CMD_VOL_DOWN) shared.volume = newVolume;
        if (msg.cmd == CMD_SEEK) {
            shared.progressMs = strtoul(msg.arg, nullptr, 10);
            shared.stamp = millis();
        }
        shared.message = "";
        xSemaphoreGive(stateLock);
    } else if (!authLost) {
        if (msg.cmd == CMD_PLAY_PLAYLIST && r.code == 500) {
            setMessage("Cannot play this playlist. It may be empty or its provider is unavailable.");
        } else {
            setMessage(describeError(r.body, r.code));
        }
    }
}

void maTask(void *) {
    uint32_t nextPoll = millis();
    for (;;) {
        if (authLost) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int32_t wait = (int32_t)(nextPoll - millis());
        CmdMsg msg = {};
        bool gotCmd = xQueueReceive(cmdQueue, &msg, pdMS_TO_TICKS(wait > 0 ? wait : 0)) == pdTRUE;

        xSemaphoreTake(netLock, portMAX_DELAY);
        bool online = WiFi.status() == WL_CONNECTED;
        if (gotCmd && online) {
            runCommand(msg);
            nextPoll = millis() + COMMAND_REPOLL_MS;
        } else if (gotCmd && listKindOfCmd(msg.cmd) >= 0) {
            finishList((ListKind)listKindOfCmd(msg.cmd), LIST_ERROR, "Wi-Fi disconnected", {}, strtoul(msg.arg, nullptr, 10));
        }
        if (reached(nextPoll)) {
            if (!online) {
                setMessage("Wi-Fi disconnected");
            } else {
                pollPlayer();
            }
            nextPoll = millis() + POLL_MS;
        }
        xSemaphoreGive(netLock);
    }
}

struct ServerAddr {
    String host;
    int port = MA_DEFAULT_PORT;
    bool tls = false;
    String path;
};

// Accepts "host", "host:port" and pasted http(s)://host[:port][/prefix] addresses.
bool parseServer(String input, ServerAddr &out) {
    input.trim();
    out = ServerAddr();
    if (input.startsWith("https://")) {
        out.tls = true;
        out.port = 443;
        input = input.substring(8);
    } else if (input.startsWith("http://")) {
        input = input.substring(7);
    }
    int slash = input.indexOf('/');
    if (slash >= 0) {
        out.path = input.substring(slash);
        input = input.substring(0, slash);
        while (out.path.endsWith("/")) out.path.remove(out.path.length() - 1);
        if (out.path.length() > 100) return false;
    }
    int colon = input.lastIndexOf(':');
    if (colon >= 0) {
        out.port = input.substring(colon + 1).toInt();
        input = input.substring(0, colon);
    }
    out.host = input;
    return out.host.length() > 0 && out.host.length() <= 80 && out.port > 0 && out.port < 65536;
}

void applyServer(const ServerAddr &a) {
    g_config.maHost = a.host;
    g_config.maPort = a.port;
    g_config.maTls = a.tls;
    g_config.maPath = a.path;
}

} // namespace

void ma_init() {
    sTls.setInsecure();
    netLock = xSemaphoreCreateMutex();
    stateLock = xSemaphoreCreateMutex();
    artLock = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(4, sizeof(CmdMsg));
}

void ma_start() {
    if (taskStarted) return;
    taskStarted = true;
    xTaskCreatePinnedToCore(maTask, "ma", 10240, nullptr, 1, nullptr, 0);
}

bool ma_configured() { return g_config.maHost.length() > 0 && g_config.maToken.length() > 0; }

bool ma_server_reachable() {
    if (g_config.maHost.isEmpty()) return false;
    WiFiClientSecure tls;
    WiFiClient plain;
    tls.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(2000);
    WiFiClient &client = g_config.maTls ? static_cast<WiFiClient &>(tls) : plain;
    if (!http.begin(client, g_config.maHost, g_config.maPort, g_config.maPath + "/info", g_config.maTls)) return false;
    int code = http.GET();
    http.end();
    return code == 200;
}

// Replaces the shared connection settings; callers hold netLock and restore on failure.
struct SavedServer {
    ServerAddr addr;
    String token;
};

SavedServer saveServer() {
    SavedServer s;
    s.addr.host = g_config.maHost;
    s.addr.port = g_config.maPort;
    s.addr.tls = g_config.maTls;
    s.addr.path = g_config.maPath;
    s.token = g_config.maToken;
    return s;
}

void restoreServer(const SavedServer &s) {
    applyServer(s.addr);
    g_config.maToken = s.token;
}

bool ma_ha_login_url(const String &server, const String &returnUrl, String &authUrl, String &error) {
    ServerAddr addr;
    if (!parseServer(server, addr)) {
        error = "Invalid server address. Use for example 192.168.1.10:8095 or https://ma.example.com.";
        return false;
    }
    String encoded;
    for (size_t i = 0; i < returnUrl.length(); i++) {
        char c = returnUrl[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }

    xSemaphoreTake(netLock, portMAX_DELAY);
    SavedServer old = saveServer();
    applyServer(addr);
    stopClients();
    HttpResult r = httpDo("GET", "/auth/authorize?provider_id=homeassistant&return_url=" + encoded, "", "");
    JsonDocument doc;
    bool ok = r.code == 200 && !deserializeJson(doc, r.body) && (doc["authorization_url"] | "")[0];
    if (ok) {
        authUrl = (const char *)doc["authorization_url"];
    } else {
        error = r.code > 0 ? "Home Assistant sign-in is not available on this server."
                           : "Cannot reach the server. Check the address and that this device is on the same network.";
    }
    restoreServer(old);
    stopClients();
    xSemaphoreGive(netLock);
    return ok;
}

bool ma_connect(const String &server, const String &token, const String &username, const String &password, String &error) {
    ServerAddr addr;
    if (!parseServer(server, addr)) {
        error = "Invalid server address. Use for example 192.168.1.10:8095 or https://ma.example.com.";
        return false;
    }

    xSemaphoreTake(netLock, portMAX_DELAY);
    SavedServer old = saveServer();
    applyServer(addr);
    stopClients();

    bool ok = false;
    HttpResult info = httpDo("GET", "/info", "", "");
    JsonDocument doc;
    if (info.code != 200 || deserializeJson(doc, info.body) || !doc["server_id"].is<const char *>()) {
        error = info.code > 0 ? "That address does not look like Music Assistant."
                              : "Cannot reach the server. Check the address and that this device is on the same network.";
    } else {
        String newToken = token;
        newToken.trim();
        if (newToken.isEmpty() && username.length()) {
            JsonDocument login;
            login["credentials"]["username"] = username;
            login["credentials"]["password"] = password;
            String body;
            serializeJson(login, body);
            HttpResult r = httpDo("POST", "/auth/login", body, "");
            JsonDocument reply;
            bool parsed = !deserializeJson(reply, r.body);
            if (r.code == 200 && parsed && (reply["success"] | false) && (reply["token"] | "")[0]) {
                newToken = (const char *)reply["token"];
            } else {
                const char *msg = parsed ? (reply["error"] | "") : "";
                error = msg[0] ? String(msg) : String("Login failed. Check the username and password.");
            }
        } else if (newToken.isEmpty()) {
            error = "Enter an access token or a username and password.";
        }

        if (newToken.length()) {
            g_config.maToken = newToken;
            HttpResult r = httpDo("POST", "/api", "{\"command\":\"music/playlists/library_items\",\"args\":{\"limit\":1}}", newToken);
            if (r.code == 200) {
                ok = true;
            } else {
                error = r.code == 401 || r.code == 403 ? "The server rejected this token." : describeError(r.body, r.code);
            }
        }
    }

    if (ok) {
        config_save();
        authLost = false;
        lastArtUrl = "";
        queueId = "";
        setMessage("");
    } else {
        restoreServer(old);
    }
    stopClients();
    xSemaphoreGive(netLock);
    return ok;
}

void ma_send(PlayerCmd cmd) {
    CmdMsg msg = {};
    msg.cmd = cmd;
    xQueueSend(cmdQueue, &msg, 0);
}

void ma_seek(uint32_t positionMs) {
    CmdMsg msg = {};
    msg.cmd = CMD_SEEK;
    snprintf(msg.arg, sizeof(msg.arg), "%u", (unsigned)positionMs);
    xQueueSend(cmdQueue, &msg, 0);
}

void ma_request_list(ListKind kind, uint32_t offset) {
    String arg = String(offset);
    xSemaphoreTake(stateLock, portMAX_DELAY);
    lists[kind].status = LIST_LOADING;
    lists[kind].items.clear();
    lists[kind].offset = offset;
    lists[kind].key = arg;
    lists[kind].version++;
    xSemaphoreGive(stateLock);

    CmdMsg msg = {};
    switch (kind) {
    case LIST_PLAYERS:
        msg.cmd = CMD_LIST_PLAYERS;
        break;
    case LIST_WEB_PLAYERS:
        msg.cmd = CMD_LIST_WEB_PLAYERS;
        break;
    case LIST_PLAYLISTS:
        msg.cmd = CMD_LIST_PLAYLISTS;
        break;
    case LIST_WEB_PLAYLISTS:
        msg.cmd = CMD_LIST_WEB_PLAYLISTS;
        break;
    }
    strlcpy(msg.arg, arg.c_str(), sizeof(msg.arg));
    xQueueSend(cmdQueue, &msg, 0);
}

void ma_select(ListKind kind, const String &id, const String &name) {
    if (kind == LIST_PLAYERS || kind == LIST_WEB_PLAYERS) {
        g_config.playerId = id;
        g_config.playerName = name;
        config_save();
        lastArtUrl = "";
        queueId = "";
        xSemaphoreTake(stateLock, portMAX_DELAY);
        shared = PlayerState();
        xSemaphoreGive(stateLock);
        return;
    }
    CmdMsg msg = {};
    msg.cmd = CMD_PLAY_PLAYLIST;
    strlcpy(msg.arg, id.c_str(), sizeof(msg.arg));
    xQueueSend(cmdQueue, &msg, 0);
}

void ma_get_list(ListKind kind, ListData &out) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    out = lists[kind];
    xSemaphoreGive(stateLock);
}

uint32_t ma_last_poll_ms() { return lastPollMs; }

bool ma_recently_ok(uint32_t withinMs) { return everOk && millis() - lastOkMs < withinMs; }

void ma_get_state(PlayerState &out) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    out = shared;
    xSemaphoreGive(stateLock);
}

bool ma_art_begin_read() {
    if (!artReady || xSemaphoreTake(artLock, 0) != pdTRUE) return false;
    if (!artReady) {
        xSemaphoreGive(artLock);
        return false;
    }
    return true;
}

void ma_art_end_read() {
    artReady = false;
    LittleFS.remove(MA_ART_PATH);
    xSemaphoreGive(artLock);
}

bool ma_auth_lost() { return authLost; }

uint32_t ma_list_version(ListKind kind) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    uint32_t v = lists[kind].version;
    xSemaphoreGive(stateLock);
    return v;
}
