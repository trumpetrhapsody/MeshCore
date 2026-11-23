#include "HybridRoom.h"
#ifdef ENABLE_HYBRID_ROOM_SERVER

#include "MyMesh.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/IdentityStore.h>
#include <helpers/TxtDataHelpers.h>
#include <target.h>
#include <stddef.h>
#include <string.h>

namespace {
constexpr uint32_t CONFIG_MAGIC = 0x52484331; // RHC1
constexpr uint32_t HISTORY_MAGIC = 0x52485031; // RHP1
constexpr unsigned long PUSH_INTERVAL_MS = 1200;
constexpr unsigned long ACK_WAIT_FLOOD_MS = 12000;
constexpr unsigned long ACK_WAIT_DIRECT_MS = 10000;
constexpr unsigned long POST_DELAY_SECS = 6;
constexpr unsigned long SAVE_DELAY_MS = 60000;
const char* const CONFIG_PATH = "/hybrid_room.cfg";
const char* const HISTORY_PATH = "/hybrid_room_posts.dat";
const char* const HISTORY_TMP = "/hybrid_room_posts.tmp";

struct RoomStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t noise_floor;
  int16_t last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood;
  uint32_t n_sent_direct;
  uint32_t n_recv_flood;
  uint32_t n_recv_direct;
  uint16_t err_events;
  int16_t last_snr;
  uint16_t n_direct_dups, n_flood_dups;
  uint16_t n_posted, n_post_push;
};

struct HistoryHeader {
  uint32_t magic;
  uint16_t count;
  uint16_t next;
  uint32_t total;
  uint32_t checksum;
};

uint32_t checksumBytes(const void* bytes, size_t len) {
  uint32_t hash = 2166136261UL;
  const uint8_t* data = static_cast<const uint8_t*>(bytes);
  while (len--) {
    hash ^= *data++;
    hash *= 16777619UL;
  }
  return hash;
}
void boundedCopy(char* dst, const char* src, size_t capacity) {
  if (!capacity) return;
  strncpy(dst, src, capacity - 1);
  dst[capacity - 1] = 0;
}
}

HybridRoom::HybridRoom(MyMesh& owner)
  : _mesh(owner), _enabled(false), _identity_collision(false), _next_post(0), _total_posts(0),
    _total_pushes(0), _next_client(0), _next_push(0), _next_advert(0),
    _next_flood_advert(0), _dirty_acl(0), _dirty_posts(0),
    _uptime_ms(0), _last_ms(0) {
  memset(&_config, 0, sizeof(_config));
  memset(_posts, 0, sizeof(_posts));
}

bool HybridRoom::loadConfig() {
  File file = SPIFFS.open(CONFIG_PATH, "r");
  if (!file || file.size() != sizeof(_config)) return false;
  Config saved = {};
  bool ok = file.read(reinterpret_cast<uint8_t*>(&saved), sizeof(saved)) == sizeof(saved);
  file.close();
  if (!ok || saved.magic != CONFIG_MAGIC) return false;
  if (checksumBytes(&saved, offsetof(Config, checksum)) != saved.checksum) return false;
  if (!memchr(saved.name, 0, sizeof(saved.name)) ||
      !memchr(saved.guest_password, 0, sizeof(saved.guest_password)) ||
      !memchr(saved.admin_password, 0, sizeof(saved.admin_password))) return false;
  _config = saved;
  return true;
}

bool HybridRoom::saveConfig() {
  _config.magic = CONFIG_MAGIC;
  _config.checksum = checksumBytes(&_config, offsetof(Config, checksum));
  File file = SPIFFS.open(CONFIG_PATH, "w");
  if (!file) return false;
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&_config), sizeof(_config)) == sizeof(_config);
  file.close();
  return ok;
}

bool HybridRoom::loadHistory() {
  File file = SPIFFS.open(HISTORY_PATH, "r");
  if (!file || file.size() != (sizeof(HistoryHeader) + sizeof(_posts))) return false;
  HistoryHeader header = {};
  bool ok = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header);
  ok = ok && header.magic == HISTORY_MAGIC &&
       header.count == HYBRID_ROOM_POSTS && header.next < HYBRID_ROOM_POSTS;
  if (ok) {
    ok = file.read(reinterpret_cast<uint8_t*>(_posts), sizeof(_posts)) == sizeof(_posts);
    ok = ok && (checksumBytes(_posts, sizeof(_posts)) ^ header.next ^ header.total) == header.checksum;
  }
  file.close();
  if (!ok) {
    memset(_posts, 0, sizeof(_posts));
    return false;
  }
  _next_post = header.next;
  _total_posts = header.total;
  return true;
}

bool HybridRoom::saveHistory() {
  HistoryHeader header = {};
  header.magic = HISTORY_MAGIC;
  header.count = HYBRID_ROOM_POSTS;
  header.next = _next_post;
  header.total = _total_posts;
  header.checksum = checksumBytes(_posts, sizeof(_posts)) ^ header.next ^ header.total;
  File file = SPIFFS.open(HISTORY_TMP, "w");
  if (!file) return false;
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);
  ok = ok && file.write(reinterpret_cast<const uint8_t*>(_posts), sizeof(_posts)) == sizeof(_posts);
  file.close();
  if (!ok) {
    SPIFFS.remove(HISTORY_TMP);
    return false;
  }
  // SPIFFS.rename cannot replace an existing destination on all ESP32 builds.
  // A power failure between removal and rename loses the snapshot, not the radio identity.
  SPIFFS.remove(HISTORY_PATH);
  return SPIFFS.rename(HISTORY_TMP, HISTORY_PATH);
}

void HybridRoom::begin() {
  IdentityStore ids(SPIFFS, "");
  if (!ids.load("_hybrid_room", _identity)) {
    do {
      _identity = radio_new_identity();
    } while (_identity.pub_key[0] == 0 || _identity.pub_key[0] == 0xFF ||
             _identity.isHashMatch(_mesh.self_id.pub_key));
    ids.save("_hybrid_room", _identity);
  }
  // The standard protocol uses a 1-byte destination hash. Never silently let
  // a room identity shadow the HA companion's destination hash.
  if (_identity.isHashMatch(_mesh.self_id.pub_key)) {
    MESH_DEBUG_PRINTLN("Hybrid room identity hash collides with companion identity; room disabled");
    _identity_collision = true;
    _enabled = false;
    return;
  }
  if (!loadConfig()) {
    memset(&_config, 0, sizeof(_config));
    _config.magic = CONFIG_MAGIC;
    boundedCopy(_config.name, HYBRID_ROOM_NAME, sizeof(_config.name));
    // Intentionally disabled until name and credentials are provisioned over the
    // companion's USB CLI. No default public/admin password in shipped firmware.
  }
  _enabled = _config.enabled && strlen(_config.admin_password) >= 12 &&
             strlen(_config.guest_password) >= 8;
  _clients.load(&SPIFFS, _identity);
  loadHistory();
  _last_ms = millis();
  scheduleAdvert();
  MESH_DEBUG_PRINTLN("HybridRoom: %s, %lu stored posts",
                     _enabled ? "enabled" : "disabled", (unsigned long)_total_posts);
}

bool HybridRoom::handleLocalCLI(const char* cmd, char* reply, size_t reply_size) {
  if (strncmp(cmd, "room.", 5) != 0) return false;
  if (strcmp(cmd, "room.status") == 0) {
    snprintf(reply, reply_size, "room: %s, name=%s, retained=%lu/%u, total=%lu",
             _enabled ? "on" : "off", _config.name,
             (unsigned long)min(_total_posts, (uint32_t)HYBRID_ROOM_POSTS),
             (unsigned)HYBRID_ROOM_POSTS, (unsigned long)_total_posts);
    return true;
  }
  if (strcmp(cmd, "room.advert") == 0) {
    if (_enabled) sendAdvert(false);
    snprintf(reply, reply_size, _enabled ? "OK - room advertisement queued" : "ERR - room is disabled");
    return true;
  }
  const struct { const char* prefix; char* dest; size_t capacity; size_t min_len; } settings[] = {
    {"room.name ", _config.name, sizeof(_config.name), 1},
    {"room.guest_password ", _config.guest_password, sizeof(_config.guest_password), 8},
    {"room.admin_password ", _config.admin_password, sizeof(_config.admin_password), 12}
  };
  for (const auto& s : settings) {
    if (strncmp(cmd, s.prefix, strlen(s.prefix)) != 0) continue;
    const char* value = cmd + strlen(s.prefix);
    size_t len = strlen(value);
    if (len < s.min_len || len >= s.capacity ||
        (s.dest == _config.name && !AdvertDataParser::isValidName(value))) {
      snprintf(reply, reply_size, "ERR - expected %u..%u characters",
               (unsigned)s.min_len, (unsigned)(s.capacity - 1));
      return true;
    }
    boundedCopy(s.dest, value, s.capacity);
    bool saved = saveConfig();
    snprintf(reply, reply_size, saved ? "OK - saved (value hidden)" : "ERR - saving failed");
    return true;
  }
  if (strncmp(cmd, "room.enabled ", 13) == 0) {
    if (strcmp(cmd + 13, "0") != 0 && strcmp(cmd + 13, "1") != 0) {
      snprintf(reply, reply_size, "ERR - use room.enabled 0 or room.enabled 1");
      return true;
    }
    bool enable = cmd[13] == '1';
    if (enable && (_identity_collision ||
                   strlen(_config.guest_password) < 8 ||
                   strlen(_config.admin_password) < 12 ||
                   strcmp(_config.guest_password, _config.admin_password) == 0)) {
      snprintf(reply, reply_size, "ERR - check identity collision and distinct guest (8+)/admin (12+) passwords");
      return true;
    }
    _config.enabled = enable;
    bool saved = saveConfig();
    if (saved) {
      _enabled = enable;
      if (_enabled) {
        scheduleAdvert();
        _next_flood_advert = millis() + 5000;
      }
    }
    snprintf(reply, reply_size, saved ? "OK - room %s" : "ERR - saving failed",
             enable ? "enabled" : "disabled");
    return true;
  }
  snprintf(reply, reply_size, "ERR - unknown room command");
  return true;
}

bool HybridRoom::isAddressed(const mesh::Packet* packet) const {
  if (!_enabled || !packet || packet->payload_len == 0) return false;
  if (packet->isRouteDirect() && packet->getPathHashCount() > 0) return false;
  switch (packet->getPayloadType()) {
    case PAYLOAD_TYPE_ANON_REQ:
    case PAYLOAD_TYPE_REQ:
    case PAYLOAD_TYPE_RESPONSE:
    case PAYLOAD_TYPE_TXT_MSG:
    case PAYLOAD_TYPE_PATH:
      return _identity.isHashMatch(packet->payload);
    case PAYLOAD_TYPE_ADVERT:
      return packet->payload_len >= PUB_KEY_SIZE &&
             _identity.matches(packet->payload);
    default:
      return false;
  }
}

mesh::DispatcherAction HybridRoom::receive(mesh::Packet* packet) {
  if (_mesh.getTables()->wasSeen(packet)) return ACTION_RELEASE;
  // Keep room messages out of the standard companion's identity-dependent
  // decrypt/dispatch path, while sharing one radio and one deduplication table.
  if (packet->getPayloadType() == PAYLOAD_TYPE_ADVERT) {
    _mesh.getTables()->markSeen(packet);
    return ACTION_RELEASE;
  }
  uint8_t data[MAX_PACKET_PAYLOAD + 1];
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) {
    size_t prefix = 1 + PUB_KEY_SIZE;
    if (packet->payload_len <= prefix + CIPHER_MAC_SIZE) return ACTION_RELEASE;
    mesh::Identity sender(&packet->payload[1]);
    uint8_t secret[PUB_KEY_SIZE];
    _identity.calcSharedSecret(secret, sender);
    int len = mesh::Utils::MACThenDecrypt(secret, data,
                                          &packet->payload[prefix], packet->payload_len - prefix);
    if (len > 8 && len <= MAX_PACKET_PAYLOAD) {
      data[len] = 0;
      _mesh.getTables()->markSeen(packet);
      handleLogin(packet, sender, secret, data, len);
    }
    return ACTION_RELEASE;
  }

  if (packet->payload_len <= 2 + CIPHER_MAC_SIZE) return ACTION_RELEASE;
  const uint8_t source_hash = packet->payload[1];
  for (int i = 0; i < _clients.getNumClients(); i++) {
    ClientInfo* client = _clients.getClientByIdx(i);
    if (!client->id.isHashMatch(&source_hash)) continue;
    int len = mesh::Utils::MACThenDecrypt(client->shared_secret, data, &packet->payload[2],
                                          packet->payload_len - 2);
    if (len <= 0 || len > MAX_PACKET_PAYLOAD) continue;
    data[len] = 0;
    _mesh.getTables()->markSeen(packet);
    handleKnown(packet, *client, data, len, packet->getPayloadType());
    return ACTION_RELEASE;
  }
  return ACTION_RELEASE;
}

void HybridRoom::sendFlood(mesh::Packet* packet, uint32_t delay, uint8_t hash_size) {
  if (!packet) return;
  TransportKey scope;
  memcpy(scope.key, _mesh.getNodePrefs()->default_scope_key, sizeof(scope.key));
  if (scope.isNull()) {
    _mesh.sendFlood(packet, delay, hash_size);
  } else {
    // Use the companion's configured default flood scope for room replies.
    uint16_t codes[2] = {scope.calcTransportCode(packet), 0};
    _mesh.sendFlood(packet, codes, delay, hash_size);
  }
}

mesh::Packet* HybridRoom::makeDatagram(uint8_t type, const mesh::Identity& dest,
                                       const uint8_t* secret, const uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_RESPONSE && type != PAYLOAD_TYPE_TXT_MSG) return nullptr;
  if (len + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE - 1 > MAX_PACKET_PAYLOAD - 2) return nullptr;
  auto pkt = _mesh.obtainNewPacket();
  if (!pkt) return nullptr;
  pkt->header = type << PH_TYPE_SHIFT;
  size_t i = 0;
  i += dest.copyHashTo(&pkt->payload[i]);
  i += _identity.copyHashTo(&pkt->payload[i]);
  i += mesh::Utils::encryptThenMAC(secret, &pkt->payload[i], data, len);
  pkt->payload_len = i;
  return pkt;
}

mesh::Packet* HybridRoom::makePathReturn(const mesh::Identity& dest, const uint8_t* secret,
                                         const uint8_t* path, uint8_t path_len,
                                         uint8_t extra_type, const uint8_t* extra, size_t extra_len) {
  if (!mesh::Packet::isValidPathLen(path_len)) return nullptr;
  size_t path_bytes = (path_len & 63) * ((path_len >> 6) + 1);
  if (path_bytes + extra_len + 5 > MAX_PACKET_PAYLOAD - 2 - CIPHER_BLOCK_SIZE) return nullptr;
  auto pkt = _mesh.obtainNewPacket();
  if (!pkt) return nullptr;
  pkt->header = PAYLOAD_TYPE_PATH << PH_TYPE_SHIFT;
  size_t i = 0;
  i += dest.copyHashTo(&pkt->payload[i]);
  i += _identity.copyHashTo(&pkt->payload[i]);
  uint8_t payload[MAX_PACKET_PAYLOAD];
  size_t n = 0;
  payload[n++] = path_len;
  memcpy(&payload[n], path, path_bytes);
  n += path_bytes;
  payload[n++] = extra_type;
  if (extra_len) {
    memcpy(&payload[n], extra, extra_len);
    n += extra_len;
  }
  i += mesh::Utils::encryptThenMAC(secret, &pkt->payload[i], payload, n);
  pkt->payload_len = i;
  return pkt;
}

void HybridRoom::sendReply(mesh::Packet* incoming, ClientInfo& client, uint8_t type,
                           const uint8_t* payload, size_t length, uint32_t delay) {
  if (type == PAYLOAD_TYPE_RESPONSE && incoming && incoming->isRouteFlood() &&
      incoming->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) {
    auto path = makePathReturn(client.id, client.shared_secret,
                               incoming->path, incoming->path_len,
                               PAYLOAD_TYPE_RESPONSE, payload, length);
    if (path) {
      sendFlood(path, delay, incoming->getPathHashSize());
      return;
    }
  }
  auto reply = makeDatagram(type, client.id, client.shared_secret, payload, length);
  if (!reply) return;
  if (client.out_path_len != OUT_PATH_UNKNOWN &&
      mesh::Packet::isValidPathLen(client.out_path_len)) {
    _mesh.sendDirect(reply, client.out_path, client.out_path_len, delay);
  } else {
    sendFlood(reply, delay, incoming ? incoming->getPathHashSize() : 1);
  }
}

void HybridRoom::replyAck(mesh::Packet* incoming, ClientInfo& client, uint32_t crc,
                          uint32_t delay, int trailing_count) {
  auto ack = _mesh.createAck(crc);
  if (!ack) return;
  if (trailing_count >= 0 && ack->payload_len < MAX_PACKET_PAYLOAD) {
    ack->payload[ack->payload_len++] = static_cast<uint8_t>(trailing_count);
  }
  if (client.out_path_len != OUT_PATH_UNKNOWN &&
      mesh::Packet::isValidPathLen(client.out_path_len)) {
    _mesh.sendDirect(ack, client.out_path, client.out_path_len, delay);
  } else {
    sendFlood(ack, delay, incoming ? incoming->getPathHashSize() : 1);
  }
}

void HybridRoom::handleLogin(mesh::Packet* packet, const mesh::Identity& sender,
                              const uint8_t* secret, uint8_t* data, size_t len) {
  if (len <= 8) return;
  uint32_t timestamp, since;
  memcpy(&timestamp, data, 4);
  memcpy(&since, &data[4], 4);
  const char* password = reinterpret_cast<const char*>(&data[8]);
  ClientInfo* client = nullptr;
  if (!*password) client = _clients.getClient(sender.pub_key, PUB_KEY_SIZE);
  if (!client) {
    uint8_t permissions;
    if (!strcmp(password, _config.admin_password)) permissions = PERM_ACL_ADMIN;
    else if (!strcmp(password, _config.guest_password)) permissions = PERM_ACL_READ_WRITE;
    else return;
    client = _clients.putClient(sender, permissions);
    if (!client || timestamp <= client->last_timestamp) return;
    client->permissions = permissions;
    client->extra.room.sync_since = since;
    client->extra.room.pending_ack = 0;
    client->extra.room.push_failures = 0;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);
    _dirty_acl = millis() + 5000;
  }
  if (timestamp < client->last_timestamp) return;
  client->last_timestamp = timestamp;
  client->last_activity = _mesh.getRTCClock()->getCurrentTime();
  if (packet->isRouteFlood()) client->out_path_len = OUT_PATH_UNKNOWN;
  uint8_t response[13] = {};
  uint32_t now = _mesh.getRTCClock()->getCurrentTimeUnique();
  memcpy(response, &now, 4);
  response[4] = 0; // RESP_SERVER_LOGIN_OK
  response[6] = client->isAdmin() ? 1 : 0;
  response[7] = client->permissions;
  _mesh.getRNG()->random(&response[8], 4);
  response[12] = 1; // room protocol revision
  _next_push = millis() + 2000;
  sendReply(packet, *client, PAYLOAD_TYPE_RESPONSE, response, sizeof(response), 1500);
}

void HybridRoom::handlePath(ClientInfo& client, uint8_t* data, size_t len) {
  if (len < 2 || !mesh::Packet::isValidPathLen(data[0])) return;
  size_t path_bytes = (data[0] & 63) * ((data[0] >> 6) + 1);
  if (1 + path_bytes + 1 > len) return;
  client.out_path_len = mesh::Packet::copyPath(client.out_path, &data[1], data[0]);
  client.last_activity = _mesh.getRTCClock()->getCurrentTime();
  if (data[1 + path_bytes] == PAYLOAD_TYPE_ACK && len >= 1 + path_bytes + 5) {
    uint32_t crc;
    memcpy(&crc, &data[2 + path_bytes], 4);
    consumeAck(crc);
  }
}

void HybridRoom::handleKnown(mesh::Packet* packet, ClientInfo& client,
                             uint8_t* data, size_t len, uint8_t type) {
  if (type == PAYLOAD_TYPE_PATH) {
    handlePath(client, data, len);
    return;
  }
  if (len < 5) return;
  uint32_t timestamp;
  memcpy(&timestamp, data, 4);
  if (timestamp < client.last_timestamp) return;
  bool retry = timestamp == client.last_timestamp;
  client.last_timestamp = timestamp;
  client.last_activity = _mesh.getRTCClock()->getCurrentTime();
  client.extra.room.push_failures = 0;

  if (type == PAYLOAD_TYPE_TXT_MSG) {
    uint8_t flags = data[4] >> 2;
    if (flags != TXT_TYPE_PLAIN ||
        (client.permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) return;
    size_t text_len = strnlen(reinterpret_cast<char*>(&data[5]), len - 5);
    if (text_len > MAX_POST_TEXT) text_len = MAX_POST_TEXT;
    if (!text_len) return;
    if (!retry) {
      char text[MAX_POST_TEXT + 1];
      memcpy(text, &data[5], text_len);
      text[text_len] = 0;
      storePost(client.id, text);
    }
    uint32_t hash;
    mesh::Utils::sha256(reinterpret_cast<uint8_t*>(&hash), 4,
                         data, 5 + text_len, client.id.pub_key, PUB_KEY_SIZE);
    replyAck(packet, client, hash, 200);
    return;
  }

  if (type == PAYLOAD_TYPE_REQ) {
    if (data[4] == 2 && packet->isRouteDirect()) { // KEEP_ALIVE
      uint8_t tagged[9] = {};
      memcpy(tagged, data, min(len, sizeof(tagged)));
      if (len >= sizeof(tagged)) {
        uint32_t since;
        memcpy(&since, &data[5], 4);
        if (since) client.extra.room.sync_since = since;
      }
      client.extra.room.pending_ack = 0;
      uint32_t hash;
      mesh::Utils::sha256(reinterpret_cast<uint8_t*>(&hash), 4, tagged,
                           sizeof(tagged), client.id.pub_key, PUB_KEY_SIZE);
      replyAck(packet, client, hash, 1500, countUnsynced(client));
      return;
    }

    uint8_t response[MAX_PACKET_PAYLOAD] = {};
    memcpy(response, data, 4); // tag original request
    size_t response_len = 0;
    if (data[4] == 1) { // GET_STATUS, matching stock room-server wire layout
      RoomStats stats = {};
      stats.batt_milli_volts = board.getBattMilliVolts();
      stats.curr_tx_queue_len = _mesh._mgr->getOutboundTotal();
      stats.noise_floor = _mesh._radio->getNoiseFloor();
      stats.last_rssi = radio_driver.getLastRSSI();
      stats.n_packets_recv = radio_driver.getPacketsRecv();
      stats.n_packets_sent = radio_driver.getPacketsSent();
      stats.total_air_time_secs = _mesh.getTotalAirTime() / 1000;
      stats.total_up_time_secs = _uptime_ms / 1000;
      stats.n_sent_flood = _mesh.getNumSentFlood();
      stats.n_sent_direct = _mesh.getNumSentDirect();
      stats.n_recv_flood = _mesh.getNumRecvFlood();
      stats.n_recv_direct = _mesh.getNumRecvDirect();
      stats.err_events = _mesh._err_flags;
      stats.last_snr = radio_driver.getLastSNR() * 4;
      stats.n_posted = _total_posts;
      stats.n_post_push = _total_pushes;
      memcpy(&response[4], &stats, sizeof(stats));
      response_len = 4 + sizeof(stats);
    } else if (data[4] == 3) { // GET_TELEMETRY_DATA
      CayenneLPP telemetry(MAX_PACKET_PAYLOAD - 4);
      telemetry.addVoltage(TELEM_CHANNEL_SELF, board.getBattMilliVolts() / 1000.0f);
      float temp = board.getMCUTemperature();
      if (!isnan(temp)) telemetry.addTemperature(TELEM_CHANNEL_SELF, temp);
      memcpy(&response[4], telemetry.getBuffer(), telemetry.getSize());
      response_len = 4 + telemetry.getSize();
    }
    if (response_len) sendReply(packet, client, PAYLOAD_TYPE_RESPONSE, response, response_len, 1500);
  }
}

void HybridRoom::storePost(const mesh::Identity& author, const char* text) {
  Post& post = _posts[_next_post];
  post.author = author;
  boundedCopy(post.text, text, sizeof(post.text));
  post.timestamp = _mesh.getRTCClock()->getCurrentTimeUnique();
  _next_post = (_next_post + 1) % HYBRID_ROOM_POSTS;
  ++_total_posts;
  _dirty_posts = millis() + SAVE_DELAY_MS; // debounce flash writes
  _next_push = millis() + 2000;
}

uint8_t HybridRoom::countUnsynced(const ClientInfo& client) const {
  uint8_t result = 0;
  for (const auto& post : _posts) {
    if (post.timestamp > client.extra.room.sync_since &&
        !post.author.matches(client.id)) ++result;
  }
  return result;
}

void HybridRoom::pushPost(ClientInfo& client, Post& post) {
  uint8_t data[MAX_POST_TEXT + 9] = {};
  size_t n = 0;
  memcpy(&data[n], &post.timestamp, 4); n += 4;
  uint8_t attempt;
  _mesh.getRNG()->random(&attempt, 1);
  data[n++] = (TXT_TYPE_SIGNED_PLAIN << 2) | (attempt & 3);
  memcpy(&data[n], post.author.pub_key, 4); n += 4;
  size_t text_len = strnlen(post.text, sizeof(post.text));
  memcpy(&data[n], post.text, text_len); n += text_len;
  auto packet = makeDatagram(PAYLOAD_TYPE_TXT_MSG, client.id, client.shared_secret, data, n);
  if (!packet) return;
  mesh::Utils::sha256(reinterpret_cast<uint8_t*>(&client.extra.room.pending_ack), 4,
                       data, n, client.id.pub_key, PUB_KEY_SIZE);
  client.extra.room.push_post_timestamp = post.timestamp;
  if (client.out_path_len == OUT_PATH_UNKNOWN) {
    sendFlood(packet, 0);
    client.extra.room.ack_timeout = millis() + ACK_WAIT_FLOOD_MS;
  } else {
    _mesh.sendDirect(packet, client.out_path, client.out_path_len);
    client.extra.room.ack_timeout = millis() + ACK_WAIT_DIRECT_MS;
  }
  ++_total_pushes;
}

bool HybridRoom::consumeAck(uint32_t ack) {
  if (!ack) return false;
  for (int i = 0; i < _clients.getNumClients(); ++i) {
    ClientInfo* client = _clients.getClientByIdx(i);
    if (client->extra.room.pending_ack != ack) continue;
    client->extra.room.pending_ack = 0;
    client->extra.room.push_failures = 0;
    client->extra.room.sync_since = client->extra.room.push_post_timestamp;
    return true;
  }
  return false;
}

bool HybridRoom::persistAdminOnly(ClientInfo* client) {
  return client->isAdmin();
}

void HybridRoom::scheduleAdvert() {
  _next_advert = millis() + 120000;
  _next_flood_advert = millis() + 15000;
}

void HybridRoom::sendAdvert(bool flood) {
  if (!_enabled) return;
  uint8_t data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder advert(ADV_TYPE_ROOM, _config.name);
  size_t len = advert.encodeTo(data);
  auto packet = _mesh.createAdvert(_identity, data, len);
  if (!packet) return;
  if (flood) sendFlood(packet, 0);
  else _mesh.sendZeroHop(packet);
}

void HybridRoom::loop() {
  unsigned long now = millis();
  _uptime_ms += (uint32_t)(now - _last_ms);
  _last_ms = now;

  if (_dirty_posts && (int32_t)(now - _dirty_posts) >= 0) {
    if (saveHistory()) _dirty_posts = 0;
    else _dirty_posts = now + SAVE_DELAY_MS;
  }
  if (_dirty_acl && (int32_t)(now - _dirty_acl) >= 0) {
    _clients.save(&SPIFFS, persistAdminOnly);
    _dirty_acl = 0;
  }
  if (!_enabled) return;
  if (_next_advert && (int32_t)(now - _next_advert) >= 0) {
    sendAdvert(false);
    _next_advert = now + 30UL*60UL*1000UL;
  }
  if (_next_flood_advert && (int32_t)(now - _next_flood_advert) >= 0) {
    sendAdvert(true);
    _next_flood_advert = now + 12UL*60UL*60UL*1000UL;
  }
  int clients = _clients.getNumClients();
  if (clients == 0 || (int32_t)(now - _next_push) < 0) return;
  if (_next_client >= clients) _next_client = 0;
  ClientInfo* client = _clients.getClientByIdx(_next_client);
  _next_client = (_next_client + 1) % clients;
  if (client->extra.room.pending_ack && (int32_t)(now - client->extra.room.ack_timeout) >= 0) {
    client->extra.room.pending_ack = 0;
    ++client->extra.room.push_failures;
  }
  if (client->last_activity && !client->extra.room.pending_ack &&
      client->extra.room.push_failures < 3) {
    uint32_t current = _mesh.getRTCClock()->getCurrentTime();
    for (int k = 0, j = _next_post; k < HYBRID_ROOM_POSTS; k++, j = (j+1)%HYBRID_ROOM_POSTS) {
      Post& post = _posts[j];
      if (post.timestamp && current >= post.timestamp + POST_DELAY_SECS &&
          post.timestamp > client->extra.room.sync_since &&
          !post.author.matches(client->id)) {
        pushPost(*client, post);
        _next_push = now + PUSH_INTERVAL_MS;
        return;
      }
    }
  }
  _next_push = now + PUSH_INTERVAL_MS/8;
}
#endif
