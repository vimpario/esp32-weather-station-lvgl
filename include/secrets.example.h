#pragma once

// Copy locally to include/secrets.h and keep that local file out of Git.
// Never commit real Wi-Fi credentials.
//
// The real password lives ONLY in the untracked include/secrets.h.

#define WIFI_SSID "Beeline_2G_F121F9"
#define WIFI_PASSWORD "CHANGE_ME"

// Work mode: static IPv4 is mandatory for this project.
// Do not commit include/secrets.h with the real password.
#define WIFI_USE_STATIC_IP 1
#define WIFI_STATIC_IP "192.168.1.111"
#define WIFI_GATEWAY "192.168.1.1"
#define WIFI_SUBNET "255.255.255.0"
#define WIFI_DNS1 "192.168.1.1"
#define WIFI_DNS2 "1.1.1.1"
