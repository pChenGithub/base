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
    //int ret = 0;
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
        //printf("%f", item->valueint);
        //printf("%f", item->valuedouble);
        break;
    default:
        return -ERR_JSON_DATA_TYPE;
        break;
    }
    return 0;
}

int getfield_double(cJSON *obj, const char *label, double *retval)
{
    double val = 0;
    //int ret = 0;
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
        //printf("%f", item->valueint);
        //printf("%f", item->valuedouble);
        break;
    default:
        return -ERR_JSON_DATA_TYPE;
        break;
    }
    return 0;
}

int getField_arrayInt(cJSON *obj, const char *label, int *array, int size)
{
    int ret = 0;
    if (NULL==obj||NULL==label||NULL==array||size<=0)
        return -ERR_JSON_CHECKPARAM;
    // 获取字段
    cJSON* json_array = cJSON_GetObjectItem(obj, label);
    if (NULL==json_array)
        return -ERR_JSON_HASNO_FIELD;
    return getField_arrayInt2(json_array, array, size);
}

int getField_obj(cJSON *obj, const char *label, cJSON **jret)
{
    if (NULL==obj||NULL==label||NULL==jret)
        return -ERR_JSON_CHECKPARAM;
    // 获取字段
    *jret = cJSON_GetObjectItem(obj, label);
    if (NULL==(*jret))
        return -ERR_JSON_HASNO_FIELD;
    return 0;
}

int getField_arrayInt2(cJSON *objarray, int *array, int size)
{
    int ret = 0;
    if (NULL==objarray||NULL==array||size<=0)
        return -ERR_JSON_CHECKPARAM;
    if (cJSON_Array!=objarray->type)
        return -ERR_JSON_DATA_TYPE;
    int count = cJSON_GetArraySize(objarray);
    count = count<size?count:size;  // 取小值
    cJSON* item = NULL;
    for (int i=0;i<count;i++) {
        item = cJSON_GetArrayItem(objarray, i);
        if (NULL==item)
            return -ERR_JSON_ARRAY_ITEM;
        // 按数字读取
        switch (item->type) {
        case cJSON_String:
            ret = atoi(item->valuestring);
            if (0==ret&&0!=strncmp(item->valuestring, "0", 2))
                return -ERR_JSON_DATA_NUM;
            array[i] = ret;
            break;
        case cJSON_Number:
            array[i] = item->valueint;
            break;
        default:
            return -ERR_JSON_DATA_TYPE;
            break;
        }
    }
    return 0;
}

int getField_obj(cJSON *obj, const char *label, cJSON **jret)
{
    if (NULL==obj||NULL==label||NULL==jret)
        return -ERR_JSON_CHECKPARAM;
    // 获取字段
    *jret = cJSON_GetObjectItem(obj, label);
    if (NULL==(*jret))
        return -ERR_JSON_HASNO_FIELD;
    return 0;
}

int json2string(cJSON *root, char *buff, int len)
{
    if (NULL==root||NULL==buff||len<=0)
        return -ERR_JSON_CHECKPARAM;
    char* pstr = cJSON_PrintUnformatted(root);
    if (NULL==pstr)
        return -ERR_JSON_PASER2STRING;
    // 判断长度
    int strlenght = strlen(pstr);
    if (len<(strlenght+1)) {
        free(pstr);pstr = NULL;
        return -ERR_JSON_BUFF_ENOUGH;
    }
    memcpy(buff, pstr, strlenght);
    buff[strlenght] = 0;
    free(pstr);pstr = NULL;
    return 0;
}

int json2string_free(cJSON **root, char *buff, int len)
{
    if (NULL==root||NULL==buff||len<=0)
        return -ERR_JSON_CHECKPARAM;
    if (NULL==(*root))
        return -ERR_JSON_CHECKPARAM;
    char* pstr = cJSON_PrintUnformatted(*root);
    // 释放json
    cJSON_Delete(*root);*root = NULL;
    if (NULL==pstr)
        return -ERR_JSON_PASER2STRING;
    // 判断长度
    int strlenght = strlen(pstr);
    if (len<(strlenght+1)) {
        free(pstr);pstr = NULL;
        return -ERR_JSON_BUFF_ENOUGH;
    }
    //
    memcpy(buff, pstr, strlenght);
    buff[strlenght] = 0;
    free(pstr);pstr = NULL;
    return 0;
}
