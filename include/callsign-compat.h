#ifndef CALLSIGN_COMPAT_H
#define CALLSIGN_COMPAT_H

/* Compatibility glue for the old standalone callsign service.  libied was
 * retired; configuration and logging now come from librustyaxe. */
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <librustyaxe/config.h>
#include <librustyaxe/logger.h>

typedef struct callsign_database {
   struct { sqlite3 *sqlite3; } hndl;
} Database;

static inline Database *sql_open(const char *name) {
   if (!name) return NULL;
   if (!strncmp(name, "sqlite3:", 8)) name += 8;
   Database *db = calloc(1, sizeof(*db));
   if (!db || sqlite3_open(name, &db->hndl.sqlite3) != SQLITE_OK) {
      if (db) { sqlite3_close(db->hndl.sqlite3); free(db); }
      return NULL;
   }
   return db;
}

static inline void sql_close(Database *db) {
   if (!db) return;
   sqlite3_close(db->hndl.sqlite3);
   free(db);
}

#define LOG_WARNING LOG_WARN
#define LOG_NOTICE LOG_INFO
#define mainlog NULL
#define log_send(_log, priority, fmt, ...) Log(priority, "callsign", fmt, ##__VA_ARGS__)

static inline time_t callsign_timestr2time_t(const char *s) {
   if (!s || !*s) return 0;
   char *end = NULL;
   long n = strtol(s, &end, 10);
   if (!end || end == s) return 0;
   if (*end == 'd' || *end == 'D') n *= 86400;
   else if (*end == 'h' || *end == 'H') n *= 3600;
   else if (*end == 'm' || *end == 'M') n *= 60;
   return (time_t)n;
}
#define timestr2time_t callsign_timestr2time_t

#endif
