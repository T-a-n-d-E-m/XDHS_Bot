#!/bin/bash

# This script will copy files from the repository to where they need to be and restart the eventbot.service.

# Works on Debian 11. May need tweaking for other distros...

# Directory to install EventBot
INSTALL_DIR="/opt/EventBot"

# Check we're actually on the host server
if [ "$HOSTNAME" != harvest-sigma ]; then
	echo "You can only run this script on the host server."
	exit
fi

# Check the version we're installing is RELEASE
VERSION=$(./eventbot -version)
if [ "$VERSION" != Release ]; then
	echo "Trying to install $VERSION build. Run './build.sh release' to build the installable version."
	exit
fi

# Need to be root to create users, services etc.
if [ $(id -u) -ne 0 ]; then
	echo "Must run as root."
	exit
fi

# Create the eventbot group, if it doesn't already exist.
if ! id -g "eventbot" > /dev/null 2&>1; then
	addgroup --system eventbot
fi

# Create the eventbot user, if it doesn't already exist.
if ! id -u "eventbot" > /dev/null 2&>1; then
	# Create the user
	adduser --system --ingroup eventbot --home=$INSTALL_DIR --no-create-home --disabled-login eventbot
fi

# Create the installation directory, if it doesn't already exist.
mkdir -p $INSTALL_DIR
chown eventbot:eventbot $INSTALL_DIR 

# Will fail on first run of this script, but that doesn't really matter.
systemctl stop eventbot

# Copy the EventBot executable to the installation directory and change permissions and ownership.
cp eventbot $INSTALL_DIR
chown eventbot:eventbot $INSTALL_DIR/eventbot
chmod 500 $INSTALL_DIR/eventbot

# Log file. Only needs to be done once.
# TODO: Set up log rotation?
touch $INSTALL_DIR/eventbot.log
chown eventbot:eventbot $INSTALL_DIR/eventbot.log
chmod 600 $INSTALL_DIR/eventbot.log

# bot.ini.
# Has to be manually created. See bot.ini.templace for variables.
chown eventbot:eventbot $INSTALL_DIR/bot.ini
chmod 400 $INSTALL_DIR/bot.ini

#cp eventbot.service /etc/systemd/system
#chown root:root /etc/systemd/system/eventbot.service
#systemctl daemon-reload
#systemctl enable eventbot
#systemctl start eventbot
