/* Hafta 3 queue deneyi icin bilgisayar (host) testi: yuk tarifi
 * (queue_exp.h) ve QRUN/QBUSY/QMSG/QCNT/QEND satirlari (protocol.c).
 *
 * Derleme (depo kokunden):
 *   gcc -std=c11 -Wall -Wextra -I firmware/Core/Inc
 *       tests/host/test_queue_exp.c firmware/Core/Src/protocol.c -o test_queue_exp
 *   ./test_queue_exp
 *
 * Icerik: normal ve yogun yukte 44 girisim, art arda bolumun tam 24 girisim
 * olmasi ve aradaki beklemeler; her satirin 64 bayt (63 + LF) olmasi, en
 * uzun QMSG'nin 63 bayta sigmasi, basarisiz gonderimde gecikme alanlarinin
 * 0 degil BOS yazilmasi. */
#include <stdio.h>
#include <string.h>
#include "queue_exp.h"
#include "protocol.h"

static int g_fail;
#define CHECK(c, msg) do { if (!(c)) { printf("  HATA: %s (satir %d)\n", msg, __LINE__); g_fail++; } } while (0)

static void test_profile(void)
{
    printf("Yuk tarifi\n");
    uint32_t sum_normal = 0, sum_burst = 0, burst_count = 0, zero_gaps = 0;
    for (uint32_t seq = 1; seq <= QEXP_ATTEMPTS; seq++)
    {
        CHECK(!qexp_is_burst_attempt(QEXP_LOAD_NORMAL, seq), "normal yukte art arda girisim olmamali");
        const uint32_t gn = qexp_gap_after_ms(QEXP_LOAD_NORMAL, seq);
        CHECK(gn == (seq == QEXP_ATTEMPTS ? 0U : QEXP_GAP_MS), "normal: her girisimden sonra 100 ms");
        sum_normal += gn;

        const bool b = qexp_is_burst_attempt(QEXP_LOAD_BURST, seq);
        burst_count += b ? 1U : 0U;
        CHECK(b == (seq >= 11U && seq <= 34U), "yogun: art arda bolum 11..34");
        const uint32_t gb = qexp_gap_after_ms(QEXP_LOAD_BURST, seq);
        sum_burst += gb;
        if (gb == 0U)
        {
            zero_gaps++;
        }
        if (seq >= 11U && seq <= 33U)
        {
            CHECK(gb == 0U, "yogun: art arda girisimler arasinda bekleme yok");
        }
        else if (seq != QEXP_ATTEMPTS)
        {
            CHECK(gb == QEXP_GAP_MS, "yogun: normal bolumde ve art arda bolumden sonra 100 ms");
        }
    }
    CHECK(burst_count == QEXP_BURST_LEN, "art arda bolum 24 girisim");
    CHECK(zero_gaps == 23U + 1U, "23 sifir aralik + son girisim");
    CHECK(sum_normal == 43U * 100U, "normal: 43 x 100 ms");
    CHECK(sum_burst == 20U * 100U, "yogun: 20 x 100 ms");
    printf("  normal: 44 girisim, toplam bekleme %u ms\n", (unsigned)sum_normal);
    printf("  yogun : 44 girisim, %u art arda, toplam bekleme %u ms\n", (unsigned)burst_count,
           (unsigned)sum_burst);
}

/* Satiri kodlar, 64 bayt + LF bicimini dogrular ve bosluksuz metni dondurur. */
static size_t encode_text(const tx_message_t *m, char *text)
{
    uint8_t buf[PROTOCOL_FRAME_SIZE];
    memset(buf, 0xEE, sizeof(buf));
    const size_t n = protocol_encode(m, 0U, buf);
    text[0] = '\0';
    if (n != PROTOCOL_FRAME_SIZE)
    {
        return 0;
    }
    CHECK(buf[PROTOCOL_LINE_MAX] == '\n', "64. bayt LF");
    size_t len = 0;
    while (len < PROTOCOL_LINE_MAX && buf[len] != ' ')
    {
        len++;
    }
    for (size_t i = len; i < PROTOCOL_LINE_MAX; i++)
    {
        CHECK(buf[i] == ' ', "metinden sonra yalnizca bosluk");
    }
    memcpy(text, buf, len);
    text[len] = '\0';
    return len;
}

static void expect_line(const tx_message_t *m, const char *want)
{
    char text[PROTOCOL_FRAME_SIZE];
    const size_t len = encode_text(m, text);
    if (strcmp(text, want) != 0)
    {
        printf("  HATA: beklenen \"%s\", gelen \"%s\" (%u bayt)\n", want, text, (unsigned)len);
        g_fail++;
    }
}

static void test_lines(void)
{
    printf("Hafta 3 satirlari\n");

    tx_message_t m = { .type = MSG_QRUN };
    m.u.qrun.run = 1;
    m.u.qrun.variant = 'A';
    m.u.qrun.capacity = 4;
    m.u.qrun.wait_ticks = 0;
    m.u.qrun.service_ms = QEXP_SERVICE_MS;
    m.u.qrun.gap_ms = QEXP_GAP_MS;
    m.u.qrun.trace = true;
    expect_line(&m, "QRUN,1,A,normal,4,0,20,100,T");
    m.u.qrun.run = 65535;
    m.u.qrun.variant = 'C';
    m.u.qrun.burst = true;
    m.u.qrun.wait_ticks = 20;
    m.u.qrun.trace = false;
    expect_line(&m, "QRUN,65535,C,yogun,4,20,20,100,-");

    m = (tx_message_t){ .type = MSG_QBUSY };
    m.u.qrun.run = 7;
    expect_line(&m, "QBUSY,7");

    /* En uzun QMSG: tam 63 bayt, kesilmeden gonderilmeli. */
    m = (tx_message_t){ .type = MSG_QMSG };
    m.u.qmsg.run = 65535;
    m.u.qmsg.seq = 44;
    m.u.qmsg.sent_ok = true;
    m.u.qmsg.received = true;
    m.u.qmsg.depth = 16;
    m.u.qmsg.t_us = UINT32_MAX;
    m.u.qmsg.send_us = UINT32_MAX;
    m.u.qmsg.g2r_us = UINT32_MAX;
    m.u.qmsg.svc_us = UINT32_MAX;
    char text[PROTOCOL_FRAME_SIZE];
    CHECK(encode_text(&m, text) == PROTOCOL_LINE_MAX, "en uzun QMSG tam 63 bayt");

    m.u.qmsg.run = 3;
    m.u.qmsg.seq = 12;
    m.u.qmsg.depth = 4;
    m.u.qmsg.t_us = 1100250;
    m.u.qmsg.send_us = 17;
    m.u.qmsg.g2r_us = 60310;
    m.u.qmsg.svc_us = 19950;
    expect_line(&m, "QMSG,3,12,ok,1100250,17,60310,19950,4");

    /* Basarisiz gonderim: alinmadi, gecikme/servis/doluluk bos (0 degil). */
    m.u.qmsg.sent_ok = false;
    m.u.qmsg.received = false;
    m.u.qmsg.send_us = 20004;
    expect_line(&m, "QMSG,3,12,fail,1100250,20004,,,");

    /* Kabul edilip alinmamis (olmamali): doluluk var, gecikme bos. */
    m.u.qmsg.sent_ok = true;
    expect_line(&m, "QMSG,3,12,ok,1100250,20004,,,4");

    static const char *const names[QCNT_COUNT] = {
        "attempted", "accepted", "failed", "consumed", "queue_max", "rx_idle_timeout",
        "order_error", "run_us", "stack_hwm_producer", "stack_hwm_consumer",
        /* Arda */
        "frames", "frame_miss", "frame_dt_min_us", "frame_dt_max_us", "frame_work_max_us",
        "intake_max_us", "inbox_max", "pool_empty", "pool_alloc_error", "pool_free_error", "pool_leak",
        "dump_retry", "deadline_miss", "status_writes", "status_reads", "status_stale", "status_missed",
        "status_age_max_us", "status_backlog_max", "couriers", "courier_busy_max", "batch", "batch_max",
        "frame_phase_us", "frame_phase_min_us", "frame_phase_max_us",
    };
    CHECK(QCNT_BASE_COUNT == 10, "her kosuda 10 sayac, Arda'da 26 tane daha");
    for (int id = 0; id < (int)QCNT_COUNT; id++)
    {
        m = (tx_message_t){ .type = MSG_QCNT };
        m.u.qcnt.run = 65535;
        m.u.qcnt.id = (qcnt_id_t)id;
        m.u.qcnt.value = UINT32_MAX;
        char want[PROTOCOL_FRAME_SIZE];
        snprintf(want, sizeof(want), "QCNT,65535,%s,4294967295", names[id]);
        expect_line(&m, want);
    }

    m = (tx_message_t){ .type = MSG_QEND };
    m.u.qend.run = 65535;
    m.u.qend.records = 44;
    expect_line(&m, "QEND,65535,44");

    /* Ic komut hatta cikmaz. */
    m = (tx_message_t){ .type = MSG_CMD_QDUMP };
    uint8_t buf[PROTOCOL_FRAME_SIZE];
    CHECK(protocol_encode(&m, 0U, buf) == 0U, "MSG_CMD_QDUMP kodlanmamali");
}

int main(void)
{
    test_profile();
    test_lines();
    printf("\n%s (%d hata)\n", g_fail == 0 ? "TUM TESTLER GECTI" : "TESTLER BASARISIZ", g_fail);
    return g_fail == 0 ? 0 : 1;
}
