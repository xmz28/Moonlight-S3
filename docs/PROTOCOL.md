# Moonlight S3 wire protocol v1

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

## Authentication

Host sends 36 bytes: ASCII `AUT1` + 32 ASCII hex characters of shared pairing token.
The token comparison is case sensitive. No newline, NUL, or JSON.
Receiver closes immediately on mismatch. Success reply is 16 bytes:

| Offset | Type | Value |
|---|---|---|
| 0 | char[4] | RDY1 |
| 4 | uint32 | 800 (panel width) |
| 8 | uint32 | 480 (panel height) |
| 12 | uint32 | 262144 (maximum JPEG bytes) |

The token is transmitted in clear text. This is local pairing, not TLS or mutual cryptographic authentication.

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


## Receiver-screen start request (MS3CTL1)

While idle, the PC GUI polls only its selected receiver over unicast UDP47990 once per second. No new PC listening port is introduced. Streaming disables this polling.

Query: `MS3CTL1 <32-lowercase-hex-nonce> <64-lowercase-hex-HMAC>` (105 ASCII bytes).
Reply: `MS3CTL1 <nonce> <12-hex-device-id> <8-hex-action-id> <64-hex-HMAC>` (127 bytes).
HMAC-SHA256 uses the shared 32-character ASCII pairing token as key, covering all bytes before the final separating space and signature. The pairing token itself is not sent in these UDP packets. The PC checks nonce, signature, source IP/port and selected device identity when known.

Touching Start creates a random nonzero action ID valid for15 seconds; zero means no pending request. The PC deduplicates actions and starts the normal TCP session. Authentication clears the request; entering Wi-Fi settings cancels it. BACK is interpreted by the host toolbar controller and ends the TCP session after releasing input. BACK remains enabled in view-only mode. TCP pairing/video are still unencrypted.
