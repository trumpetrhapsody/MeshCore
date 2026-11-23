#pragma once
// Experimental hybrid room server for the ESP32-S3 companion firmware.
// The companion remains the sole owner of the LoRa dispatcher and USB protocol.
#ifdef ENABLE_HYBRID_ROOM_SERVER

#if !defined(ESP32)
#error "Hybrid room server currently supports only ESP32 boards"
#endif

#include <Arduino.h>
#include <Mesh.h>
#include <SPIFFS.h>
#include <helpers/ClientACL.h>

#ifndef HYBRID_ROOM_NAME
#define HYBRID_ROOM_NAME "MeshCore HA Room"
#endif
#ifndef HYBRID_ROOM_PASSWORD
#define HYBRID_ROOM_PASSWORD "CHANGE_ME"
#endif
#ifndef HYBRID_ROOM_ADMIN_PASSWORD
#define HYBRID_ROOM_ADMIN_PASSWORD "CHANGE_ADMIN"
#endif
#ifndef HYBRID_ROOM_POSTS
#define HYBRID_ROOM_POSTS 64
#endif
#if HYBRID_ROOM_POSTS < 1 || HYBRID_ROOM_POSTS > 255
#error "HYBRID_ROOM_POSTS must be between 1 and 255"
#endif

class MyMesh;

class HybridRoom {
public:
  explicit HybridRoom(MyMesh& owner);
  void begin();
  void loop();
  bool isAddressed(const mesh::Packet* packet) const;
  mesh::DispatcherAction receive(mesh::Packet* packet);
  bool consumeAck(uint32_t ack);
  const mesh::LocalIdentity& identity() const { return _identity; }
  uint32_t countPosts() const { return _total_posts; }
  bool handleLocalCLI(const char* command, char* reply, size_t reply_size);
  bool isEnabled() const { return _enabled; }

private:
  enum { MAX_POST_TEXT = 151 };
  struct Config {
    uint32_t magic;
    uint8_t enabled;
    char name[33];
    char guest_password[33];
    char admin_password[33];
    uint32_t checksum;
  };
  struct Post {
    mesh::Identity author;
    uint32_t timestamp;
    char text[MAX_POST_TEXT + 1];
  };
  MyMesh& _mesh;
  mesh::LocalIdentity _identity;
  ClientACL _clients;
  Post _posts[HYBRID_ROOM_POSTS];
  Config _config;
  bool _enabled;
  bool _identity_collision;
  uint16_t _next_post;
  uint32_t _total_posts, _total_pushes;
  uint16_t _next_client;
  unsigned long _next_push, _next_advert, _next_flood_advert;
  unsigned long _dirty_acl;
  unsigned long _dirty_posts;
  uint64_t _uptime_ms;
  uint32_t _last_ms;

  bool loadConfig();
  bool saveConfig();
  bool loadHistory();
  bool saveHistory();
  void sendAdvert(bool flood);
  void scheduleAdvert();
  void sendFlood(mesh::Packet* packet, uint32_t delay, uint8_t hash_size=1);
  void sendReply(mesh::Packet* received, ClientInfo& client, uint8_t type,
                 const uint8_t* payload, size_t length, uint32_t delay);
  void replyAck(mesh::Packet* received, ClientInfo& client, uint32_t crc, uint32_t delay,
                int trailing_count=-1);
  mesh::Packet* makeDatagram(uint8_t type, const mesh::Identity& dest,
                            const uint8_t* secret, const uint8_t* data, size_t len);
  mesh::Packet* makePathReturn(const mesh::Identity& dest, const uint8_t* secret,
                               const uint8_t* path, uint8_t path_len,
                               uint8_t extra_type, const uint8_t* extra, size_t extra_len);
  void handleLogin(mesh::Packet* packet, const mesh::Identity& sender,
                   const uint8_t* secret, uint8_t* data, size_t len);
  void handleKnown(mesh::Packet* packet, ClientInfo& client,
                   uint8_t* data, size_t len, uint8_t type);
  void handlePath(ClientInfo& client, uint8_t* data, size_t len);
  void storePost(const mesh::Identity& author, const char* text);
  void pushPost(ClientInfo& client, Post& post);
  uint8_t countUnsynced(const ClientInfo& client) const;
  static bool persistAdminOnly(ClientInfo* client);
};
#endif
