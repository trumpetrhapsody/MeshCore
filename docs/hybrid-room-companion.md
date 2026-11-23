# Experimental MeshCore room server + companion (XIAO ESP32-S3 + Wio-SX1262)

**Status: experimental development branch. Do not flash an operational repeater.**
The existing RAK 1 W repeater and the \`rak1w-bthome\` branch are unchanged.

## Goal and architecture

One ESP32-S3 and **one SX1262 transceiver** run both identities:

- Standard **companion** identity and unchanged serial framing, used by the MeshCore Home Assistant integration. USB is the preferred connection. Optional Wi-Fi/TCP uses the existing companion Wi-Fi stack.
- Independent **room-server** keypair, advertised as \`ADV_TYPE_ROOM\`, accepting room login, text posts, keepalive, status/telemetry requests, acknowledgments and push synchronization.

This is intentionally **not** repeater firmware. Both roles share the companion's single packet dispatcher, packet manager, receive path, and transmit queue. The room handler intercepts only packets addressed to its own destination hash; unrelated packets continue to the stock companion stack. There are no independent competing radio loops.

## Build targets

PlatformIO environments:

- \`Xiao_S3_WIO_hybrid_room_companion_usb\` — **recommended**; serial companion protocol over USB.
- \`Xiao_S3_WIO_hybrid_room_companion_usb_wifi\` — exposes USB, and optionally enables the stock Wi-Fi/TCP companion interface after credentials are configured.

Example:

\`\`\`sh
pio run -e Xiao_S3_WIO_hybrid_room_companion_usb
\`\`\`

Use the usual ESP32-S3 flashing process for the correct hardware variant. Do not assume the binary is verified until both CI builds complete and runtime testing succeeds. A build result is not evidence of RF/protocol interoperability.

## Provisioning (USB companion CLI)

The room starts **disabled** and has **no default password**. The room configuration is stored separately in SPIFFS. Use a client that can send \`CMD_RUN_CLI_COMMAND\` over the companion serial link, such as a compatible MeshCore app / developer CLI. Do not type these commands into the framed companion USB serial stream as if it were the old room server's plain-text serial CLI.

\`\`\`text
room.status
room.name Vinculum Room
room.guest_password <choose-a-unique-password-8-to-32-characters>
room.admin_password <choose-an-independent-password-12-to-32-characters>
room.enabled 1
room.advert
\`\`\`

Values in angle brackets are examples: enter actual passwords **without** the brackets. The CLI does not echo credentials back in responses. The saved settings are plaintext on device flash, so protect the hardware and backups. Turn the room off using \`room.enabled 0\`. Setting \`room.enabled 0\` does not turn off the HA companion role.

The first boot generates a **separate** room key at \`/_hybrid_room.id\`; it must not share the companion's public-key prefix. If it collides with the companion's one-byte hash (unlikely), the room is disabled to avoid ambiguous routing. Factory erasure of SPIFFS changes the room identity and loses room history.

## History and persistence

The proof-of-concept room ring stores the latest **128 posts in RAM** by default (compile-time \`HYBRID_ROOM_POSTS\`). A debounced full history snapshot is written to SPIFFS about 60 seconds after the latest new post. On boot a checksum verifies the saved snapshot. This can preserve the recent room history across clean reboots.

**Known caveats:** SPIFFS snapshot replacement isn't fully power-fail atomic; a reboot before the delayed save loses the latest posts; high message volume causes flash wear and can briefly block radio processing during a snapshot. There's no HA archive export yet, and the buffer cannot deliver overwritten messages. A proper append-only journal or an HA-side archiver is future work.

## Network and testing

Configure the companion's LoRa parameters to **exactly** match the regional mesh / your RAK 1 W repeater. Region/flood scope behavior uses the companion's default scope, and nonmatching scopes need additional validation.

Minimum on-device smoke test, with the working repeater still untouched:

1. Flash the XIAO S3/Wio board and confirm normal companion connection over USB from the stock app.
2. Check \`room.status\`, set unique room credentials, and enable it.
3. Confirm a second contact appears as a **room server** after \`room.advert\`, with a **different public key** from the HA companion.
4. From another portable companion, log into the room using the guest password and send a message. Verify an ACK and that a second room participant receives the post.
5. Disconnect/reconnect the other companion, verify the room sends missed posts without duplicate messages.
6. Connect Home Assistant through the USB serial device using the stock MeshCore integration and poll the RAK repeater's statistics. Check this doesn't interrupt the room service.
7. Send interleaved room posts and HA repeater queries while watching serial logs and queue pressure. Reboot the board and check whether saved history reloads.

A pass means the two identities work concurrently *under the tested traffic conditions*. Further tests should cover wrong passwords, hash collisions, malformed packets, duplicate ACKs, flash exhaustion, high traffic, and mesh routing through repeaters.

## Explicit limitations

- **Untested on physical hardware** until someone flashes and exercises the branch.
- The first version prioritizes room login, posting, synchronization and read-only status/telemetry. It does **not** implement every room CLI/admin operation, persistent per-client ACK state, full transport-scope selection, or native HA history archive.
- The room supports the normal companion radio independently; it does not advertise repeater capability or forward mesh traffic.
- USB and Wi-Fi should not normally be used by two different controlling HA clients at the same time.
- Keep credentials private and do not expose the optional TCP companion interface outside the trusted LAN/VLAN.
- Verify runtime memory headroom and packet-pool usage before raising the post count further.

## Implementation map

- \`examples/companion_radio/HybridRoom.{h,cpp}\` — room identity, room protocol, flash history, runtime CLI.
- \`examples/companion_radio/MyMesh.{h,cpp}\` — destination-hash dispatch and ACK hook; otherwise retains stock companion behavior.
- \`variants/xiao_s3_wio/hybrid.ini\` + root \`platformio.ini\` — hardware build environments.
- \`.github/workflows/hybrid-room-companion-build.yml\` — compile both variants.
