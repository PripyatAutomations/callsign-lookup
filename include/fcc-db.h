//      This is part of rustyrig-fw. https://github.com/pripyatautomations/rustyrig-fw
//
// Do not pay money for this, except donations to the project, if you wish to.
// The software is not for sale. It is freely available, always.
//
// Licensed under MIT license, if built without mongoose or GPL if built with.
#include <librustyaxe/core.h>
#include "ft8goblin_types.h"

#if     !defined(_fcc_db_h)
#define _fcc_db_h

#ifdef __cplusplus
extern "C" {
#endif

extern calldata_t *uls_lookup_callsign(const char *callsign);
#ifdef __cplusplus
};
#endif

#endif // !defined(_fcc_db_h)
