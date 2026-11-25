/* Optic Link: show a screen on another device over Wi-Fi (an R36S running "Optic Link") and
 * take its buttons back as input. The same file is used by Optic DE (optic-link) and by
 * Velvet on the CardputerZero; the R36S side is ~/r36s/opticlink.
 *
 *   discovery  every second the sender broadcasts UDP to port 47800:
 *                "OLINK1 <tcp port> <free|busy> <name>"
 *   handshake  receiver connects (TCP 47801) and sends   "OLINK1 HELLO <id> <name>\n"
 *              sender: "OK <name>\n" (id already trusted) or "CODE\n"; then the receiver sends
 *              "PIN <4 digits>\n" (the code the sender shows) and gets "OK <name>\n" or "NO\n".
 *              A paired receiver's id is remembered, so the code is asked once.
 *   stream     messages [u8 type][u32 length, little endian][payload]
 *              sender -> receiver: FRAME (u16 w, h: a new picture size, full refresh follows),
 *                TILE (u16 x, y, w, h, u8 enc 0 raw / 1 zlib, RGB565 pixels), END (a picture is
 *                complete), TEXT (a status line), PING
 *              receiver -> sender: BTN (u8 button, u8 down), AXIS (u8 axis, i16 value), PING
 * Pictures are at most 640 x 480, RGB565. */
#ifndef OLINK_H
#define OLINK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define OLINK_BEACON_PORT 47800
#define OLINK_PORT 47801
#define OLINK_MAXW 640
#define OLINK_MAXH 480

enum { OL_FRAME = 1, OL_TILE = 2, OL_END = 3, OL_TEXT = 4, OL_PING = 5,
       OL_BTN = 10, OL_AXIS = 11 };

/* buttons, by position (SDL game controller names) */
enum { OLB_A, OLB_B, OLB_X, OLB_Y, OLB_L1, OLB_R1, OLB_L2, OLB_R2, OLB_SELECT, OLB_START,
       OLB_UP, OLB_DOWN, OLB_LEFT, OLB_RIGHT, OLB_L3, OLB_R3, OLB_COUNT };
enum { OLA_LX, OLA_LY, OLA_RX, OLA_RY };

typedef struct { int type; int code; int value; } OLinkEv;    /* OL_BTN / OL_AXIS */

enum { OLS_WAITING, OLS_PAIRING, OLS_LIVE };

typedef struct OLink OLink;

/* Starts sharing: a background thread announces [name] and serves one receiver at a time.
   [trust] is the file that remembers paired receivers. NULL when the port is taken. */
OLink *olink_start(const char *name, const char *trust);
void olink_stop(OLink *l);
/* OLS_*; while pairing, [code] gets the 4 digits to show; [peer] the receiver's name */
int olink_state(OLink *l, char code[5], char *peer, int peerlen);
/* 1 when a receiver is watching and the last picture has gone out: time to capture another */
int olink_wants_frame(OLink *l);
/* hands over a picture (copied), RGB565, w <= 640, h <= 480 */
void olink_frame(OLink *l, const uint16_t *px, int w, int h);
void olink_text(OLink *l, const char *s);
/* the receiver's next button or stick event; 0 when there's none */
int olink_input(OLink *l, OLinkEv *ev);

#ifdef __cplusplus
}
#endif
#endif
