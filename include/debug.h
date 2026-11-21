#ifndef GW_DEBUG_H
#define GW_DEBUG_H
#ifdef GW_NATIVE_RUNTIME
/* AOT source stepping requires instrumentation and source maps. */
#define gw_debug_stop() ((void)0)
#else
void gw_debug_open(const char *script, const char *output);
void gw_debug_stop(void);
#endif
#endif
