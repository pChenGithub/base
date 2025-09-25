#ifndef __DEBUG_OPT_H__
#define __DEBUG_OPT_H__
#ifdef __cplusplus
extern "C" {
#endif

#define DEBUGOPTERR_CHECKPARAM      1
#define DEBUGOPTERR_FOPEN_FAIL      2
//
int debug_get_crash_2_file(const char *file, char flag_append);

#ifdef __cplusplus
}
#endif
#endif
