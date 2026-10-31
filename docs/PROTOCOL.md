# Moonlight S3 wire protocol v3 session control / v2 PIN authorization / v1 JPEG framing

## 中文版本说明

当前配套版本使用 v3 会话控制、v2 PIN 授权和 v1 JPEG 帧格式，必须同步更新电脑端与固件；旧版 JPEG 帧格式相同不代表会话兼容。菜单暂停要求 PAU3/PAK3 确认，性能浮窗使用 STA3/OVR3；使用入口及配置迁移见 [SESSION_UX.md](SESSION_UX.md)，升级分区要求见 [RELEASING.md](RELEASING.md)。电脑端当前通过完整设备身份自动发现和连接，手填 IP 与 USB 配网按钮已从主界面移除，底层协议和诊断能力保留。

TCP 47989, one PC per receiver. All uint32 fields are big endian.
Messages may be fragmented/coalesced arbitrarily by TCP; parsers accumulate exact lengths.
Session state belongs to one TCP connection; reconnect resets framing and authentication.

## LAN connection and discovery

Receiver uses STA only, joins the existing 2.4GHz router, and never starts a SoftAP.
The PC can connect through Ethernet or Wi-Fi on the same reachable LAN.
Discovery query: UDP port47990, ASCII `MS3DISC1 ` + 32 random lowercase hex characters (41 bytes).
Reply to the query source IP/port: `MS3HERE1 <nonce> <device-id> <tcp-port> <hostname>`.
Device ID is the full12-digit lowercase STA MAC without separators. Hostname is
`moonlight-s3-` + last6 digits of ID; names are not unique identifiers.
Reply contains no password/token. Host validates nonce, ID, name and port, deduplicates
by ID+address+port, and requires the user to choose a result. Discovery is not authentication.
Receiver also advertises `_moonlight-s3._tcp` over mDNS with TXT id/version.
IP/hostname addressing remains available at the protocol/tool layer for diagnostics; the current PC GUI uses automatic discovery and has no manual address entry.

## Initial Wi-Fi provisioning over USB

115200 baud on the same serial interface as firmware logs. Newline-terminated commands.
`MS3STATUS1` responds `MS3STATUS1 <device-id> <IPv4> CONNECTED|WAITING|UNCONFIGURED`.
Only after recognizing this response, the PC sends:
`MS3WIFI1 <UTF8-SSID-as-hex> <UTF8-password-as-hex-or-dash>`.
SSID:1..32 bytes; password:empty,8..63 bytes,or64 hexadecimal PSK characters; embedded NUL rejected.
Receiver atomically stores one credentials blob in Preferences namespace `moonlight`, key `wifi`.
Success response `MS3WIFI1 OK RESTART` means stored, not connected; device then reboots.
Errors use `MS3WIFI1 ERROR <reason>`. Passwords are not logged. Provisioned credentials
override compile-time defaults; connection retries keep running without an AP fallback.

## Authorization and authentication (v2)

The old `AUT1` plus fixed shared token is rejected. Physical four-digit first authorization uses SRP-6a, RFC5054 Appendix A 2048-bit group, g=2, SHA256 throughout. This is application framing, not TLS-SRP or GameStream.

All binary integers in SRP are left-padded to256 bytes. Host ID is32 lowercase ASCII hex, device ID12 ASCII hex, salt16 random bytes. Physical screen action opens a120-second window with at most5 attempts, including interrupted sessions. No network command opens this window. Old authorization is replaced only after successful proof and NVS write.

1. PC → S3: `PAR2` + host ID32 (36 bytes).
2. S3 → PC: `SRP2` + device ID12 + salt16 + B256 (288 bytes).
3. PC → S3: `SRA2` + A256 + client proof32 (292 bytes).
4. S3 → PC: `PAK2` + server proof32 + device ID12 (48 bytes), then close. A failed/expired exchange closes without saving.

Let H=SHA256, I=`MoonlightS3:` + device ID, P=exact four ASCII digits. x=H(salt || H(I || `:` || P)); v=g^x mod N; k=H(PAD(N)||PAD(g)); B=(k*v+g^b) mod N; u=H(PAD(A)||PAD(B)). Client S=(B-k*g^x)^(a+u*x) mod N, server S=(A*v^u)^b mod N. K=H(PAD(S)). Both public values must satisfy1<value<N; u=0 is rejected. Private a/b have at least256 random bits.

T=device ID12 || host ID32 || salt16 || PAD(A) || PAD(B).
Client proof=HMAC-SHA256(K, `client-v2:` || T), server proof uses `server-v2:`. Long-term key is the first16 bytes of HMAC-SHA256(K, `secret-v2:` || T), encoded as32 lowercase ASCII hex. PIN and long-term key are never transmitted directly. These domain-separated application proofs are custom MS3 v2, not the RFC5054 TLS Finished messages.

Subsequent TCP connections:

1. PC → S3: `AU3?` + saved host ID32 + active request ID uint32 (40 bytes). S3 checks this exact physically initiated request before issuing the challenge and before accepting its proof. Cancelled/expired requests and legacy AU2 are rejected. The request field is a lifecycle guard, not an extension to the proof transcript.
2. S3 → PC: `CHL2` + device ID12 + fresh nonce32 (48 bytes).
3. PC → S3: `PRF2` +64 ASCII hex HMAC(key, `client` || device ID12 || host ID32 || nonce32), total68 bytes.
4. S3 → PC: `OKA2` +64 ASCII hex HMAC(key, `server` || same transcript), total68 bytes, followed by `RDY1` below. Proofs apply only to the outstanding challenge on that TCP connection.

Successful authentication emits16 bytes: `RDY1` + uint32 width800 + uint32 height480 + uint32 maximum JPEG262144. Video packets and input injection begin only after authentication completes; fixed credentials are not used as fallback.

This protects the authorization exchange from passive PIN guessing and ordinary proof replay. Video/touch packets are not encrypted or MAC-protected; this is not a TLS implementation and does not protect streamed data against an active on-path relay/modification. Local NVS/settings are not encrypted. See PAIRING.md for deployment limits and tests.

## Host to device

16-byte frame header followed by exactly `length` bytes:

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[4] | JPG1 |
| 4 | uint32 | Sequence, wraps at 2^32 |
| 8 | uint32 | JPEG length, 4..262144 inclusive |
| 12 | uint32 | CRC-32/ISO-HDLC of JPEG, same as Python zlib.crc32 / esp_rom_crc32_le(0,...) |

CRC test vector: ASCII `123456789` gives `0xcbf43926`.
JPEG is baseline, 4:2:0, either 800×480 or 400×240; decoder validates dimensions and output length.
At most three unacknowledged frames in smooth mode, or two otherwise. Host captures a new frame only after a credit is available.
Both success and error acknowledge/retire the named sequence; current host stops on ERR1.
Partial header/body timeout is 3 seconds of no receive progress. The host additionally enforces a 3-second ACK timeout.

## Device to host

All packets: tag char[4], a uint32, b uint32, c uint32, total 16 bytes.

| Tag | a | b | c |
|---|---|---|---|
| ACK1 | Sequence | Parse+decode microseconds | Total decode/upscale/submit/synchronization microseconds |
| ERR1 | Sequence | Decode microseconds if available | Total microseconds if available |
| TCH1 | Panel x 0..799 | Panel y 0..479 | Left contact state 0 or 1 |

Touch is polled every 10ms. TCH1 sends changes immediately after polling, held-contact
heartbeats every 20ms, and idle/no-touch heartbeats every 50ms. Full state is always sent.
This reduces small TCP packet traffic while remaining inside the 350ms host watchdog.
Device suppresses initial pressed state until it observes touch-up after authentication.
Host releases input after 350ms without TCH1 and requires a fresh lift before accepting another press.
Host releases on stop, exception or disconnect. No screen-to-host executable commands are transmitted.

## Rendering and touch

Logical screen coordinates always remain 800×480 even when JPEG uses 400×240.
With toolbar enabled, y=448..479 is reserved for six buttons: LEFT x0..159, RIGHT x160..319, SCROLL x320..479, KEYBOARD x480..639, BACK x640..719, MENU x720..799.
Desktop is fitted with preserved aspect ratio in the remaining rectangle.
Black-bar touches do not initiate a press; an existing drag leaving the image clamps at the image boundary.
Coordinates map into the selected Windows monitor's physical pixel rectangle, including negative origins.
SendInput absolute coordinates are normalized to the entire Windows virtual desktop.
Toolbar is interpreted on the PC; firmware has no dependence on GUI layout.

The receiver uses three RGB buffers with exclusive ownership: scanned buffer,
queued/prepared buffer, and decoder buffer. Only the LCD-complete callback retires
the previous scanned buffer. Decoder and presenter tasks overlap; ACK1 still
follows the actual completed LCD switch, and the host permits three frames
in flight in smooth mode, or two when smooth mode is disabled. Network/idle notices require both JPEG and RGB queues to be idle.


## Computer discovery and screen-initiated requests (MS3HOST2 / MS3DEV2)

The PC discovers receivers using MS3DISC1, then announces itself by outbound unicast to each receiver's UDP47990. The same ephemeral PC socket receives replies; no new fixed incoming PC port/firewall rule is introduced. While streaming this PC pauses discovery/announcements. Receivers expire computers after10 seconds without announcements; up to8 live computers and4 saved authorizations are supported.

PC → S3 (ASCII space-separated):
`MS3HOST2 <nonce32hex> <computer-id32hex> <receiver-id12hex> <known0-or-1> <UTF8-name-as-hex> 1 <signature>`

Name is1..48 UTF8 bytes; controls are rejected. Known=1 only when PC has credentials for the exact receiver and this stable computer ID. Signature is64 lowercase hex HMAC-SHA256 using the saved ASCII long-term key, covering everything before the last separating space; otherwise `-`. Discovery has no secrets. Computer status is paired only when S3 also holds that exact computer credential and verifies the announcement.

S3 → PC:
`MS3DEV2 <same-nonce> <same-computer-id> <receiver-id> <action-id8hex> <paired0-or-1> <signature>`

Paired=1 requires a valid HMAC over the prefix, checked against the exact device credential and source IP/port. Paired=0 uses `-` and can only request a PIN dialog, never auto-authorize capture/input. Action0 means no request. A physical tap creates a random nonzero action; paired requests last15 seconds, PIN requests120 seconds. The PC deduplicates IDs so one tap does not produce repeated dialogs.

The S3 screen opens a PIN window only for the tapped computer ID, and PAR2 must carry that ID. After proof success the PC saves credentials then immediately starts the ordinary authenticated stream. BACK returns to the computer list. Pairing deletion is local on either endpoint; the next announcement detects the missing/mismatched authorization and falls back to first pairing. Old MS3CTL1 is no longer used or accepted by this firmware.


Authentication replay boundary: the nonce is freshly sampled from the ESP32-S3 hardware RNG for each connection, and the pending challenge is consumed on the first proof attempt and cleared on disconnect. Recorded proofs do not authenticate a new nonce. This protection does not authenticate/encrypt JPG1/TCH1 payloads or prevent live on-path relay. See PAIRING.md for the plain-language explanation and historical device-test evidence.

## Session control v3 (supersedes HOST2/DEV2 play flow above)

The PC continues announcing during capture, including busy state for other receivers:

`MS3HOST3 nonce32 pcID32 deviceID12 known nameHex I|B phase action8hex reason monitors signature`

S3 replies:

`MS3DEV3 nonce32 pcID32 deviceID12 action8hex paired options8hex flow signature`

HMAC-SHA256 uses the saved credential and covers the full ASCII prefix before the final space/signature. Unknown credentials use `-` and never authorize automatic capture. Names and addresses are not identities. HOST2/DEV2 are recognized for address probing only; current GUI requires v3 for starting sessions.

Phases: 0 idle, 1 accepted, 2 PIN, 3 verifying, 4 preparing, 5 first frame, 6 streaming, 7 declined, 8 failed, 9 disconnected, 10 cancelled. Reasons: 0 none, 1 network, 2 authorization, 3 busy, 4 capture, 5 input, 6 decode, 7 expired, 8 user stopped, 9 version. Status applies only to the selected PC and current action. Terminal states stay terminal until a new physical request.

Flow: 0 no request, 1 pending, 2 streaming, 3 cancelled, 4 ended/failed/expired. PC handles cancellation/expiry even while PIN dialog is open. Initial waiting timeout is 15 seconds; accepted preparation permits up to 60 seconds, first pairing 120 seconds. Reconnection requires a fresh physical request ID.

Options uint32: bit0 trackpad, bit2 toolbar, bit3 control allowed, bit4 400x240 (clear=800x480), bits8..11 monitor1..8, bits16..23 JPEG quality30..90, bits24..29 FPS10..30. Other bits reserved. Options live in S3 NVS `ms3ui/profiles`, per exact PC identity.

Current extension: options bit1 enables the S3 performance overlay (default off). S3 sends OVR3 with a=0/1 for immediate switching from its local menu. The host serializes STA3 `(a=latest frame-ACK microseconds,b=overlay0/1,c=0)` with JPEG packets: about once per second when enabled, once on disabling. This is not ping and does not change authentication or input permissions. RX Mbps is counted locally from received JPEG payload bytes. Overlay text is composed into internal decode/upscale rows before the existing framebuffer copy, never into the scanned front buffer.

Current toolbar supersedes older coordinates: six equal logical slots with edges 0/133/266/400/533/666/800, y448..479. BACK occupies533..665 and MENU666..799; the local KEYBOARD hit region is400..532. Display and input boundaries are identical. Hidden toolbar retains only MENU in its same bottom-right slot.

Pause drain extension: PAU3(a=1,b=monotonic pause ID,c=0) requests a pause; the PC finishes any already-started packet, stops creating JPEG frames, and serializes PAK3(a=matching pause ID,b=0,c=0) before idle NOP3 heartbeats. The S3 continues draining JPEG bodies/returning DRP3 while waiting; it paints/enables local-menu touch only after matching PAK3 and idle decode/display queues. PAU3(a=0,b=same pause ID,c=0) resumes. Stale PAK3 cannot release another pause. All post-auth packets share partial-write retry logic with the unchanged 3-second failure deadline; the socket's 100ms receive timeout must not directly terminate a control write. Both endpoints require this pause extension. See MENU_PAUSE_FIX.md for evidence and tests.

Current application storage uses dedicated NVS partition `ms3_store`, offset0x910000, size64KB. First boot copies the application's existing `moonlight/wifi`, `ms3pair/peers` or legacy `peer`, and `ms3ui/profiles` from default NVS if absent in the destination. Existing destination keys, including an empty peers table, take precedence so reboot cannot restore a deleted pairing. Source NVS and unrelated projects are retained. Wi-Fi driver persistence is disabled; explicit application writes target the dedicated partition. Partition table and app must be updated together when migrating an older installation.

Additional 16-byte `(tag,a,b,c)` messages from S3: PAU3 a=1 pause/0 resume; BYE3 disconnect; MOD3 a=0 direct/1 trackpad; KEY3 a=0..7 (Esc,Enter,Tab,Backspace,Ctrl+C,Ctrl+V,Ctrl+A,Alt+Tab); DRP3 a=sequence retires an in-flight frame dropped while pausing. TXT3 has a=1..128 followed by exactly a bytes UTF-8. Current screen keyboard produces ASCII. These are fixed input actions, not shell commands; KEY3/TXT3 are ignored in view-only mode.

While paused the host sends NOP3 every 500ms; TCH1 idle heartbeat continues. The receiver drains in-flight JPG1 bodies and sends DRP3 so the local UI cannot race JPEG rendering. MENU at logical x720..799,y448..479 remains available when the bottom toolbar is hidden. Opening MENU releases input; resume requires touch-up before another press.
