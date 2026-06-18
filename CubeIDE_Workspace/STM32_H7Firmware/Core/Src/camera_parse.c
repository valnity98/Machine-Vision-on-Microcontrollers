/*
 * camera_parse.c
 *
 * ASCII command-line parsers for the STM32 Edge Vision UART protocol.
 */

#include "camera_parse.h"

#include <string.h>

uint8_t camparse_parse_level(const char *s, ov2640_level_t *lvl)
{
    if ((s == NULL) || (lvl == NULL)) { return 0u; }

    if (strcmp(s, "-2") == 0) { *lvl = OV2640_LEVEL_MINUS2; return 1u; }
    if (strcmp(s, "-1") == 0) { *lvl = OV2640_LEVEL_MINUS1; return 1u; }
    if (strcmp(s, "0")  == 0 ||
        strcmp(s, "+0") == 0) { *lvl = OV2640_LEVEL_0;      return 1u; }
    if (strcmp(s, "+1") == 0 ||
        strcmp(s, "1")  == 0) { *lvl = OV2640_LEVEL_PLUS1;  return 1u; }
    if (strcmp(s, "+2") == 0 ||
        strcmp(s, "2")  == 0) { *lvl = OV2640_LEVEL_PLUS2;  return 1u; }

    return 0u;
}

uint8_t camparse_parse_res(const char *s, ov2640_resolution_t *r)
{
    if ((s == NULL) || (r == NULL)) { return 0u; }

    if (ascii_equal(s, "QQVGA")) { *r = OV2640_RES_QQVGA_160x120; return 1u; }
    if (ascii_equal(s, "QVGA"))  { *r = OV2640_RES_QVGA_320x240;  return 1u; }
    if (ascii_equal(s, "WQVGA")) { *r = OV2640_RES_WQVGA_480x272; return 1u; }
    if (ascii_equal(s, "VGA"))   { *r = OV2640_RES_VGA_640x480;   return 1u; }
    if (ascii_equal(s, "SVGA"))  { *r = OV2640_RES_SVGA_800x600;  return 1u; }
    if (ascii_equal(s, "XGA"))   { *r = OV2640_RES_XGA_1024x768;  return 1u; }
    if (ascii_equal(s, "SXGA"))  { *r = OV2640_RES_SXGA_1280x960; return 1u; }

    return 0u;
}

uint8_t camparse_parse_effect(const char *s, ov2640_effect_t *e)
{
    if ((s == NULL) || (e == NULL)) { return 0u; }

    if (ascii_equal(s, "NORMAL"))      { *e = OV2640_EFFECT_NORMAL;      return 1u; }
    if (ascii_equal(s, "BW"))          { *e = OV2640_EFFECT_BW;          return 1u; }
    if (ascii_equal(s, "NEGATIVE"))    { *e = OV2640_EFFECT_NEGATIVE;    return 1u; }
    if (ascii_equal(s, "NEGATIVE_BW")) { *e = OV2640_EFFECT_NEGATIVE_BW; return 1u; }
    if (ascii_equal(s, "BLUISH"))      { *e = OV2640_EFFECT_BLUISH;      return 1u; }
    if (ascii_equal(s, "GREENISH"))    { *e = OV2640_EFFECT_GREENISH;    return 1u; }
    if (ascii_equal(s, "REDDISH"))     { *e = OV2640_EFFECT_REDDISH;     return 1u; }
    if (ascii_equal(s, "ANTIQUE"))     { *e = OV2640_EFFECT_ANTIQUE;     return 1u; }

    return 0u;
}

uint8_t camparse_parse_light(const char *s, ov2640_lightmode_t *m)
{
    if ((s == NULL) || (m == NULL)) { return 0u; }

    if (ascii_equal(s, "AUTO"))   { *m = OV2640_LIGHT_AUTO;   return 1u; }
    if (ascii_equal(s, "SUNNY"))  { *m = OV2640_LIGHT_SUNNY;  return 1u; }
    if (ascii_equal(s, "CLOUDY")) { *m = OV2640_LIGHT_CLOUDY; return 1u; }
    if (ascii_equal(s, "OFFICE")) { *m = OV2640_LIGHT_OFFICE; return 1u; }
    if (ascii_equal(s, "HOME"))   { *m = OV2640_LIGHT_HOME;   return 1u; }

    return 0u;
}

uint8_t camparse_uart_readline_from_queue(camera_app_t *app,
                                          char         *out,
                                          size_t        maxlen,
                                          TickType_t    wait)
{
    uint8_t ch;

    if ((app == NULL) || (app->rx_q == NULL) ||
        (out == NULL) || (maxlen == 0u)) {
        return 0u;
    }

    if (xQueueReceive(app->rx_q, &ch, wait) != pdTRUE) { return 0u; }

    if (ch == '\r') { return 0u; }

    if (ch == '\n') {
        size_t n = (app->rx_line_len < maxlen) ? app->rx_line_len : (maxlen - 1u);
        memcpy(out, app->rx_line, n);
        out[n] = '\0';
        app->rx_line_len = 0u;
        return 1u;
    }

    if ((app->rx_line_len + 1u) < sizeof(app->rx_line)) {
        app->rx_line[app->rx_line_len++] = (char)ch;
    } else {
        /* Line too long — discard and wait for next newline. */
        app->rx_line_len = 0u;
    }

    return 0u;
}
