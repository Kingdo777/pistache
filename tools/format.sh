#!/bin/sh

set -eu

# 通过find_files罗列出所有的工程文件
find_files() {
    git ls-files --cached --exclude-standard --others | grep -E '\.(cc|cpp|h)$'
}
# xargs将输出标准化成参数形式（去掉换行等）传递给clang-format
# clang-format按标准修改代码的格式，-i表示在源码上修改
find_files | xargs clang-format -i
