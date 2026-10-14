#include "port_layer.h"
#include "arda.h"

/*
 * Port tablosu (port_layer.h). Her port icin depolama burada statik ayrilir;
 * boyutlar tablodan gelir ve derleme aninda denetlenir.
 *
 *   Port              Tur       Yazan            Okuyan           Icerik
 *   PORT_ARDA_MSG     queuing   Producer         Arda cercevesi   qexp_message_t, 32 slot
 *   PORT_ARDA_STATUS  sampling  Arda cercevesi   Producer         arda_status_t, tazeleme 2 cerceve
 *
 * Yazan ve okuyan gorevler queue_exp_init'te port_bind ile baglanir.
 */

#define MSG_CAP  ARDA_POOL_LEN
#define MSG_SIZE sizeof(qexp_message_t)
#define ST_SIZE  sizeof(arda_status_t)

_Static_assert(MSG_CAP >= 1U && MSG_CAP < PORT_HANDLE_NONE, "tutamac 1 bayta sigmali, 0xFF ayrilmis");
_Static_assert(MSG_SIZE % 4U == 0U && ST_SIZE % 4U == 0U, "slotlar 4 bayt hizali kalmali");
/* Sampling portta kopya kritik bolumde yapilir: kisa kalmali. */
_Static_assert(ST_SIZE <= 32U, "sampling port mesaji en cok 32 bayt");

static uint32_t s_msg_slots[MSG_CAP * MSG_SIZE / 4U];
static uint8_t s_msg_state[MSG_CAP];
static uint8_t s_msg_free_items[MSG_CAP * sizeof(port_handle_t)];
static uint8_t s_msg_ready_items[MSG_CAP * sizeof(port_handle_t)];
static StaticQueue_t s_msg_free_ctrl;
static StaticQueue_t s_msg_ready_ctrl;

static uint32_t s_status_slot[ST_SIZE / 4U];

const port_def_t g_port_defs[PORT_COUNT] = {
    [PORT_ARDA_MSG] = {
        .name = "ArdaMsg",
        .kind = PORT_QUEUING,
        .msg_size = (uint16_t)MSG_SIZE,
        .capacity = (uint8_t)MSG_CAP,
        .refresh_us = 0U,
        .slots = s_msg_slots,
        .state = s_msg_state,
        .free_items = s_msg_free_items,
        .ready_items = s_msg_ready_items,
        .free_ctrl = &s_msg_free_ctrl,
        .ready_ctrl = &s_msg_ready_ctrl,
    },
    [PORT_ARDA_STATUS] = {
        .name = "ArdaStatus",
        .kind = PORT_SAMPLING,
        .msg_size = (uint16_t)ST_SIZE,
        .capacity = 1U,
        .refresh_us = ARDA_STATUS_REFRESH_US,
        .slots = s_status_slot,
        .state = NULL,
        .free_items = NULL,
        .ready_items = NULL,
        .free_ctrl = NULL,
        .ready_ctrl = NULL,
    },
};
