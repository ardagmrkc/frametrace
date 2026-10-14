#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

/*
 * Portlarin kimlikleri. Her portun tanimi (tur, mesaj boyu, kapasite,
 * tazeleme suresi, depolama) port_config.c'deki tablodadir; yazan ve okuyan
 * gorevler port_bind ile baglanir. Yeni bir veri akisi = buraya bir kimlik,
 * tabloya bir satir.
 */
typedef enum
{
    PORT_ARDA_MSG = 0, /* queuing: Producer -> Arda cerceve gorevi, qexp_message_t */
    PORT_ARDA_STATUS,  /* sampling: Arda cerceve gorevi -> Producer, arda_status_t */
    PORT_COUNT
} port_id_t;

#endif /* PORT_CONFIG_H */
