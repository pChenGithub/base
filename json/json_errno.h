/**
 * JSON 操作自定义错误码
*/
#ifndef __JSON_ERRNO_H__
#define __JSON_ERRNO_H__
#define ERR_JSON_CHECKPARAM     1   // 参数检查失败
#define ERR_JSON_HASNO_FIELD    2   // 没有指定字段
#define ERR_JSON_DATA_TYPE      3   // 数据类型错误
#define ERR_JSON_DATA_NUM       4   // 数据数字格式检查异常
#define ERR_JSON_BUFF_LEN       5   // 给的buff长度不足
#define ERR_JSON_ARRAY_ITEM     6   // 数组元素检查异常
#endif

