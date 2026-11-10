/* ps3_log.h -- logging a archivo, la unica fuente de diagnostico en PS3.
 *
 * mk64-ps3 -- port de Mario Kart 64 (decomp n64decomp/mk64) a PS3 homebrew.
 */

#ifndef PS3_LOG_H
#define PS3_LOG_H

/* Ruta del log. USRDIR siempre es escribible; /dev_hdd0/data/ se prueba aparte. */
#define PS3_USRDIR      "/dev_hdd0/game/MK64PS300/USRDIR"

void ps3_log_init(void);
void ps3_log(const char *msg);
void ps3_logf(const char *fmt, ...);
void ps3_log_shutdown(void);

#endif /* PS3_LOG_H */
