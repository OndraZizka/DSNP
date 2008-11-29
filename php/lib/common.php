<?php

/* 
 * Copyright (c) 2007, Adrian Thurston <thurston@cs.queensu.ca>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

function loginForm()
{
	?><form method="post" action="submitlogin.php">
	Owner Login to Iduri:
	<input type="password" name="password">
	<input type="submit">
	</form><?php
}

function friendLoginForm()
{
	?><form method="post" action="submitflogin.php">
	Friend Login to Iduri:
	<input type="text" size=70 name="uri">
	<input type="submit">
	</form><?php
}

function adminLoginForm()
{
	?><form method="post" action="salogin.php">
	Admin Login:
	<input type="password" name="password">
	<input type="submit">
	</form><?php
}

function importId( $gnupg, $uri )
{
	global $CFG_HTTP_GET_TIMEOUT;
	$get_options = array( "timeout" => $CFG_HTTP_GET_TIMEOUT );
	$response = http_get( $uri . 'id.asc', $get_options, $info );
	$message = http_parse_message( $response );

	/* Import the key. */
	$res = $gnupg->import( $message->body );

	$fp = $res['fingerprint'];
	$res = $gnupg->keyinfo( '0x' . $fp );

	/* Need to verify what is returned. If it fails then delete it. */
	$uid = $res[0]['uids'][0];
	$user_id = $uid['uid'];

	return $fp;
}

function encryptSign( $gnupg, $to_fp, $plain )
{
	global $CFG_FINGERPRINT;
	$gnupg->clearencryptkeys();
	$gnupg->clearsignkeys();
	$gnupg->setsignmode( gnupg::SIG_MODE_NORMAL );
	$gnupg->addencryptkey( $to_fp );
	$gnupg->addsignkey( $CFG_FINGERPRINT, '' );
	return $gnupg->encryptsign( $plain );
}

function publishMessage( $prefix, $name, $message )
{
	$fn = $prefix . '/' . $name . '.asc';
	$fd = fopen( $fn, 'wt' );
	fwrite( $fd, $message );
	fclose( $fd );
	chmod( $fn, 0644 );
}

function decryptVerify( $gnupg, $message )
{
	global $CFG_FINGERPRINT;
	$plain= "";
	$gnupg->cleardecryptkeys();
	$gnupg->setsignmode( gnupg::SIG_MODE_NORMAL );
	$gnupg->adddecryptkey( $CFG_FINGERPRINT, "" );
	$res = $gnupg->decryptverify( $message->body, $plain );
	return $plain;
}

function fetchHTTP( $resource )
{
	global $CFG_HTTP_GET_TIMEOUT;
	$get_options = array( "timeout" => $CFG_HTTP_GET_TIMEOUT );
	$response = http_get( $resource, $get_options, $info );
	return http_parse_message( $response );
}

function fetchMessage( $from_uri, $prefix, $name )
{
	$asc = $from_uri . $prefix . '/' . $name . '.asc';
	return fetchHTTP( $asc );
}

function writeFriendData( $friend_fp, $friend_data )
{
	$s = serialize( $friend_data );

	$fd = fopen( 'downloads/' . $friend_fp . '.srl', 'w' );
	fprintf( $fd, "%d\n", strlen( $s ) );
	fwrite( $fd, $s );
	fprintf( $fd, "\n" );
	fclose( $fd );
}

function writeData( $data )
{
	$s = serialize( $data );

	$fd = fopen( 'data.srl', 'w' );	
	fprintf( $fd, "%d\n", strlen( $s ) );
	fwrite( $fd, $s );
	fprintf( $fd, "\n" );
}

function readData( )
{
	$fd = fopen( 'data.srl', 'r' );	
	fscanf( $fd, "%d\n", &$slen  );
	$s = fread( $fd, $slen );
	return unserialize( $s );
}

function friendList( $data )
{
	$owner = $_SESSION['auth'] == 'owner';
	$friends = $data['friends'];
	foreach ( $friends as $uri => $fp ) {
		echo "<a href=\"$uri\">$uri</a> ";
		$fn = 'downloads/' . $fp . '.srl';
		if ( is_file( $fn ) ) {
			$fd = fopen( $fn, 'r' );
			fclose( $fd );
			echo "have data ";
		}
		else {
			echo "no data ";
		}
		if ( $owner ) {
			echo "<a href=\"refresh.php?uri=" . urlencode($uri) . "\">refresh</a>";
		}
		echo "<br>";
	}
}

function showFriendRequests( $data )
{
	$requests = $data['requests'];
	foreach ( $requests as $furi => $n ) {
		echo "<b>Friend Request:</b> <a href=\"$furi\">$furi</a>&nbsp;&nbsp;<a\n";
		echo "href=\"answer.php?uri=" . urlencode($furi) . "&a=yes\">yes</a>&nbsp;&nbsp;<a\n";
		echo "href=\"answer.php?uri=" . urlencode($furi) . "&a=no\">no</a><br>\n";
	}
}

function requireFriend()
{
	if ( $_SESSION['auth'] != 'friend' )
		exit("You do not have permission to access this page\n");
}

function requireOwner()
{
	if ( $_SESSION['auth'] != 'owner' )
		exit("You do not have permission to access this page\n");
}

?>
