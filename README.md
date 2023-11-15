A Discord bot for the XMage Draft Historical Society. Used for scheduling multiple weekly Magic: The Gathering drafts. This bot is created specifically for our needs and channel layout and thus is unlikely to work "out of the box" for other servers.

Still in development. Features are added, changed or removed frequently.

Installation as a systemd service on Debian 11 (Bullseye):

	Create a user and group for the eventbot service

		- sudo addgroup --system eventbot

		- sudo adduser --system --ingroup eventbot --home=/opt/EventBot/ --disabled-login eventbot


	Clone this repository to your build server.

	Install headers and libs for libDPP++, MariaDB, fmt and Curl.

	Compile the service with './build.sh release'

	Run the ./install.sh script

	Edit /opt/EventBot/bot.ini. See bot.ini.template for what each line is for.

		mysql_host=

		mysql_username=

		mysql_password=

		mysql_port=

		logfile_path=

		discord_token=
	
	TODO: Write the log to /var/log/eventbot.log and use logrotated?



Requires:

DPP: https://github.com/brainboxdotcc/DPP

fmt: https://github.com/fmtlib/fmt

curl (Install with package manager)
