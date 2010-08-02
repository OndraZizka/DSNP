#!/bin/bash
#
# Copyright (c) 2007-2010, Adrian Thurston <thurston@complang.org>
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

DSNPD_CONF=@sysconfdir@/dsnpd.conf
PHP_CONF=@sysconfdir@/config.php
DATADIR=@datadir@
LOCALSTATEDIR=@localstatedir@

#
# Config for all sites
#

# Port for the server.
CFG_PORT=7085

# Start the config file.
{ echo '<?php'; echo; } #>> $PHP_CONF

echo
echo "Please give a short name for the site. It should contain only letters and"
echo "numbers and should begin with a letter. This name will be used internally to"
echo "identify the installation. It will not be visible to the user. If you are"
echo "finished giving installations just press enter."
echo

while true; do 
	read -p 'installation name: ' NAME

	if [ -z "$NAME" ]; then
		done=yes;
		break;
	fi
	if echo $NAME | grep '^[a-zA-Z][a-zA-Z0-9]*$' >/dev/null; then
		break
	fi
	echo; echo error: name did not validate; echo
done 

[ -n "$done" ] && break;

echo
echo "Please give the Uniform Resource Identifier (URI) of this site. This will be"
echo "the installations public name. It should start with 'https://' and end with '/'."
echo

while true; do 
	read -p 'installation uri: ' URI_IN

	if echo $URI_IN | grep '^https:\/\/.*\/$' >/dev/null; then
		break
	fi
	echo; echo error: uri did not validate; echo
done 

CFG_HOST=`echo $URI_IN | sed 's/^https:\/\///; s/\/.*$//;'`
CFG_URI=$URI_IN;
CFG_PATH=`echo $URI_IN | sed 's/^https:\/\///; s/^[^\/]*//;'`

# Make a key for communication from the frontend to backend.
CFG_COMM_KEY=`head -c 24 < /dev/urandom | xxd -p`

cat << EOF
Please choose a password to protect the new database user with. 

EOF

while true; do
	read -s -p 'password: ' CFG_ADMIN_PASS; echo
	read -s -p '   again: ' AGAIN; echo

	if [ "$CFG_ADMIN_PASS" != "$AGAIN" ]; then
		echo; echo error: passwords do not match; echo
		continue
	fi

	if [ -z "$CFG_ADMIN_PASS" ]; then
		echo; echo error: password must not be empty; echo 
		continue
	fi
	break;
done


	#
	# Init the database.
	#

	#
	# Add the site to the dsnpd config file.
	#

	cat >> $DSNPD_CONF << EOF

===== $NAME =====
CFG_URI = $CFG_URI
CFG_HOST = $CFG_HOST
CFG_PATH = $CFG_PATH
CFG_DB_HOST = localhost
CFG_DB_USER = ${NAME}_dsnp
CFG_DB_DATABASE = ${NAME}
CFG_ADMIN_PASS = $CFG_ADMIN_PASS
CFG_COMM_KEY = $CFG_COMM_KEY
CFG_PORT = $CFG_PORT
CFG_TLS_CA_CERTS = /etc/ssl/certs/ca-certificates.crt
CFG_TLS_CRT = SET_THIS
CFG_TLS_KEY = SET_THIS
EOF

#CFG_TLS_CA_CERTS = /etc/ssl/certs/ca-certificates.crt
#CFG_TLS_CRT = /etc/ssl/local/yoho.crt
#CFG_TLS_KEY = /etc/ssl/local/yoho.key

	#
	# Add the site to the PHP config file.
	#

grep -v '^\?>' $PHP_CONF > $PHP_CONF.new

	cat >> $PHP_CONF.new << EOF

if ( strpos( \$_SERVER['HTTP_HOST'] . \$_SERVER['REQUEST_URI'], '$CFG_HOST$CFG_PATH' ) === 0 ) {
	\$CFG_SITE_NAME = '$NAME DSNP';
	\$CFG_NAME = '$NAME';
	\$CFG_URI = '$CFG_URI';
	\$CFG_HOST = '$CFG_HOST';
	\$CFG_PATH = '$CFG_PATH';
	\$CFG_DB_HOST = 'localhost';
	\$CFG_DB_USER = '${NAME}_ua';
	\$CFG_DB_DATABASE = '${NAME}_ua';
	\$CFG_ADMIN_PASS = '$CFG_ADMIN_PASS';
	\$CFG_COMM_KEY = '$CFG_COMM_KEY';
	\$CFG_PORT = $CFG_PORT;
	\$CFG_IM_CONVERT = 'gm convert';
	\$CFG_USE_RECAPTCHA = 'unused';
	\$CFG_RC_PUBLIC_KEY = 'unused';
	\$CFG_RC_PRIVATE_KEY = 'unused';
}
?>
EOF

cat $PHP_CONF.new > $PHP_CONF
rm $PHP_CONF.new

echo
echo "Thank you, now initializing the database. Please login as root@localhost."
echo "Please ignore any \"Can't drop\" messages."
echo

mkdir $LOCALSTATEDIR/lib/dsnp/$NAME
mkdir $LOCALSTATEDIR/lib/dsnp/$NAME/data
mkdir $LOCALSTATEDIR/lib/dsnp/$NAME/tmp
mkdir $LOCALSTATEDIR/lib/dsnp/$NAME/tmp/cache
mkdir $LOCALSTATEDIR/lib/dsnp/$NAME/tmp/logs
mkdir $LOCALSTATEDIR/lib/dsnp/$NAME/tmp/sessions
mkdir $LOCALSTATEDIR/lib/dsnp/$NAME/tmp/tests

chown -R www-data:www-data $LOCALSTATEDIR/lib/dsnp/$NAME
