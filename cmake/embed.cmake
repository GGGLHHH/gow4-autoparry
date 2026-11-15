# 把一个二进制文件转成 C++ 数组（构建时由 CMakeLists 的 add_custom_command 调用）
#   cmake -DIN=<文件> -DOUT=<生成的 .cpp> -DNAME=<变量名> -P embed.cmake
# 生成：extern const unsigned char NAME[]; extern const unsigned long NAME_size;
file(READ "${IN}" hex HEX)
# 每 32 字节一行，再把每个字节写成 0xNN,
string(REGEX REPLACE "(................................................................)" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
file(WRITE "${OUT}.tmp"
"// 由 cmake/embed.cmake 生成，别手改\n"
"extern const unsigned char ${NAME}[] = {\n${hex}\n};\n"
"extern const unsigned long ${NAME}_size = sizeof(${NAME});\n")
configure_file("${OUT}.tmp" "${OUT}" COPYONLY) # 内容没变就不动，免得无谓重编
file(REMOVE "${OUT}.tmp")
