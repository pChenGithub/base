#include "cJSON_tools.h"
#include "string_opt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// 获取字段内容
int getfield_str(cJSON *obj, const char *label, char *buff, unsigned int bufflen) {
    if (NULL==obj||NULL==label)
        return -ERR_JSON_CHECKPARAM;
    if (NULL==buff||0==bufflen)
        return -ERR_JSON_CHECKPARAM;
    // 获取字段
    cJSON* item = cJSON_GetObjectItem(obj, label);
    if (NULL==item)
        return -ERR_JSON_HASNO_FIELD;
    memset(buff, 0, bufflen);
    switch (item->type) {
    case cJSON_String:
        if (strlen(item->valuestring)>=bufflen)
            return -ERR_JSON_BUFF_LEN;
        strCopyC(buff, bufflen, item->valuestring);
        break;
    case cJSON_Number:
        snprintf(buff, bufflen, "%d", item->valueint);
        break;
    default:
        return -ERR_JSON_DATA_TYPE;
        break;
    }
    return 0;
}

int getfield_int(cJSON *obj, const char *label, int *retval)
{
    int ret = 0;
    if (NULL==obj||NULL==label||NULL==retval)
        return -ERR_JSON_CHECKPARAM;
    // 获取字段
    cJSON* item = cJSON_GetObjectItem(obj, label);
    if (NULL==item)
        return -ERR_JSON_HASNO_FIELD;
    switch (item->type) {
    case cJSON_String:
        ret = atoi(item->valuestring);
        if (0==ret&&0!=strncmp(item->valuestring, "0", 2))
            return -ERR_JSON_DATA_NUM;
        *retval = ret;
        break;
    case cJSON_Number:
        *retval = item->valueint;
        break;
    default:
        return -ERR_JSON_DATA_TYPE;
        break;
    }
    return 0;
}

int getfield_strPoint(cJSON *obj, const char *label, char **pstr)
{
    if (NULL==obj||NULL==label||NULL==pstr)
        return -ERR_JSON_CHECKPARAM;
    // 获取字段
    cJSON* item = cJSON_GetObjectItem(obj, label);
    if (NULL==item)
        return -ERR_JSON_HASNO_FIELD;
    switch (item->type) {
    case cJSON_String:
        *pstr = item->valuestring;
        break;
    default:
        return -ERR_JSON_DATA_TYPE;
        break;
    }
    return 0;
}

int getfield_float(cJSON *obj, const char *label, float *retval)
{
    float val = 0;
    int ret = 0;
    if (NULL==obj||NULL==label||NULL==retval)
        return -ERR_JSON_CHECKPARAM;
    // 获取字段
    cJSON* item = cJSON_GetObjectItem(obj, label);
    if (NULL==item)
        return -ERR_JSON_HASNO_FIELD;
    switch (item->type) {
    case cJSON_String:
        val = atof(item->valuestring);
        if (0==val&&0!=strncmp(item->valuestring, "0", 2))
            return -ERR_JSON_DATA_NUM;
        *retval = val;
        break;
    case cJSON_Number:
        *retval = item->valuedouble;
        printf("%f", item->valueint);
        printf("%f", item->valuedouble);
        break;
    default:
        return -ERR_JSON_DATA_TYPE;
        break;
    }
    return 0;
}
