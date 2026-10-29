# Moonlight S3 wire protocol v1

TCP 47989, one PC per receiver. All uint32 fields are big endian.
Messages may be fragmented/coalesced arbitrarily by TCP; parsers accumulate exact lengths.
Session state belongs to one TCP connection; reconnect resets framing and authentication.

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
At most two unacknowledged frames. Host captures a new frame only after a credit is available.
Both success and error acknowledge/retire the named sequence; current host stops on ERR1.
Partial header/body timeout is 3 seconds of no receive progress. The host additionally enforces a 3-second ACK timeout.

## Device to host

All packets: tag char[4], a uint32, b uint32, c uint32, total 16 bytes.

| Tag | a | b | c |
|---|---|---|---|
| ACK1 | Sequence | Parse+decode microseconds | Total decode/upscale/submit/synchronization microseconds |
| ERR1 | Sequence | Decode microseconds if available | Total microseconds if available |
| TCH1 | Panel x 0..799 | Panel y 0..479 | Left contact state 0 or 1 |

TCH1 repeats full state approximately every 10ms, including no-touch heartbeats.
Device suppresses initial pressed state until it observes touch-up after authentication.
Host releases input after 350ms without TCH1 and requires a fresh lift before accepting another press.
Host releases on stop, exception or disconnect. No screen-to-host executable commands are transmitted.

## Rendering and touch

Logical screen coordinates always remain 800×480 even when JPEG uses 400×240.
With toolbar enabled, y=448..479 is reserved for four 200px buttons.
Desktop is fitted with preserved aspect ratio in the remaining rectangle.
Black-bar touches do not initiate a press; an existing drag leaving the image clamps at the image boundary.
Coordinates map into the selected Windows monitor's physical pixel rectangle, including negative origins.
SendInput absolute coordinates are normalized to the entire Windows virtual desktop.
Toolbar is interpreted on the PC; firmware has no dependence on GUI layout.
