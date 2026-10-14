#include "app_trace.h"

#if USE_SYSVIEW

/* Olay aciklamasi (SystemView kullanici kilavuzu UM08027, "Registering the
 * module"): "M=<modul>, <no> <olay> <parametreler>, ...". Hedef bu metni en
 * fazla SEGGER_SYSVIEW_MAX_STRING_LEN (128) karakter gonderir; fazlasi
 * kesilir, bu yuzden adlar kisa tutuldu. Sira app_trace.h'deki enum ile
 * ayni olmali. Parametreler kilavuzdaki gibi "ad=%u" bicimindedir (n = olay
 * kimligi); '#' kullanilmaz: SystemView aciklama sozdiziminde yorum
 * baslatir. */
#define APP_TRACE_MODULE_DESC                                                  \
    "M=BTN, 0 ACCEPT n=%u S=%u, 1 TASK n=%u, 2 READY n=%u, 3 ENQUEUE n=%u q=%u, " \
    "4 DROP n=%u at=%u, 5 TX_START n=%u, 6 TX_TC n=%u"

_Static_assert(sizeof(APP_TRACE_MODULE_DESC) - 1U <= SEGGER_SYSVIEW_MAX_STRING_LEN,
               "SystemView modul aciklamasi 128 karakteri asiyor, kesilir");

SEGGER_SYSVIEW_MODULE g_app_trace_module = {
    APP_TRACE_MODULE_DESC,
    APP_EV_COUNT,
    0,    /* EventOffset: SEGGER_SYSVIEW_RegisterModule() yazar */
    NULL, /* ek aciklama yok */
    NULL, /* pNext: SEGGER_SYSVIEW_RegisterModule() yazar */
};

/* Hafta 3 queue deneyi; sira app_trace.h'deki APP_W3_* ile ayni. */
#define APP_W3_MODULE_DESC                                                     \
    "M=W3, 0 SEND_BEGIN n=%u, 1 SEND_OK n=%u us=%u q=%u, 2 SEND_FAIL n=%u us=%u, " \
    "3 RECEIVE n=%u lat=%u, 4 DONE n=%u"

_Static_assert(sizeof(APP_W3_MODULE_DESC) - 1U <= SEGGER_SYSVIEW_MAX_STRING_LEN,
               "SystemView W3 modul aciklamasi 128 karakteri asiyor, kesilir");

SEGGER_SYSVIEW_MODULE g_app_w3_module = {
    APP_W3_MODULE_DESC,
    APP_W3_COUNT,
    0,
    NULL,
    NULL,
};

/* BTN once kaydedilir: olay numaralari 512 (BTN) ve 519 (W3) ile baslar. */
void app_trace_register(void)
{
    SEGGER_SYSVIEW_RegisterModule(&g_app_trace_module);
    SEGGER_SYSVIEW_RegisterModule(&g_app_w3_module);
}

void app_trace_window(char variant, uint8_t scenario)
{
    char text[] = "Pencere ?/S?";
    text[8] = variant;
    text[11] = (char)('0' + scenario);
    SEGGER_SYSVIEW_Print(text);
}

static char *put_str(char *p, const char *s)
{
    while (*s != '\0')
    {
        *p++ = *s++;
    }
    return p;
}

static char *put_dec(char *p, uint32_t v)
{
    char tmp[10];
    unsigned n = 0;
    do
    {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0U)
    {
        *p++ = tmp[--n];
    }
    return p;
}

void app_trace_q_run_begin(uint16_t run, char variant, bool burst, const void *queue,
                           uint8_t capacity)
{
    char name[8]; /* "MsgQ16" */
    char *p = put_dec(put_str(name, "MsgQ"), capacity);
    *p = '\0';
    SEGGER_SYSVIEW_NameResource((U32)queue, name);

    char text[32]; /* "Kosu 65535 C/normal MsgQ16" */
    p = put_dec(put_str(text, "Kosu "), run);
    *p++ = ' ';
    *p++ = variant;
    *p++ = '/';
    p = put_str(p, burst ? "yogun " : "normal ");
    p = put_str(p, name);
    *p = '\0';
    SEGGER_SYSVIEW_Print(text);
}

void app_trace_q_run_end(uint16_t run)
{
    char text[20]; /* "Kosu 65535 bitti" */
    char *p = put_str(put_dec(put_str(text, "Kosu "), run), " bitti");
    *p = '\0';
    SEGGER_SYSVIEW_Print(text);
}

#endif /* USE_SYSVIEW */
