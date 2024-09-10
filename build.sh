#!/bin/bash

# Build script for XDHS Bot.
# Needs headers and libs for:
#     MariaDB
#     Curl
#     DPP++
#     libfmt
#     libpoppler-cpp

# No, this does not need a Makefile or some other convoluted build system!

if [[ $1 == 'release' ]]; then
	BUILD_MODE="-DRELEASE -O3"
	BINARY_NAME="xdhs_bot"
else
	BUILD_MODE="-DDEBUG -g -O3"
	BINARY_NAME="xdhs_bot_dev"
fi

# Opts for Howard Hinnant's date/tz library.
LIB_DATE_OPTS="-DINSTALL=/tmp/$BINARY_NAME/ -DHAS_REMOTE_API=1 -DAUTO_DOWNLOAD=0"

# Libraries to link with
LIBS="$(mariadb_config --include --libs) -ldpp -lfmt -lcurl -lpoppler-cpp -lpthread"

# Compile mongoose.o if it doesn't exist
if [ ! -f ./src/mongoose.o ]; then
	echo "Compiling mongoose"
	MONGOOSE_OPTS="-DMG_MAX_RECV_SIZE=52428800 -DMG_IO_SIZE=1048576"
	gcc -O3 -c $MONGOOSE_OPTS ./src/mongoose.c -o ./src/mongoose.o
fi

# -Wno-unused_function for stbi_resize
time g++ -std=c++20 $BUILD_MODE -DXDHS_BOT -Wall -Werror -Wpedantic -Wno-unused-function -Wno-maybe-uninitialized -fno-rtti $LIB_DATE_OPTS -I./src/date ./src/tz.cpp ./src/mongoose.o ./src/xdhs_bot.cpp $LIBS -o $BINARY_NAME

# Copy compiled binary to host.
if [ "$HOSTNAME" != harvest-sigma ]; then
	strip $BINARY_NAME
	scp -o PubkeyAuthentication=no $BINARY_NAME tandem@harvest-sigma.bnr.la:~/dev/XDHS_Bot/
fi
