#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sqlite3.h>
#include <spatialite.h>
#include "ft8goblin_types.h"
#include "gnis-lookup.h"

/*
 * Query the local copy of GNIS database to find a locality near a WGS-84 coordinate
 */
bool gnis_initialized = false;
bool use_gnis = false;
static const char *gnis_db = NULL;
static char *gnis_db_owned = NULL;

int gnis_init(void) {
   use_gnis = cfg_get_bool("gnis-lookup.use-gnis", false);

   gnis_db_owned = cfg_get_path("gnis-lookup.gnis-db");
   if (gnis_db_owned != NULL) {
      gnis_db = gnis_db_owned;
   }

   gnis_initialized = true;

   return 0;
}
