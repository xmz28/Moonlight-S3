# Moonlight S3 wire protocol v2 authorization / v1 JPEG framing

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
Manual IP/hostname remains supported when broadcast discovery cannot reach the receiver.

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

1. PC → S3: `AU2?` + saved host ID32 (36 bytes).
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
With toolbar enabled, y=448..479 is reserved for five 160px buttons: LEFT, RIGHT, SCROLL, KEYBOARD, BACK.
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
