#pragma once
#include <Arduino.h>

struct MaSetupRequest {
    String server;
    String token;
    String username;
    String password;
    String returnUrl; // when set, only request the Home Assistant sign-in URL
};

void web_portal_begin();
void web_portal_loop();
bool web_portal_take_setup(MaSetupRequest &out);
void web_portal_set_result(bool ok, const String &error);
void web_portal_set_ha_url(const String &url);
