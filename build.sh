#!/bin/bash

# Build script for EventBot.
# Needs headers and libs for:
#     MariaDB
#     DPP++
#     Curl

# No, this does not need a Makefile or some other convoluted build system!

if [ $1 == 'release' ]; then
	BUILD_MODE="-DRELEASE -O3"
else
	BUILD_MODE="-DDEBUG -g"
fi

# Opts for Howard Hinnant's date/tz library.
LIB_DATE_OPTS="-DINSTALL=/tmp -DHAS_REMOTE_API=1" # -DAUTO_DOWNLOAD=1

# Libraries to link with
LIBS="$(mariadb_config --include --libs) -ldpp -lfmt -lcurl -lpthread"


g++ --verbose -std=c++20 $BUILD_MODE -DEVENTBOT -Wall -Werror -Wpedantic -Wno-unused-function -fno-rtti $LIB_DATE_OPTS -I./src/date ./src/tz.cpp ./src/eventbot.cpp $LIBS -o eventbot
