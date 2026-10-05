/* Logging in the format of rtspcam.py: "2026-10-03 20:58:01,123 rtspcam[kapu] INFO message" */
#ifndef RC_LOG_H
#define RC_LOG_H

void log_setup(const char *camera, int debug);
void log_msg(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern int log_debug_enabled;
extern int log_quiet;        /* suppress everything (tests) */

#define log_debug(...) do { if (log_debug_enabled) log_msg("DEBUG", __VA_ARGS__); } while (0)
#define log_info(...) log_msg("INFO", __VA_ARGS__)
#define log_warn(...) log_msg("WARNING", __VA_ARGS__)
#define log_error(...) log_msg("ERROR", __VA_ARGS__)

#endif
