#include "config.h"
#include "debuglog.h"
#include "util.h"
#include "ft8goblin_types.h"

// toggle an intbool (<= 0 is off, >= 1 is on)
void toggle(int *v) {
   if (*v <= 0)
      *v = 1;
   else if (*v >= 1)
      *v = 0;
}

int is_file(const char *path) {
   struct stat sb;

   if (stat(path, &sb) != 0)
      return 0;

   if (S_ISREG(sb.st_mode)) {
//      log_send(mainlog, LOG_DEBUG, "is_file: %s: true", path);
      return 1;
   }

   return 0;
}

int is_dir(const char *path) {
   struct stat sb;

   if (stat(path, &sb) != 0)
      return 0;

   if (S_ISDIR(sb.st_mode))
      return 1;

   return 0;
}

int is_link(const char *path) {
   struct stat sb;

   if (stat(path, &sb) != 0)
      return 0;

   if (S_ISLNK(sb.st_mode))
      return 1;

   return 0;
}

int is_fifo(const char *path) {
   struct stat sb;

   if (stat(path, &sb) != 0)
      return 0;

   if (S_ISFIFO(sb.st_mode))
      return 1;

   return 0;
}

time_t timestr2time_t(const char *str) {
   time_t seconds = 0;
   char *copy = NULL;

   if (str == NULL) {
      fprintf(stderr, "+ERROR timestr2time_t: passed NULL str\n");
      return 0;
   }
   
   size_t len = strlen(str);
   if ((copy = malloc(len + 1)) == NULL) {
      fprintf(stderr, "+ERROR timestr2time_t: out of memory\n");
      exit(ENOMEM);
   }

   memset(copy, 0, len + 1);
   memcpy(copy, str, len);

   char *multipliers = "ywdhms";
   char *ptr = copy;

   while (*ptr != '\0') {
      // Find the numeric value and extract the unit character
      int value = strtol(ptr, &ptr, 10);
      char unit = *ptr;

      switch (unit) {
         case 'y':
            seconds += value * 365 * 24 * 60 * 60;  // Convert years to seconds (assuming 365 days per year)
            break;
         case 'w':
            seconds += value * 7 * 24 * 60 * 60;  // Convert weeks to seconds
            break;
         case 'd':
            seconds += value * 24 * 60 * 60;  // Convert days to seconds
            break;
         case 'h':
            seconds += value * 60 * 60;  // Convert hours to seconds
            break;
         case 'm':
            seconds += value * 60;  // Convert minutes to seconds
            break;
         case 's':
            seconds += value;  // Add seconds
            break;
      }

      ptr++;  // Move to the next character
   }

   free(copy);  // Free the memory allocated for the copy
   return seconds;
}

bool str2bool(const char *str, bool def) {
   if (strcasecmp(str, "true") == 0 || strcasecmp(str, "on") == 0 || strcasecmp(str, "yes") == 0) {
      return true;
   }

   return def;
}

/////////////////////////////////////////////
// return a static string of the mode name //
/////////////////////////////////////////////
const char *mode_names[] = {
   "OFF",
   "FT8",
   "FT4",
//   "JS8",
//   "PSK11",
//   "ARDOP FEC",
   NULL
};
const char *get_mode_name(tx_mode_t mode) {
   if (mode <= TX_MODE_NONE || mode >= TX_MODE_END) {
      return mode_names[0];
   }
   return mode_names[mode];
}
