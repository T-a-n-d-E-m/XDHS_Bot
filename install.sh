#!/bin/bash

# Works on Debian 11. May need tweaking for other distros.

INSTALL_DIR="/opt/EventBot" # Directory to install EventBot

# This script will copy files from the repository to where they need to be and restart the eventbot.service.
# Also needs:
#     1) A group account: sudo addgroup --system eventbot
#     2) A user account: sudo adduser --system --ingroup eventbot --home=$INSTALL_DIR --disabled-login eventbot
#     3) A bot.ini file in $INSTALL_DIR

if [ $(id -u) -ne 0 ]; then
	echo "Must run as root."
	exit
fi

#systemctl stop eventbot

# Install the executable and change permissions and ownership
cp eventbot $INSTALL_DIR
chown eventbot:eventbot $INSTALL_DIR/eventbot
chmod 500 $INSTALL_DIR/eventbot

# Log file
touch $INSTALL_DIR/eventbot.log
chown eventbot:eventbot $INSTALL_DIR/eventbot.log
chmod 600 $INSTALL_DIR/eventbot.log

# bot.ini
chown eventbot:eventbot $INSTALL_DIR/bot.ini
chmod 400 $INSTALL_DIR/bot.ini

#cp eventbot.service /etc/systemd/system
#chown root:root /etc/systemd/system/eventbot.service
#systemctl daemon-reload
#systemctl enable eventbot
#systemctl start eventbot
