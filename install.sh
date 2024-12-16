#!/bin/bash

# This script will copy files from the repository to where they need to be and restart the xdhs_bot.service.

# Works on Debian 11. May need tweaking for other distros...

# Directory to install XDHS Bot
INSTALL_DIR="/opt/XDHS_Bot"

# Check we're actually on the host server
if [ "$HOSTNAME" != harvest-sigma ]; then
	echo "You can only run this script on the host server."
	exit
fi

# Check the version we're installing is RELEASE
VERSION=$(./xdhs_bot -version)
if [ "$VERSION" != Release ]; then
	echo "Trying to install $VERSION build. Run './build.sh release' to build the installable version."
	exit
fi

# Need to be root to create users, services etc.
if [ $(id -u) -ne 0 ]; then
	echo "Script must be run as root."
	exit
fi

# Create the xdhs_bot group, if it doesn't already exist.
if ! id -g "xdhs_bot" > /dev/null 2&>1; then
	addgroup --system xdhs_bot
fi

# Create the xdhs_bot user, if it doesn't already exist.
if ! id -u "xdhs_bot" > /dev/null 2&>1; then
	# Create the user
	adduser --system --ingroup xdhs_bot --home=$INSTALL_DIR --no-create-home --disabled-login xdhs_bot
fi

# Create the installation directory, if it doesn't already exist.
mkdir -p $INSTALL_DIR
chown xdhs_bot:xdhs_bot $INSTALL_DIR

# Will fail on first run of this script, but that doesn't really matter.
systemctl stop xdhs_bot

# Copy the XDHS Bot executable to the installation directory and change permissions and ownership.
cp xdhs_bot $INSTALL_DIR
chown xdhs_bot:xdhs_bot $INSTALL_DIR/xdhs_bot
chmod 500 $INSTALL_DIR/xdhs_bot

# Log file. Only needs to be done once.
# TODO: Set up log rotation?
touch $INSTALL_DIR/xdhs_bot.log
chown xdhs_bot:xdhs_bot $INSTALL_DIR/xdhs_bot.log
chmod 600 $INSTALL_DIR/xdhs_bot.log

# bot.ini.
# Has to be manually created. See bot.ini.templace for variables.
chown xdhs_bot:xdhs_bot $INSTALL_DIR/bot.ini
chmod 400 $INSTALL_DIR/bot.ini

# Copy static assets, if they're newer, and create the www-root generation directory.
cp --update --recursive gfx $INSTALL_DIR
cp --update --recursive www-root $INSTALL_DIR
mkdir -p $INSTALL_DIR/www-root
chown xdhs_bot:xdhs_bot $INSTALL_DIR/gfx
chown xdhs_bot:xdhs_bot $INSTALL_DIR/www-root

cp xdhs_bot.service /etc/systemd/system
chown root:root /etc/systemd/system/xdhs_bot.service
systemctl daemon-reload
systemctl enable xdhs_bot
systemctl start xdhs_bot
