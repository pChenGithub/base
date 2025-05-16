#ifndef __CJSON_TOOLS_H__
#define __CJSON_TOOLS_H__
#ifdef __cplusplus
extern "C" {
#endif
#include "cJSON.h"
#include "json_errno.h"
// 获取json字段内容
int getfield_strPoint(cJSON* obj, const char* label, char** pstr);
int getfield_str(cJSON* obj, const char* label, char* buff, unsigned int bufflen);
int getfield_int(cJSON* obj, const char* label, int* retval);
int getfield_float(cJSON* obj, const char* label, float* retval);
int getfield_double(cJSON* obj, const char* label, double* retval);
#ifdef __cplusplus
}
#endif
#endif

