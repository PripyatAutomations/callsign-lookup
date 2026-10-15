//      This is part of rustyrig-fw. https://github.com/pripyatautomations/rustyrig-fw
//
// Do not pay money for this, except donations to the project, if you wish to.
// The software is not for sale. It is freely available, always.
//
// Licensed under MIT license, if built without mongoose or GPL if built with.
#if     !defined(_gnis_lookup_h)
#define _gnis_lookup_h

#ifdef __cplusplus
extern "C" {
#endif
extern bool gnis_initialized;
extern bool use_gnis;
extern int gnis_init(void);
#ifdef __cplusplus
};
#endif

#endif // !defined(_gnis_lookup_h)
