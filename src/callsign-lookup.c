/*
 * Support for looking up callsigns using FCC ULS (local) or QRZ XML API (paid)
 *
 * Here we lookup callsigns using FCC ULS and QRZ XML API to fill our cache
 */

// XXX: Here we need to try looking things up in the following order:
//	Cache
//	FCC ULS Database
//	QRZ XML API
//
// We then need to save it to the cache (if it didn't come from there already)
// XXX: We need to make this capable of talking on stdio or via a socket
#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <ev.h>
#include "debuglog.h"
#include "daemon.h"
#include "qrz-xml.h"
#include "ft8goblin_types.h"
#include "gnis-lookup.h"
#include "fcc-db.h"
#include "qrz-xml.h"
#include "sql.h"
#include "maidenhead.h"
#include "util.h"
#define	PROTO_VER	1

// Local types.. Gross!
#define BUFFER_SIZE 1024
typedef struct {
   char		buffer[BUFFER_SIZE];
   size_t 	length;
   int		fd;
} InputBuffer;

struct Config Config = {
  .cache_default_expiry = 86400 * 3,	// 3 days
  .offline = true,
  .use_cache = true
};

// globals.. yuck ;)
static const char *callsign_cache_db = NULL;
static bool callsign_keep_stale_offline = false, qrz_active = false;
static Database *calldata_cache = NULL, *calldata_uls = NULL;
static int callsign_max_requests = 0, callsign_ttl_requests = 0;
static const char *my_grid = NULL;
static Coordinates my_coords = { 0, 0 };
static sqlite3_stmt *cache_insert_stmt = NULL;
static sqlite3_stmt *cache_select_stmt = NULL;
static sqlite3_stmt *cache_expire_stmt = NULL;

// common shared things for our library
const char *progname = "callsign-lookup";
bool dying = 0;
time_t now = -1;

static void sql_fini(void) {
   if (cache_insert_stmt != NULL) {
      sqlite3_finalize(cache_insert_stmt);
   }

   if (cache_select_stmt != NULL) {
      sqlite3_finalize(cache_select_stmt);
   }

   if (cache_expire_stmt != NULL) {
      sqlite3_finalize(cache_expire_stmt);
   }

   if (calldata_cache != NULL) {
      sql_close(calldata_cache);
      calldata_cache = NULL;
   }

   if (calldata_uls != NULL) {
      sql_close(calldata_uls);
      calldata_uls = NULL;
   }

   exit(0);
}

char expiry_sql[256];
void run_sql_expire(void) {
   int rc = 0;

   if (cache_expire_stmt == NULL) {
      memset(expiry_sql, 0, 256);
      snprintf(expiry_sql, 256, "DELETE FROM cache WHERE cache_expires <= %lu", now);
      rc = sqlite3_prepare_v2(calldata_cache->hndl.sqlite3, expiry_sql , -1, &cache_expire_stmt, 0);

      if (rc != SQLITE_OK) {
         sqlite3_reset(cache_expire_stmt);
         log_send(mainlog, LOG_WARNING, "Error preparing statement for cache expiry: %s\n", sqlite3_errmsg(calldata_cache->hndl.sqlite3));
      }
   } else {
      sqlite3_reset(cache_expire_stmt);
      sqlite3_clear_bindings(cache_expire_stmt);
   }
   int changes = sqlite3_changes(calldata_cache->hndl.sqlite3);
   log_send(mainlog, LOG_DEBUG, "cache expiry done: %d changes!", changes);
}

// Load the configuration (cfg_get_str(...)) into *our* configuration locals
static void callsign_lookup_setup(void) {
   Config.initialized = true;

   // Use local ULS database?
   const char *s = cfg_get_str(cfg, "callsign-lookup/use-uls");

   if (strncasecmp(s, "true", 4) == 0) {
      Config.use_uls = true;
   } else {
      Config.use_uls = false;
   }

   // use QRZ XML API?
   s = cfg_get_str(cfg, "callsign-lookup/use-qrz");

   if (strncasecmp(s, "true", 4) == 0) {
      Config.use_qrz = true;
   } else {
      Config.use_qrz = false;
   }

   // use QRZ XML API?
   s = cfg_get_str(cfg, "callsign-lookup/use-qrz");

   if (strncasecmp(s, "true", 4) == 0) {
      Config.use_qrz = true;
   } else {
      Config.use_qrz = false;
   }

   // Use local cache db?
   s = cfg_get_str(cfg, "callsign-lookup/use-cache");

   if (strncasecmp(s, "true", 4) == 0) {
      Config.use_cache = true;
   } else {
      Config.use_cache = false;
   }

   if (Config.use_cache) {
      // is cache database configured?
      s = cfg_get_str(cfg, "callsign-lookup/cache-db");
      if (s == NULL) {
         log_send(mainlog, LOG_CRIT, "callsign_lookup_setup: Failed to find cache-db in config! Disabling cache...");
      } else {
         callsign_cache_db = s;
         Config.cache_default_expiry = timestr2time_t(cfg_get_str(cfg, "callsign-lookup/cache-expiry"));
         log_send(mainlog, LOG_DEBUG, "setting default callsign cache expiry to %lu seconds (from config)", Config.cache_default_expiry);

         // minimum 1 hour cache lifetime
         if (Config.cache_default_expiry < 3600) {
            log_send(mainlog, LOG_WARNING, "callsign-lookup/cache-expiry %lu is too low, defaulting to 1 hour. If you wish to disable caching, set callsign-lookup/use-cache to false instead.", Config.cache_default_expiry);
            Config.cache_default_expiry = 3600;
         }
         callsign_keep_stale_offline = str2bool(cfg_get_str(cfg, "callsign-lookup/cache-keep-stale-if-offline"), true);

         if ((calldata_cache = sql_open(callsign_cache_db)) == NULL) {
            log_send(mainlog, LOG_CRIT, "callsign_lookup_setup: failed opening cache %s! Disabling caching!", callsign_cache_db);
            Config.use_cache = false;
            calldata_cache = NULL;
         } else {
            // cache database was succesfully opened
            // XXX: Detect if we need to initialize it -- does table cache exist?
            // XXX: Initialize the tables using sql in sql/cache.sql
            log_send(mainlog, LOG_INFO, "calldata cache database opened");
         }
      }
   }

   // after X requests, should we exit with 0 status and restart?
   callsign_max_requests = cfg_get_int(cfg, "callsign-lookup/respawn-after-requests");

   // if invalid value, disable this feature
   if (callsign_max_requests < 0) {
      callsign_max_requests = 0;
   }
}
   
// save a callsign record to the cache
bool callsign_cache_save(calldata_t *cp) {
   // Here we insert into the SQL table, for now only sqlite3 is supported but this shouldnt be the case...
   int rc = 0;

   if (cp == NULL) {
      log_send(mainlog, LOG_DEBUG, "callsign_cache_save: called with NULL calldata pointer...");
      return false;
   }

   // if caching is disabled, act like it successfully saved
   if (Config.use_cache == false) {
      return true;
   }

   // did someone forget to call callsign_lookup_setup() or edit config.json properly?? hmm...
   if (calldata_cache == NULL) {
      log_send(mainlog, LOG_DEBUG, "callsign_cache_save: called but calldata_cache == NULL...");
      return false;
   }

   // dont cache ULS data as it's already in a database...
   if (cp->origin == DATASRC_ULS) {
      return false;
   }

   // initialize or reset prepared statement as needed
   if (cache_insert_stmt == NULL) {
      const char *sql = "INSERT INTO cache "
         "(callsign, dxcc, aliases, first_name, last_name, addr1, addr2,"
         "state, zip, grid, country, latitude, longitude, county, class,"
         "codes, email, u_views, effective, expires, cache_expires,"
         "cache_fetched) VALUES"
         "( UPPER(@CALL), @DXCC, @ALIAS, @FNAME, @LNAME, @ADDRA, @ADDRB, "
         "@STATE, @ZIP, @GRID, @COUNTRY, @LAT, @LON, @COUNTY, @CLASS, @CODE, @EMAIL, @VIEWS, @EFF, @EXP, @CEXP, @CFETCH);";

      rc = sqlite3_prepare_v2(calldata_cache->hndl.sqlite3, sql , -1, &cache_insert_stmt, 0);

      if (rc != SQLITE_OK) {
         sqlite3_reset(cache_insert_stmt);
         log_send(mainlog, LOG_WARNING, "Error preparing statement for cache insert of record for %s: %s\n", cp->callsign, sqlite3_errmsg(calldata_cache->hndl.sqlite3));
      }
   } else {
      sqlite3_reset(cache_insert_stmt);
      sqlite3_clear_bindings(cache_insert_stmt);
   }

   cp->cache_expiry = now + Config.cache_default_expiry;
   cp->cache_fetched = now;

   // bind variables
   int idx_callsign = sqlite3_bind_parameter_index(cache_insert_stmt, "@CALL");
   int idx_dxcc = sqlite3_bind_parameter_index(cache_insert_stmt, "@DXCC");
   int idx_aliases = sqlite3_bind_parameter_index(cache_insert_stmt, "@ALIAS");
   int idx_fname = sqlite3_bind_parameter_index(cache_insert_stmt, "@FNAME");
   int idx_lname = sqlite3_bind_parameter_index(cache_insert_stmt, "@LNAME");
   int idx_addr1 = sqlite3_bind_parameter_index(cache_insert_stmt, "@ADDRA");
   int idx_addr2 = sqlite3_bind_parameter_index(cache_insert_stmt, "@ADDRB");
   int idx_state = sqlite3_bind_parameter_index(cache_insert_stmt, "@STATE");
   int idx_zip = sqlite3_bind_parameter_index(cache_insert_stmt, "@ZIP");
   int idx_grid = sqlite3_bind_parameter_index(cache_insert_stmt, "@GRID");
   int idx_country = sqlite3_bind_parameter_index(cache_insert_stmt, "@COUNTRY");
   int idx_latitude = sqlite3_bind_parameter_index(cache_insert_stmt, "@LAT");
   int idx_longitude = sqlite3_bind_parameter_index(cache_insert_stmt, "@LON");
   int idx_county = sqlite3_bind_parameter_index(cache_insert_stmt, "@COUNTY");
   int idx_class = sqlite3_bind_parameter_index(cache_insert_stmt, "@CLASS");
   int idx_codes = sqlite3_bind_parameter_index(cache_insert_stmt, "@CODES");
   int idx_email = sqlite3_bind_parameter_index(cache_insert_stmt, "@EMAIL");
   int idx_views = sqlite3_bind_parameter_index(cache_insert_stmt, "@VIEWS");
   int idx_eff = sqlite3_bind_parameter_index(cache_insert_stmt, "@EFF");
   int idx_exp = sqlite3_bind_parameter_index(cache_insert_stmt, "@EXP");
   int idx_cache_expiry = sqlite3_bind_parameter_index(cache_insert_stmt, "@CEXP");
   int idx_cache_fetched = sqlite3_bind_parameter_index(cache_insert_stmt, "@CFETCH");

   sqlite3_bind_text(cache_insert_stmt, idx_callsign, cp->callsign, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(cache_insert_stmt, idx_dxcc, cp->dxcc);
   sqlite3_bind_text(cache_insert_stmt, idx_aliases, cp->aliases, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_fname, cp->first_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_lname, cp->last_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_addr1, cp->address1, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_addr2, cp->address2, -1, SQLITE_TRANSIENT); 
   sqlite3_bind_text(cache_insert_stmt, idx_state, cp->state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_zip, cp->zip, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_grid, cp->grid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_country, cp->country, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(cache_insert_stmt, idx_latitude, cp->latitude);
   sqlite3_bind_double(cache_insert_stmt, idx_longitude, cp->longitude);
   sqlite3_bind_text(cache_insert_stmt, idx_county, cp->county, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_class, cp->opclass, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_codes, cp->codes, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(cache_insert_stmt, idx_email, cp->email, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(cache_insert_stmt, idx_views, cp->qrz_views);
   sqlite3_bind_int64(cache_insert_stmt, idx_eff, cp->license_effective);
   sqlite3_bind_int64(cache_insert_stmt, idx_exp, cp->license_expiry);
   sqlite3_bind_int64(cache_insert_stmt, idx_cache_expiry, cp->cache_expiry);
   sqlite3_bind_int64(cache_insert_stmt, idx_cache_fetched, cp->cache_fetched);

   // execute the query
   rc = sqlite3_step(cache_insert_stmt);
   
   if (rc == SQLITE_OK) {
      // nothing to do here...
      return true;
//   } else {
      log_send(mainlog, LOG_DEBUG, "calldata_cache_save: rc: %d", rc);
//      log_send(mainlog, LOG_WARNING, "inserting %s into calldata cache failed: %s", cp->callsign, sqlite3_errmsg(calldata_cache->hndl.sqlite3));
   }

   return false;
}

calldata_t *callsign_cache_find(const char *callsign) {
   calldata_t *cd = NULL;
   int rc = -1;
   int idx_callsign = 0, idx_dxcc = 0, idx_aliases = 0, idx_fname = 0, idx_lname = 0, idx_addr1 = 0;
   int idx_addr2 = 0, idx_state = 0, idx_zip = 0, idx_grid = 0, idx_country = 0, idx_latitude = 0;
   int idx_longitude = 0, idx_county = 0, idx_class = 0, idx_codes = 0, idx_email = 0, idx_views = 0;
   int idx_effective = 0, idx_expiry = 0, idx_cache_expiry = 0, idx_cache_fetched = 0;

   // if no callsign given, bail
   if (callsign == NULL) {
      log_send(mainlog, LOG_CRIT, "callsign_cache_find: callsign == NULL");
      return NULL;
      }

   // try to allocate memory for the calldata_t structure
   if ((cd = malloc(sizeof(calldata_t))) == NULL) {
      fprintf(stderr, "+ERROR callsign_cache_find: out of memory!\n");
      exit(ENOMEM);
   }
   memset(cd, 0, sizeof(calldata_t));

   // prepare the statement if it's not been done yet
   if (cache_select_stmt == NULL) {
      char *sql = "SELECT * FROM cache WHERE callsign = UPPER(@CALL);";
      rc = sqlite3_prepare(calldata_cache->hndl.sqlite3, sql, -1, &cache_select_stmt, 0);

      if (rc == SQLITE_OK) {
         rc = sqlite3_bind_text(cache_select_stmt, 1, callsign, -1, SQLITE_TRANSIENT);
         if (rc == SQLITE_OK) {
//            log_send(mainlog, LOG_DEBUG, "prepared cache SELECT statement succesfully");
         } else {
            log_send(mainlog, LOG_WARNING, "sqlite3_bind_text cache select callsign failed: %s", sqlite3_errmsg(calldata_cache->hndl.sqlite3));
            free(cd);
            return NULL;
         }
      } else {
         log_send(mainlog, LOG_WARNING, "Error preparing statement for cache select of record for %s: %s\n", callsign, sqlite3_errmsg(calldata_cache->hndl.sqlite3));
         free(cd);
         return NULL;
      }
   } else {	// reset the statement for reuse
      sqlite3_reset(cache_select_stmt);
      sqlite3_clear_bindings(cache_select_stmt);
      rc = sqlite3_bind_text(cache_select_stmt, 1, callsign, -1, SQLITE_TRANSIENT);
      if (rc != SQLITE_OK) {
         log_send(mainlog, LOG_WARNING, "sqlite3_bind_text reset cache select callsign failed: %s", sqlite3_errmsg(calldata_cache->hndl.sqlite3));
         free(cd);
         return NULL;
      } else {
         log_send(mainlog, LOG_DEBUG, "reset cache SELECT statement succesfully");
      }
   }

   int step = sqlite3_step(cache_select_stmt);
   if (step == SQLITE_ROW || step == SQLITE_DONE) {
      // Find column names, so we can avoid trying to refer to them by number
      int cols = sqlite3_column_count(cache_select_stmt);

      for (int i = 0; i < cols; i++) {
          const char *cname = sqlite3_column_name(cache_select_stmt, i);
          if (strcasecmp(cname, "callsign") == 0) {
             idx_callsign = i;
          } else if (strcasecmp(cname, "dxcc") == 0) {
             idx_dxcc = i;
          } else if (strcasecmp(cname, "aliases") == 0) {
             idx_aliases = i;
          } else if (strcasecmp(cname, "first_name") == 0) {
             idx_fname = i;
          } else if (strcasecmp(cname, "last_name") == 0) {
             idx_lname = i;
          } else if (strcasecmp(cname, "addr1") == 0) {
             idx_addr1 = i;
          } else if (strcasecmp(cname, "addr2") == 0) {
             idx_addr2 = i;
          } else if (strcasecmp(cname, "state") == 0) {
             idx_state = i;
          } else if (strcasecmp(cname, "zip") == 0) {
             idx_zip = i;
          } else if (strcasecmp(cname, "grid") == 0) {
             idx_grid = i;
          } else if (strcasecmp(cname, "country") == 0) {
             idx_country = i;
          } else if (strcasecmp(cname, "latitude") == 0) {
             idx_latitude = i;
          } else if (strcasecmp(cname, "longitude") == 0) {
             idx_longitude = i;
          } else if (strcasecmp(cname, "county") == 0) {
             idx_county = i;
          } else if (strcasecmp(cname, "class") == 0) {
             idx_class = i;
          } else if (strcasecmp(cname, "codes") == 0) {
             idx_codes = i;
          } else if (strcasecmp(cname, "email") == 0) {
             idx_email = i;
          } else if (strcasecmp(cname, "u_views") == 0) {
             idx_views = i;
          } else if (strcasecmp(cname, "effective") == 0) {
             idx_effective = i;
          } else if (strcasecmp(cname, "expires") == 0) {
             idx_expiry = i;
          } else if (strcasecmp(cname, "cache_expires") == 0) {
             idx_cache_expiry = i;
          } else if (strcasecmp(cname, "cache_fetched") == 0) {
             idx_cache_fetched = i;
          } else if (strcasecmp(cname, "cache_id") == 0) {
             // skip
          } else {
             log_send(mainlog, LOG_DEBUG, "Unknown column: %d (%s)", i, cname);
          }
      }

      // Copy the data into the calldata_t
      cd->origin = DATASRC_CACHE;
      cd->cached = true;
      const unsigned char *cs = sqlite3_column_text(cache_select_stmt, idx_callsign);
      if (cs == NULL) {
         free(cd);
         return NULL;
      }
      snprintf(cd->callsign, MAX_CALLSIGN, "%s", cs);
      snprintf(cd->aliases, MAX_QRZ_ALIASES, "%s", sqlite3_column_text(cache_select_stmt, idx_aliases));
      snprintf(cd->first_name, MAX_FIRSTNAME, "%s", sqlite3_column_text(cache_select_stmt, idx_fname));
      snprintf(cd->last_name, MAX_LASTNAME, "%s", sqlite3_column_text(cache_select_stmt, idx_lname));
      snprintf(cd->address1, MAX_ADDRESS_LEN, "%s", sqlite3_column_text(cache_select_stmt, idx_addr1));
      snprintf(cd->address2, MAX_ADDRESS_LEN, "%s", sqlite3_column_text(cache_select_stmt, idx_addr2));
      snprintf(cd->state, 3, "%s", sqlite3_column_text(cache_select_stmt, idx_state));
      snprintf(cd->zip, MAX_ZIP_LEN, "%s", sqlite3_column_text(cache_select_stmt, idx_zip));
      snprintf(cd->grid, MAX_GRID_LEN, "%s", sqlite3_column_text(cache_select_stmt, idx_grid));
      snprintf(cd->country, MAX_COUNTRY_LEN, "%s", sqlite3_column_text(cache_select_stmt, idx_country));
      cd->latitude = sqlite3_column_double(cache_select_stmt, idx_latitude);
      cd->longitude = sqlite3_column_double(cache_select_stmt, idx_longitude);
      snprintf(cd->county, MAX_COUNTY, "%s", sqlite3_column_text(cache_select_stmt, idx_county));
      snprintf(cd->opclass, MAX_CLASS_LEN, "%s", sqlite3_column_text(cache_select_stmt, idx_class));
      snprintf(cd->codes, MAX_CLASS_LEN, "%s", sqlite3_column_text(cache_select_stmt, idx_codes));
      snprintf(cd->email, MAX_EMAIL, "%s", sqlite3_column_text(cache_select_stmt, idx_email));
      cd->qrz_views = sqlite3_column_int64(cache_select_stmt, idx_views);
      cd->dxcc = sqlite3_column_int64(cache_select_stmt, idx_dxcc);
      cd->license_effective = sqlite3_column_int64(cache_select_stmt, idx_effective);
      cd->license_expiry = sqlite3_column_int64(cache_select_stmt, idx_expiry);
      cd->cache_expiry = sqlite3_column_int64(cache_select_stmt, idx_cache_expiry);
      cd->cache_fetched = sqlite3_column_int64(cache_select_stmt, idx_cache_fetched);
   } else {
      log_send(mainlog, LOG_DEBUG, "no rows - step: %d", step);
      free(cd);
      return NULL;
   }

   // is it expired?
   if (cd->cache_expiry <= now) {
      // are we offline? are we keeping stale results if offline?
      if (Config.offline) {
         // are we configured to discard even when offline?
         if (!callsign_keep_stale_offline) {
            log_send(mainlog, LOG_WARNING, "cache expiry: record for %s is %lu seconds old (%lu expiry), forcing cache deletion", cd->callsign, (now - cd->cache_fetched), (cd->cache_expiry - cd->cache_fetched));

            // we should run a SQL expiry here to delete stale records
            run_sql_expire();

            // free the data structure before returning, so will look it up
            free(cd);
            cd = NULL;
         } else {	// 
            log_send(mainlog, LOG_WARNING, "returning stale result for %s (%lu old)", cd->callsign, (cd->cache_expiry - now));
         }
      } else {         // we are online, so if it's expired, force a lookup
         free(cd);
         cd = NULL;
      }
   } // expired?
   return cd;
}

calldata_t *callsign_lookup(const char *callsign) {
   bool from_cache = false;
   bool res = false;
   calldata_t *qr = NULL;

   // has callsign_lookup_setup() been called yet?
   if (!Config.initialized) {
      callsign_lookup_setup();
   }

   // If enabled, Look in cache first
   if (Config.use_cache && (qr = callsign_cache_find(callsign)) != NULL) {
      log_send(mainlog, LOG_DEBUG, "got cached calldata for %s", callsign);
      from_cache = true;
   }

   // XXX: If offline, check last Config.online_last_retry and if it's been long
   // XXX: enough, try to reconnect before the QRZ check
   if (Config.offline) {
      if (Config.online_last_retry == 0 || (Config.online_last_retry + Config.online_mode_retry <= now)) {
         if (Config.use_qrz && !qrz_active) {
            res = qrz_start_session();
            qrz_active = true;
            Config.online_last_retry = now;

            // if logging into qrz failed, set offline mode
            if (res == false) {
               log_send(mainlog, LOG_CRIT, "Failed logging into QRZ, setting offline mode!");
               Config.offline = true;
            } else {	// if we logged in, clear offline mode
               Config.offline = false;
            }
         }
      }
   }
   // nope, check QRZ XML API, if the user has an account
   if (!Config.offline && Config.use_qrz && qr == NULL) {
      if ((qr = qrz_lookup_callsign(callsign)) != NULL) {
         log_send(mainlog, LOG_DEBUG, "got qrz calldata for %s", callsign);
      }
   }

   // nope, check FCC ULS next since it's available offline
   if (Config.use_uls && qr == NULL) {
      if ((qr = uls_lookup_callsign(callsign)) != NULL) {
         log_send(mainlog, LOG_DEBUG, "got uls calldata for %s", callsign);
      }
   }

   // no results :(
   if (qr == NULL) {
      log_send(mainlog, LOG_WARNING, "no matches found for callsign %s", callsign);
      return NULL;
   }

   // only save it in cache if it did not come from there already
   if (!from_cache) {
      log_send(mainlog, LOG_DEBUG, "adding new item (%s) to cache", callsign);
      callsign_cache_save(qr);
   }

   // increment total requests counter
   callsign_ttl_requests++;

   // is max_requests set?
   if (callsign_max_requests > 0) {
      // have we met/exceeded it?
      if (callsign_ttl_requests >= callsign_max_requests) {
         log_send(mainlog, LOG_CRIT, "answered %d of %d allowed requests, exiting", callsign_ttl_requests, callsign_max_requests);
         // XXX: Dump CPU and memory statistics to the log, so we can look for leaks and profile
         // XXX: req/sec, etc too
         fini(0);
      }
   }
   return qr;
}

static void exit_fix_config(void) {
   printf("Please edit your config.json and try again!\n");
   exit(255);
}

static const char *origin_name[5] = { "NONE", "ULS", "QRZ", "CACHE", NULL };

static void init_my_coords(void) {
   const char *coords = cfg_get_str(cfg, "site/coordinates");
   my_grid = cfg_get_str(cfg, "site/gridsquare");

   if (coords != NULL) {
      const char *comma = strchr(coords, ',');

      if (comma == NULL) {
         log_send(mainlog, LOG_CRIT, "cfg:site/coordinates is invalid (missing comma)!");
      } else {
         comma++;	// skip the comma

         if (comma == NULL) {		// this is an error
            log_send(mainlog, LOG_CRIT, "cfg:site/coordinates is invalid (no value after comma)!");
         } else  if (*comma == ' ') {	// trim leading white space
             while (*comma == ' ') {
                comma++;
             }
         }
         float lat = atof(coords);	// this stops at the comma after latitude
         float lon = atof(comma);		// this stops at any text after longitude
         my_coords.latitude = lat;
         my_coords.longitude = lon;
      }
   } else {	// site/coordinates overrides calculation from site/gridsquare, unless it isn't set...
      my_coords = maidenhead2latlon(my_grid);
   }
   log_send(mainlog, LOG_DEBUG, "configured mygrid: %s, lat: %f, lon: %f", my_grid, my_coords.latitude, my_coords.longitude);
}

// dump all the set attributes of a calldata to the screen
bool calldata_dump(calldata_t *calldata, const char *callsign) {
   if (calldata == NULL) {
      return false;
   }

   const char *online = (Config.offline ? "OFFLINE" : "ONLINE");

   if (calldata->callsign[0] == '\0') {
      if (calldata->query_callsign[0] != '\0') {
         fprintf(stdout, "404 NOT FOUND %s %s %lu\n", calldata->query_callsign, online, now);
         log_send(mainlog, LOG_DEBUG, "Lookup for %s failed, not found!\n", calldata->query_callsign);
      } else if (callsign != NULL) {
         fprintf(stdout, "404 NOT FOUND %s %s %lu\n", callsign, online, now);
      } else {
         log_send(mainlog, LOG_DEBUG, "Lookup failed: query_callsign unset!");
         fprintf(stdout, "404 NOT FOUND (unknown) %s %lu\n", online, now);
      }
      return false;
   }

   // 200 OK N0CALL ONLINE 1683541080 QRZ
   // Lookup for N0CALL succesfully performed using QRZ online (not cached)
   // at Mon May  8 06:18:00 AM EDT 2023.
   // 200 OK N0CALL CACHE 1683541080 QRZ EXPIRES 1683800280
   // Lookup for N0CALL was answered by the cache.
   // The answer originally came from QRZ at Mon May  8 06:18:00 AM EDT 2023
   // and will expire (if we go online) at Thu May 11 06:18:00 AM EDT 2023.
   fprintf(stdout, "200 OK %s ONLINE %lu %s\n", calldata->callsign, time(NULL), origin_name[calldata->origin]);
   fprintf(stdout, "Callsign: %s\n", calldata->callsign);

   fprintf(stdout, "Cached: %s\n", (calldata->cached ? "true" : "false"));

   struct tm *cache_fetched_tm;
   struct tm *cache_expiry_tm;

   if (calldata->cached) {
      char fetched[128], expiry[128];
      struct tm *tm_fetched = NULL, *tm_expiry = NULL;

      // zeroize memories
      memset(fetched, 0, 128);
      memset(expiry, 0, 128);

      if ((tm_fetched = localtime(&calldata->cache_fetched)) == NULL) {
         log_send(mainlog, LOG_CRIT, "localtime() failed");
         fprintf(stderr, "+ERROR Internal error: %d:%s.\n", errno, strerror(errno));
         exit(255);
      }

      if (strftime(fetched, 128, "%Y/%m/%d %H:%M:%S", tm_fetched) == 0 && errno != 0) {
         log_send(mainlog, LOG_CRIT, "strftime() failed");
         fprintf(stderr, "+ERROR Internal error: %d:%s.\n", errno, strerror(errno));
         exit(254);
      }

      if ((tm_expiry = localtime(&calldata->cache_expiry)) == NULL) {
         log_send(mainlog, LOG_CRIT, "localtime() failed");
         fprintf(stderr, "+ERROR Internal error: %d:%s.\n", errno, strerror(errno));
         exit(255);
      }

      if (strftime(expiry, 128, "%Y/%m/%d %H:%M:%S", tm_expiry) == 0 && errno != 0) {
         log_send(mainlog, LOG_CRIT, "strftime() failed");
         fprintf(stderr, "+ERROR Internal error: %d:%s.\n", errno, strerror(errno));
         exit(254);
      }

      fprintf(stdout, "Cache-Fetched: %s\n", fetched);
      fprintf(stdout, "Cache-Expiry: %s\n", expiry);
   }

   if (calldata->first_name[0] != '\0') {
      fprintf(stdout, "Name: %s %s\n", calldata->first_name, calldata->last_name);
   }

   char *opclass = NULL;
   if (calldata->opclass[0] != '\0') {
      // Parse out US callsign classes to names
      if (strcasecmp(calldata->country, "United States") == 0) {
         switch(calldata->opclass[0]) {
            case 'N':
               opclass = "Novice";
               break;
            case 'A':
               opclass = "Advanced";
               break;
            case 'T':
               opclass = "Technician";
               break;
            case 'G':
               opclass = "General";
               break;
            case 'E':
               opclass = "Extra";
               break;
            default:
               break;
         }
      } else {
         opclass = calldata->opclass;
      }

      // is it a valid pointer with non-empty content?
      if (opclass != NULL && opclass[0] != '\0') {
         fprintf(stdout, "Class: %s\n", opclass);
      }
   }

   if (calldata->grid[0] != 0) {
      fprintf(stdout, "Grid: %s\n", calldata->grid);
   }

   if (calldata->latitude != 0 && calldata->longitude != 0) {
      fprintf(stdout, "WGS-84: %.3f, %.3f\n", calldata->latitude, calldata->longitude);
   }

   // get distance and bearing
   if (my_grid != NULL) {
      if (my_coords.latitude == 0 && my_coords.longitude == 0) {
         init_my_coords();
      }

      // did QRZ provide lat / lon?
      if (calldata->latitude != 0 && calldata->longitude != 0) {
         double distance = calculateDistance(my_coords.latitude, my_coords.longitude, calldata->latitude, calldata->longitude);
         double bearing = calculateBearing(my_coords.latitude, my_coords.longitude, calldata->latitude, calldata->longitude);

         if (distance > 0 && bearing > 0) {
            float heading_miles = distance * 0.6214;
            fprintf(stdout, "Heading: %.1f mi / %.1f km at %.0f degrees\n", heading_miles, distance, bearing);
         }
      } else {		// nope, convert the grid
         Coordinates call_coord = { 0, 0 };

         if (calldata->grid[0] != '\0') {
            call_coord = maidenhead2latlon(calldata->grid);
            log_send(mainlog, LOG_DEBUG, "call grid: %s => lat/lon: %.4f, %.4f", calldata->grid, call_coord.latitude, call_coord.longitude);
         }

         if (call_coord.latitude == 0 && call_coord.longitude == 0) {
            return false;
         } else {
            double distance = calculateDistance(my_coords.latitude, my_coords.longitude, call_coord.latitude, call_coord.longitude);
            double bearing = calculateBearing(my_coords.latitude, my_coords.longitude, call_coord.latitude, call_coord.longitude);

            if (distance > 0 && bearing > 0) {
               float heading_miles = distance * 0.6214;
               fprintf(stdout, "Heading: %.1f mi / %.1f km at %.0f degrees\n", heading_miles, distance, bearing);
            }
         }
      }
   }

   if (calldata->alias_count > 0 && (calldata->aliases[0] != '\0')) {
      fprintf(stdout, "Aliases: %d: %s\n", calldata->alias_count, calldata->aliases);
   }

   if (calldata->dxcc != 0) {
      fprintf(stdout, "DXCC: %d\n", calldata->dxcc);
   }

   if (calldata->email[0] != '\0') {
      fprintf(stdout, "Email: %s\n", calldata->email);
   }

   if (calldata->address1[0] != '\0') {
      fprintf(stdout, "Address1: %s\n", calldata->address1);
   }

   if (calldata->address_attn[0] != '\0') {
      fprintf(stdout, "Attn: %s\n", calldata->address_attn);
   }

   if (calldata->address2[0] != '\0') {
      fprintf(stdout, "Address2: %s\n", calldata->address2);
   }

   if (calldata->state[0] != '\0') {
      fprintf(stdout, "State: %s\n", calldata->state);
   }

   if (calldata->zip[0] != '\0') {
      fprintf(stdout, "Zip: %s\n", calldata->zip);
   }

   if (calldata->county[0] != '\0') {
      fprintf(stdout, "County: %s\n", calldata->county);
   }

   if (calldata->fips[0] != '\0') {
      fprintf(stdout, "FIPS: %s\n", calldata->fips);
   }

   if (calldata->license_effective > 0) {
      struct tm *eff_tm = localtime(&calldata->license_effective);

      if (eff_tm == NULL) {
         log_send(mainlog, LOG_DEBUG, "calldata_dump: failed converting license_effective to tm");
      } else {
         char eff_buf[129];
         memset(eff_buf, 0, 129);

         size_t eff_ret = -1;
         if ((eff_ret = strftime(eff_buf, 128, "%Y/%m/%d", eff_tm)) == 0) {
            if (errno != 0) {
               log_send(mainlog, LOG_DEBUG, "calldata_dump: strfime license effective failed: %d: %s", errno, strerror(errno));
            }
         } else {
            fprintf(stdout, "License Effective: %s\n", eff_buf);
         }
      }
   } else {
      fprintf(stdout, "License Effective: UNKNOWN\n");
   }

   if (calldata->license_expiry > 0) {
      struct tm *exp_tm = localtime(&calldata->license_expiry);

      if (exp_tm == NULL) {
         log_send(mainlog, LOG_DEBUG, "calldata_dump: failed converting license_expiry to tm");
      } else {
         char exp_buf[129];
         memset(exp_buf, 0, 129);

         size_t ret = -1;
         if ((ret = strftime(exp_buf, 128, "%Y/%m/%d", exp_tm)) == 0) {
            if (errno != 0) {
               log_send(mainlog, LOG_DEBUG, "calldata_dump: strfime license expiry failed: %d: %s", errno, strerror(errno));
            }
         } else {
            fprintf(stdout, "License Expires: %s\n", exp_buf);
         }
      }
   } else {
      fprintf(stdout, "License Expires: UNKNOWN\n");
   }

   if (calldata->country[0] != '\0') {
      fprintf(stdout, "Country: %s (%d)\n", calldata->country, calldata->country_code);
   }

   // end of record marker, optional, don't rely on it's presence!
   fprintf(stdout, "+EOR\n\n");
   return true;
}

// XXX: Create a socket IO type to pass around here
typedef struct sockio {
  char readbuf[16384];
  char writebuf[32768];
} sockio_t;

static bool parse_request(const char *line) {
   if (strlen(line) == 0) {
      return true;
   } else if (strncasecmp(line, "/HELP", 5) == 0) {
      fprintf(stdout, "200 OK\n");
      fprintf(stdout, "*** HELP ***\n");
      // XXX: Implement NOCACHE
      fprintf(stdout, "/CALL <CALLSIGN> [NOCACHE]\tLookup a callsign\n");
      // XXX: Implement optional password
      fprintf(stdout, "/EXIT\t\t\t\tShutdown the service\n");
      fprintf(stdout, "/GOODBYE\t\t\tDisconnect from the service, leaving it running\n");
      fprintf(stdout, "/GRID [GRID|COORD]\t\tGet information about a grid square or lat/lon\n");
      fprintf(stdout, "/HELP\t\t\t\tThis message\n");

      fprintf(stdout, "*** Planned ***\n");
      fprintf(stdout, "/GNIS <GRID|COORDS>\t\tLook up the place name for a grid or WGS-84 coordinate\n");
      fprintf(stdout, "+OK\n\n");
   } else if (strncasecmp(line, "/CALL", 5) == 0) {
      const char *callsign = line + 6;

      calldata_t *calldata = callsign_lookup(callsign);

      const char *online = (Config.offline ? "OFFLINE" : "ONLINE");

      if (calldata == NULL) {
         fprintf(stdout, "404 NOT FOUND %s %s %lu\n", callsign, online, now);
         log_send(mainlog, LOG_NOTICE, "Callsign %s was not found in enabled databases.", callsign);
      } else {
         // Send the result
         calldata_dump(calldata, callsign);
         free(calldata);
         calldata = NULL;
      }
   } else if (strncasecmp(line, "/GNIS", 5) == 0) {
     const char *point = line + 6;

     if (*point == '\0') {
        fprintf(stdout, "You must specify a WGS-84 coordinate or a 4-10 digit grid square.\n");
        return false;
     }
   } else if (strncasecmp(line, "/GRID", 5) == 0) {
     Coordinates coord = { 0, 0 };
     const char *point = line + 6;
     const char *comma = NULL;
     char dupe_point[11];
     const char *their_grid = NULL;

     if (*(line + 6) == '\0') {
        fprintf(stdout, "You must specify a WGS-84 coordinate or a 4-10 digit grid square.\n");
        return false;
     }

     // skip leading whitespace...
     const char *p = point;
     while (p != NULL && (*p == ' ' || *p == '\t')) {
        p++;
     }

     // if no point is passed...
     if (p != NULL) {
        comma = strchr(p, ',');

        if (comma == NULL) {
           // skip leading spaces
           size_t point_len = strlen(p);
           // is it too long?
           if (point_len > 10) {
              fprintf(stderr, "+ERROR Invalid grid square '%s' (over 10 characters)\n", point);
              return false;
           }
           memset(dupe_point, 0, 11);
           memcpy(dupe_point, p, point_len);
          
           // upper case it, for readability
           for (int i = 0; i < point_len; i++) {
              // upper case all letters
              if (!isdigit(dupe_point[i])) {
                 dupe_point[i] = toupper(point[i]);
              } else {
                 dupe_point[i] = p[i];
              }
           }
           coord = maidenhead2latlon(dupe_point);
        } else {
           // skip leading white space
           const char *p = point;
           while (p != NULL && (*p == ' ' || *p == '\t')) {
              p++;
           }

           // Calculate how many digits of precision we can accomodate with the data given
           int lat_digits = 0, lon_digits = 0;
           const char *lat_dot = strchr(p, '.');
           const char *lon_dot = strchr(comma, '.');

           if (lat_dot != NULL && lon_dot != NULL) {
              const char *lon_end = lon_dot + strlen(lon_dot);
              lat_digits = (int)((comma - 1) - (lat_dot + 1));	// get lat length
              comma++;						// skip the comma
              lon_digits = (int)(lon_end - (lon_dot + 1));		// figure out lon length
//              log_send(mainlog, LOG_DEBUG, "precision: lat_digits: %lu, lon_digits: %lu", lat_digits, lon_digits);
           } else {
              fprintf(stdout, "+ERROR: You must specify at least one decimal place for each coordinate\n");
              return false;
           }

           // set the precision of our coordinates
           if (lat_digits >= 3 && lon_digits >= 3) {
              coord.precision = 5;
           } else if (lat_digits >= 2 && lon_digits >= 2) {
              coord.precision = 4;
           } else if (lat_digits >= 1 && lon_digits >= 1) {
              coord.precision = 3;
           } else {
              coord.precision = 2;
           }

           if (comma == NULL) {		// this is an error
              log_send(mainlog, LOG_CRIT, "cfg:site/coordinates is invalid (no value after comma)!");
              return false;
           } else  if (*comma == ' ') {	// trim leading white space on longitude
              while (*comma == ' ') {
                 comma++;
              }
           }

           float lat = atof(p);		// this stops at the comma after latitude
           float lon = atof(comma);	// this stops at any text after longitude
           coord.latitude = lat;
           coord.longitude = lon;
           their_grid = latlon2maidenhead(&coord);
        }
     }

     if (coord.latitude == 0 && coord.longitude == 0) {
        return false;
     }

     if (comma == NULL) {
        fprintf(stdout, "Grid: %s\n", dupe_point);
     } else {
        fprintf(stdout, "Grid: %s\n", their_grid);
     }

     // XXX: this is ugly, can we make it more compact?
//     fprintf(stdout, "WGS-84: %*f, %*f\n", coord.precision, coord.latitude, coord.precision, coord.longitude);
     if (coord.precision >= 5) {
        fprintf(stdout, "WGS-84: %.5f, %.5f\n", coord.latitude, coord.longitude);
     } else if (coord.precision <= 4) {
        fprintf(stdout, "WGS-84: %.4f, %.4f\n", coord.latitude, coord.longitude);
     } else if (coord.precision <= 3) {
        fprintf(stdout, "WGS-84: %.3f, %.3f\n", coord.latitude, coord.longitude);
     } else if (coord.precision <= 2) {
        fprintf(stdout, "WGS-84: %.2f, %.2f\n", coord.latitude, coord.longitude);
     } else if (coord.precision <= 1) {
        fprintf(stdout, "WGS-84: %.1f, %.1f\n", coord.latitude, coord.longitude);
     }

     double distance = calculateDistance(my_coords.latitude, my_coords.longitude, coord.latitude, coord.longitude);
     double bearing = calculateBearing(my_coords.latitude, my_coords.longitude, coord.latitude, coord.longitude);

     float heading_miles = distance * 0.6214;
     fprintf(stdout, "Heading: %.1f mi / %.1f km at %.0f degrees\n", heading_miles, distance, bearing);
     fprintf(stdout, "+EOR\n\n");
   } else if (strncasecmp(line, "/EXIT", 5) == 0) {
      log_send(mainlog, LOG_CRIT, "Got EXIT from client. Goodbye!");
      fprintf(stdout, "+GOODBYE Hope you had a nice session! Exiting.\n");
      fini(0);
   } else if (strncasecmp(line, "/GOODBYE", 8) == 0) {
      log_send(mainlog, LOG_NOTICE, "Got GOODBYE from client. Disconnecting it.");
      fprintf(stdout, "+GOODBYE Hope you had a nice session!\n");
      // XXX: Disconnect client
      // XXX: Free the client
   } else {
      // XXX: Someday we should implement a read-line interface and treat this as a callsign lookup ;)
      fprintf(stdout, "400 Bad Request - Your client sent a request I do not understand... Try /HELP for commands!\n");
   }
   
   return false;
}

static void stdin_cb(EV_P_ ev_io *w, int revents) {
    if (EV_ERROR & revents) {
       fprintf(stderr, "+ERROR Error event in stdin watcher\n");
       return;
    }

    InputBuffer *input = (InputBuffer *)w->data;
    ssize_t bytesRead = read(STDIN_FILENO, input->buffer + input->length, BUFFER_SIZE - input->length);

    if (bytesRead < 0) {
        perror("read");
        return;
    }

    if (bytesRead == 0) {
       // End of file (Ctrl+D pressed)
       ev_io_stop(EV_A, w);
       free(input);
       log_send(mainlog, LOG_CRIT, "got ^D (EOF), exiting!");
       fprintf(stdout, "+GOODBYE Hope you had a nice session! Exiting.\n");
       fini(0);
       return;
    }

    input->length += bytesRead;

    // Process complete lines
    char *newline;
    while ((newline = strchr(input->buffer, '\n')) != NULL) {
       *newline = '\0';  // Replace newline character with null terminator
       parse_request(input->buffer);
       memmove(input->buffer, newline + 1, input->length - (newline - input->buffer));
       input->length -= (newline - input->buffer) + 1;
    }

    // If buffer is full and no newline is found, consider it an incomplete line
    if (input->length == BUFFER_SIZE) {
       fprintf(stdout, "+ERROR Input buffer full, discarding incomplete line: %s\n", input->buffer);
       input->length = 0;  // Discard the incomplete line
    }
}

static void periodic_cb(EV_P_ ev_timer *w, int revents) {
   now = time(NULL);			   // update our shared timestamp

   // every 3 hours, expire old cache data entries
   if ((now % 10800) == 0) {
      run_sql_expire();
   }
}

int main(int argc, char **argv) {
   struct ev_loop *loop = EV_DEFAULT;
   struct ev_io stdin_watcher;
   struct ev_timer periodic_watcher;
   bool res = false;
   InputBuffer *input = NULL;

#if	defined(DEBUG)
   // setup logging for address sanitizers early
   setenv("ASAN_OPTIONS", "log_path=asan.log", 1);
   setenv("UBSAN_OPTIONS", "log_path=ubsan.log", 1);
#endif

   // start our clock, periodic_cb will refresh in once a second
   now = time(NULL);

   // This can't work without a valid configuration...
   if (!(cfg = load_config()))
      exit_fix_config();

   const char *logpath = dict_get(runtime_cfg, "logpath", "file://callsign-lookup.log");
   if (logpath != NULL) {
      mainlog = log_open(logpath);
   } else {
      fprintf(stderr, "+ERROR logpath not found, defaulting to stderr!\n");
      mainlog = log_open("stderr");
   }
   log_send(mainlog, LOG_NOTICE, "%s/%s starting up!", progname, VERSION);

   init_signals();
   // how often should we retry going online?
   Config.online_mode_retry = timestr2time_t(cfg_get_str(cfg, "callsign-lookup/retry-delay"));
   if (Config.online_mode_retry < 30) { // enforce a minimum of 30 seconds between retries
      Config.online_mode_retry = 30;
   }

   // initialize site location data
   init_my_coords();

   // setup stdin
   if ((input = malloc(sizeof(InputBuffer))) == NULL) {
      fprintf(stderr, "out of memory!\n");
      log_send(mainlog, LOG_CRIT, "malloc(InputBuffer): out of memory!");
      exit(ENOMEM);
   }
   memset(input, 0, sizeof(InputBuffer));
   ev_io_init(&stdin_watcher, stdin_cb, STDIN_FILENO, EV_READ);
   stdin_watcher.data = input;
   ev_io_start(loop, &stdin_watcher);

   // start our once a second periodic timer (used for housekeeping)
   ev_timer_init(&periodic_watcher, periodic_cb, 0, 1);
   ev_timer_start(loop, &periodic_watcher);

   // initialize things
   callsign_lookup_setup();


   printf("+NOTICE This server is experimental. Please feel free to suggest improvements or send patches\n");
   printf("+NOTICE Use /HELP to see available commands.\n");
   printf("+PROTO %d mytime=%lu\n", PROTO_VER, now);
   printf("+OK %s/%s ready to answer requests. QRZ: %s%s, ULS: %s, GNIS: %s, Cache: %s\n",
         progname, VERSION,
         (Config.use_qrz ? "On" : "Off"), (Config.offline ? " (offline)" : ""),
         (Config.use_uls ? "On" : "Off"), (use_gnis ? "On" : "Off"),
         (Config.use_cache ? "On" : "Off"));

   // run expires at startup (useful for non-daemon users)
   run_sql_expire();

   // if called with callsign(s) as args, look them up, return the parsed output and exit
   if (argc > 1) {
      for (int i = 1; i <= (argc - 1); i++) {
         char *callsign = argv[i];
         calldata_t *calldata = NULL;

         if (argv[i] != NULL) {
            calldata = callsign_lookup(callsign);
         } else {
            break;
         }

         const char *online = (Config.offline ? "OFFLINE" : "ONLINE");

         if (calldata == NULL) {
            fprintf(stdout, "404 NOT FOUND %s %s %lu\n", callsign, online, now);
            log_send(mainlog, LOG_NOTICE, "Callsign %s was not found in enabled databases (%s).", callsign, online);
         } else {
            calldata_dump(calldata, callsign);
            free(calldata);
            calldata = NULL;
         }
      }
      fprintf(stdout, "+GOODBYE Hope you had a nice session! Exiting.\n");

      dying = true;
   } else {
      log_send(mainlog, LOG_INFO, "%s/%s ready to answer requests. QRZ: %s, ULS: %s, GNIS: %s, Cache: %s", progname, VERSION, (Config.use_qrz ? "On" : "Off"), (Config.use_uls ? "On" : "Off"), (use_gnis ? "On" : "Off"), (Config.use_cache ? "On" : "Off"));
   }

   // run the EV main loop...
   if (!dying) {
      ev_run(loop, 0);
   }

   // Close the database(s)
   sql_fini();

   // XXX: get rid of this in favor of per-socket buffers, if we go that route...
   if (input != NULL) {
      free(input);
      input = NULL;
   }
   return 0;
}
