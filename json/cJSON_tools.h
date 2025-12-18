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
//
int getField_arrayInt(cJSON* obj, const char* label, int* array, int size);
//
int getField_obj(cJSON* obj, const char* label, cJSON** jret);
// json转字符串
int json2string(cJSON* root, char* buff, int len);
int json2string_free(cJSON **root, char* buff, int len);
#ifdef __cplusplus
}
#endif
#endif

