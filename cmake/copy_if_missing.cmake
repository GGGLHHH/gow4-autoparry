# 目标文件不存在才拷。用在 ini 上：输出目录里的 ini 是用户手改 / 修改器写过的，重新编译不能把它盖回默认值。
# 用法：cmake -DSRC=<源文件> -DDST=<目标文件> -P copy_if_missing.cmake
if(NOT EXISTS "${DST}")
    configure_file("${SRC}" "${DST}" COPYONLY)
endif()
