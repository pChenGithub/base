#ifndef __RAND_OPS_H__
#define __RAND_OPS_H__
#ifdef __cplusplus
extern "C" {
#endif

// 获取一个随机数，返回int型
int rand_get(unsigned int seed, unsigned int num);
// 获取一个随机数，返回long型
long random_get();
// 获取一个0～3600的随机数
int rand_get_0to3600(unsigned int seed, unsigned int num);
#ifdef __cplusplus
}
#endif
#endif


