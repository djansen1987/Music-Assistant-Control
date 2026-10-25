#pragma once
#include <Arduino.h>
#include <vector>

#define MA_DEFAULT_PORT 8095
#define MA_ART_PATH "/art.jpg"

enum PlayerCmd : uint8_t {
    CMD_PLAY_PAUSE = 1,
    CMD_NEXT,
    CMD_PREV,
    CMD_VOL_UP,
    CMD_VOL_DOWN,
    CMD_LIST_PLAYERS,
    CMD_LIST_PLAYLISTS,
    CMD_LIST_WEB_PLAYLISTS,
    CMD_LIST_WEB_PLAYERS,
    CMD_SEEK,
    CMD_SELECT_PLAYER,
    CMD_PLAY_PLAYLIST,
};

enum ListKind : uint8_t { LIST_PLAYERS, LIST_PLAYLISTS, LIST_WEB_PLAYLISTS, LIST_WEB_PLAYERS };
enum ListStatus : uint8_t { LIST_IDLE, LIST_LOADING, LIST_READY, LIST_ERROR };

struct ListItem {
    String id;
    String name;
    bool active = false;
    bool pinned = false;
    String owner;
};

struct ListData {
    ListStatus status = LIST_IDLE;
    std::vector<ListItem> items;
    String error;
    String key;
    uint32_t version = 0;
    uint32_t offset = 0;
    uint32_t next = 0;
    uint32_t total = 0;
};

struct PlayerState {
    bool active = false;
    bool playing = false;
    String trackId;
    String title;
    String artist;
    String album;
    String deviceName;
    String artUrl;
    String nerd;
    String message;
    uint32_t progressMs = 0;
    uint32_t durationMs = 0;
    int volume = -1;
    uint32_t stamp = 0;
};

void ma_init();
void ma_start();
bool ma_connect(const String &server, const String &token, const String &username, const String &password, String &error);
bool ma_ha_login_url(const String &server, const String &returnUrl, String &authUrl, String &error);
bool ma_configured();
bool ma_server_reachable();
void ma_send(PlayerCmd cmd);
void ma_seek(uint32_t positionMs);
void ma_request_list(ListKind kind, uint32_t offset = 0);
void ma_select(ListKind kind, const String &id, const String &name = "");
void ma_get_list(ListKind kind, ListData &out);
void ma_get_state(PlayerState &out);
bool ma_art_begin_read();
void ma_art_end_read();
bool ma_auth_lost();
uint32_t ma_last_poll_ms();
bool ma_recently_ok(uint32_t withinMs);
uint32_t ma_list_version(ListKind kind);
