#if	!defined(_config_h)
#define	_config_h
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <yajl/yajl_tree.h>
#include <sys/stat.h>
#include <errno.h>
#include "dict.h"

#define	PATHMAX_JSON	16
///////////////////////
// Some tunable bits //
///////////////////////
#define	MAX_MENULEVEL	8		// how deep can menus go?
#define	MAX_MENUNAME	32		// how long can the menu name be?
#define	MAX_MENUTITLE	128		// how long of a description is allowed?
#define	MAX_MENUITEMS	32		// how many items per menu?

///

#define	MAX_MODES	10
#define	MAX_CALLSIGN		32
#define	MAX_QRZ_ALIASES		10
#define	MAX_FIRSTNAME		65
#define	MAX_LASTNAME		65
#define	MAX_ADDRESS_LEN		128
#define	MAX_ZIP_LEN		12
#define	MAX_COUNTRY_LEN		64
#define	MAX_GRID_LEN		10
#define	MAX_COUNTY		65
#define	MAX_CLASS_LEN		11
#define	MAX_EMAIL		129
#define	MAX_URL			257

#ifdef __cplusplus
extern "C" {
#endif
    // Global configuration pointer
    extern yajl_val cfg;
    extern dict     *runtime_cfg;
    // Parse the configuration and return a yajl tree
    extern yajl_val parse_config(const char *cfgfile);
    extern int free_config(yajl_val node);
    extern yajl_val load_config(void);
    extern int cfg_get_int(yajl_val cfg, const char *path);
    extern const char *cfg_get_str(yajl_val cfg, const char *path);
#ifdef __cplusplus
};
#endif

#endif	// !defined(_config_h)
