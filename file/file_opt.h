/**
 * 文件相关操作
*/
#ifndef __FILE_OPT_H__
#define __FILE_OPT_H__
#ifdef __cplusplus
extern "C" {
#endif
/** ************************************/
// 错误码
#define ERR_FILEOPT_CHECKPARAM  1   // 检查参数失败

/** ************************************/
// 文件读
int file_read(const char* file, char* buf, unsigned int buflen);
// 文件写，重新写/追加
int file_write(const char* file, char* buf, unsigned int size);
int file_append(const char* file, char* buf, unsigned int size);
// 文件删除
int file_remove(const char* file);
// 文件拷贝
int file_copy(const char* file_src, const char* file_dist);
// 文件移动
int file_move(const char* file_src, const char* file_dist);
// 获取文件长度
int file_size(const char* file);
// 文件是否存在
int file_exist(const char* file);

/** ************************************/
// 获取文件后缀
int file_ext();
// 获取文件创建时间
int file_ctime();

/** ************************************/
// 创建文件夹
int dir_create(const char* dir);
// 删除文件夹
int dir_remove(const char* dir);
// 文件夹是否存在
int dir_exist(const char* dir);
#ifdef __cplusplus
}
#endif
#endif
