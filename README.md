A Discord bot for the XMage Draft Historical Society. Used for scheduling multiple weekly Magic: The Gathering drafts. This bot is created specifically for our needs and channel layout and thus is unlikely to work "out of the box" for other servers.

Still in development. Features are added, changed or removed frequently.

Installation as a systemd service on Debian 11 (Bullseye):

	Install MySQL/MariaDB and configure an account for the bot to use. It will need DELETE, INSERT, SELECT, UPDATE privileges:
		mysql -u root -p
			CREATE DATABASE XDHS;
			CREATE USER 'xdhs'@localhost IDENTIFIED BY 'password goes here';
			GRANT DELETE, INSERT, SELECT, UPDATE ON XDHS.* TO 'xdhs'@localhost;	
			FLUSH PRIVILEGES;

	Clone this repository to your build server.

	Install headers and libs for libDPP++, MariaDB, fmt and Curl.

	Compile the executable with './build.sh release'

	Run ./eventbot -sql to generate the database schema.sql file. Create these tables with the mysql admin tool.

	Run the ./install.sh script to create the installation directory, username/group and install the systemd service.

	Edit /opt/EventBot/bot.ini. See bot.ini.template for what each line is for.

		eventbot_host=

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

MySQL or MariaDB, curl (Install with package manager)

 libpoppler-cpp https://poppler.freedesktop.org/api/cpp/
