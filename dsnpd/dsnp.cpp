/*
 * Copyright (c) 2008-2009, Adrian Thurston <thurston@complang.org>
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

#include "dsnp.h"
#include "encrypt.h"
#include "string.h"
#include "disttree.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <mysql.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/sha.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

#define NOTIF_LOG_FILE LOGDIR "/notification.log"


#define LOGIN_TOKEN_LASTS 86400

void set_config_by_uri( const char *uri )
{
	c = config_first;
	while ( c != 0 && strcmp( c->CFG_URI, uri ) != 0 )
		c = c->next;

	if ( c == 0 ) {
		fatal( "bad site\n" );
		exit(1);
	}
}

void set_config_by_name( const char *name )
{
	c = config_first;
	while ( c != 0 && strcmp( c->name, name ) != 0 )
		c = c->next;

	if ( c == 0 ) {
		fatal( "bad site\n" );
		exit(1);
	}
}

char *strend( char *s )
{
	return s + strlen(s);
}

char *get_site( const char *identity )
{
	char *res = strdup( identity );
	char *last = res + strlen(res) - 1;
	while ( last[-1] != '/' )
		last--;
	*last = 0;
	return res;
}

char *bin2hex( unsigned char *data, long len )
{
	char *res = (char*)malloc( len*2 + 1 );
	for ( int i = 0; i < len; i++ ) {
		unsigned char l = data[i] & 0xf;
		if ( l < 10 )
			res[i*2+1] = '0' + l;
		else
			res[i*2+1] = 'a' + (l-10);

		unsigned char h = data[i] >> 4;
		if ( h < 10 )
			res[i*2] = '0' + h;
		else
			res[i*2] = 'a' + (h-10);
	}
	res[len*2] = 0;
	return res;
}

long hex2bin( unsigned char *dest, long len, const char *src )
{
	long slen = strlen( src ) / 2;
	if ( len < slen )
		return 0;
	
	for ( int i = 0; i < slen; i++ ) {
		char l = src[i*2+1];
		char h = src[i*2];

		if ( '0' <= l && l <= '9' )
			dest[i] = l - '0';
		else
			dest[i] = 10 + (l - 'a');
			
		if ( '0' <= h && h <= '9' )
			dest[i] |= (h - '0') << 4;
		else
			dest[i] |= (10 + (h - 'a')) << 4;
	}
	return slen;
}

AllocString bn_to_base64( const BIGNUM *n )
{
	long len = BN_num_bytes(n);
	u_char *bin = new u_char[len];
	BN_bn2bin( n, bin );
	AllocString b64 = bin_to_base64( bin, len );
	delete[] bin;
	return b64;
}

BIGNUM *base64_to_bn( const char *base64 )
{
	u_char *bin = new u_char[strlen(base64)];
	long len = base64_to_bin( bin, 0, base64 );
	BIGNUM *bn = BN_bin2bn( bin, len, 0 );
	delete[] bin;
	return bn;
}

AllocString pass_hash( const u_char *pass_salt, const char *pass )
{
	unsigned char pass_hash[SHA_DIGEST_LENGTH];
	u_char *pass_comb = new u_char[SALT_SIZE + strlen(pass)];
	memcpy( pass_comb, pass_salt, SALT_SIZE );
	memcpy( pass_comb + SALT_SIZE, pass, strlen(pass) );
	SHA1( pass_comb, SALT_SIZE+strlen(pass), pass_hash );
	return bin_to_base64( pass_hash, SHA_DIGEST_LENGTH );
}

int currentPutBk( MYSQL *mysql, const char *user, long long &generation, String &bk )
{
	DbQuery query( mysql, 
		"SELECT user.put_generation, broadcast_key "
		"FROM put_broadcast_key JOIN user "
		"WHERE user.user = put_broadcast_key.user AND "
		"	put_broadcast_key.generation <= user.put_generation AND"
		"	user.user = %e "
		"ORDER BY put_generation DESC", user );
	
	if ( query.rows() == 0 )
		fatal( "failed to get current put bk\n" );
	
	MYSQL_ROW row = query.fetchRow();
	generation = strtoll( row[0], 0, 10 );
	bk.set( row[1] );
	return 1;
}

void new_broadcast_key( MYSQL *mysql, const char *user, long long generation )
{
	unsigned char broadcast_key[RELID_SIZE];
	const char *bk = 0;

	/* Generate the relationship and request ids. */
	RAND_bytes( broadcast_key, RELID_SIZE );
	bk = bin_to_base64( broadcast_key, RELID_SIZE );

	exec_query( mysql, 
		"INSERT INTO put_broadcast_key "
		"( user, generation, broadcast_key ) "
		"VALUES ( %e, %L, %e ) ",
		user, generation, bk );
}

void try_new_user( MYSQL *mysql, const char *user, const char *pass, const char *email )
{
	//char *n, *e, *d, *p, *q, *dmp1, *dmq1, *iqmp;
	char *pass_hashed, *pass_salt_str, *id_salt_str;
	u_char pass_salt[SALT_SIZE], id_salt[SALT_SIZE];

	RAND_bytes( pass_salt, SALT_SIZE );
	pass_salt_str = bin_to_base64( pass_salt, SALT_SIZE );
	message("pass_salt_str: %s\n", bin2hex( pass_salt, SALT_SIZE ) );
	message("pass: %s\n", pass );

	RAND_bytes( id_salt, SALT_SIZE );
	id_salt_str = bin_to_base64( id_salt, SALT_SIZE );

	/* Generate a new key. */
	RSA *rsa = RSA_generate_key( 1024, RSA_F4, 0, 0 );
	if ( rsa == 0 ) {
		BIO_printf( bioOut, "ERROR key generation failed\r\n");
		return;
	}

	/* Extract the components to hex strings. */
	String n = bn_to_base64( rsa->n );
	String e = bn_to_base64( rsa->e );
	String d = bn_to_base64( rsa->d );
	String p = bn_to_base64( rsa->p );
	String q = bn_to_base64( rsa->q );
	String dmp1 = bn_to_base64( rsa->dmp1 );
	String dmq1 = bn_to_base64( rsa->dmq1 );
	String iqmp = bn_to_base64( rsa->iqmp );

	/* Hash the password. */
	pass_hashed = pass_hash( pass_salt, pass );

	/* Execute the insert. */
	exec_query( mysql,
		"INSERT INTO user "
		"("
		"	user, pass_salt, pass, email, id_salt, put_generation, "
		"	rsa_n, rsa_e, rsa_d, rsa_p, rsa_q, rsa_dmp1, rsa_dmq1, rsa_iqmp "
		")"
		"VALUES ( %e, %e, %e, %e, %e, 1, %e, %e, %e, %e, %e, %e, %e, %e );", 
		user, pass_salt_str, pass_hashed, email, id_salt_str,
		n.data, e.data, d.data, p.data, q.data, dmp1.data, dmq1.data, iqmp.data );

	RSA_free( rsa );
}

void new_user( MYSQL *mysql, const char *user, const char *pass, const char *email )
{
	exec_query( mysql, "LOCK TABLES user WRITE;");

	/* Query the user. */
	DbQuery userCheck( mysql, "SELECT user FROM user WHERE user = %e", user );
	if ( userCheck.fetchRow() ) {
		BIO_printf( bioOut, "ERROR user exists\r\n" );
		exec_query( mysql, "UNLOCK TABLES;" );
		return;
	}

	try_new_user( mysql, user, pass, email );
	BIO_printf( bioOut, "OK\r\n" );

	exec_query( mysql, "UNLOCK TABLES;" );

	/* Make the first broadcast key for the user. */
	new_broadcast_key( mysql, user, 1 );
}

void public_key( MYSQL *mysql, const char *user )
{
	MYSQL_RES *result;
	MYSQL_ROW row;

	/* Query the user. */
	exec_query( mysql, "SELECT rsa_n, rsa_e FROM user WHERE user = %e", user );

	/* Check for a result. */
	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( !row ) {
		BIO_printf( bioOut, "ERROR user not found\r\n" );
		goto free_result;
	}

	/* Everythings okay. */
	BIO_printf( bioOut, "OK %s %s\n", row[0], row[1] );

free_result:
	mysql_free_result( result );
}

long open_inet_connection( const char *hostname, unsigned short port )
{
	sockaddr_in servername;
	hostent *hostinfo;
	long socketFd, connectRes;

	/* Create the socket. */
	socketFd = socket( PF_INET, SOCK_STREAM, 0 );
	if ( socketFd < 0 )
		return ERR_SOCKET_ALLOC;

	/* Lookup the host. */
	servername.sin_family = AF_INET;
	servername.sin_port = htons(port);
	hostinfo = gethostbyname (hostname);
	if ( hostinfo == NULL ) {
		::close( socketFd );
		return ERR_RESOLVING_NAME;
	}

	servername.sin_addr = *(in_addr*)hostinfo->h_addr;

	/* Connect to the listener. */
	connectRes = connect( socketFd, (sockaddr*)&servername, sizeof(servername) );
	if ( connectRes < 0 ) {
		::close( socketFd );
		return ERR_CONNECTING;
	}

	return socketFd;
}

long fetch_public_key_db( PublicKey &pub, MYSQL *mysql, const char *identity )
{
	long result = 0;
	long query_res;
	MYSQL_RES *select_res;
	MYSQL_ROW row;

	query_res = exec_query( mysql, 
		"SELECT rsa_n, rsa_e FROM public_key WHERE identity = %e", identity );
	if ( query_res != 0 ) {
		result = ERR_QUERY_ERROR;
		goto query_fail;
	}

	/* Check for a result. */
	select_res= mysql_store_result( mysql );
	row = mysql_fetch_row( select_res );
	if ( row ) {
		pub.n = strdup( row[0] );
		pub.e = strdup( row[1] );
		result = 1;
	}

	/* Done. */
	mysql_free_result( select_res );

query_fail:
	return result;
}

long store_public_key( MYSQL *mysql, const char *identity, PublicKey &pub )
{
	long result = 0, query_res;

	query_res = exec_query( mysql,
		"INSERT INTO public_key ( identity, rsa_n, rsa_e ) "
		"VALUES ( %e, %e, %e ) ", identity, pub.n, pub.e );

	if ( query_res != 0 ) {
		result = ERR_QUERY_ERROR;
		goto query_fail;
	}

query_fail:
	return result;
}

RSA *fetch_public_key( MYSQL *mysql, const char *identity )
{
	PublicKey pub;
	RSA *rsa;

	Identity id( identity );
	id.parse();

	/* First try to fetch the public key from the database. */
	long result = fetch_public_key_db( pub, mysql, identity );
	if ( result < 0 )
		return 0;

	/* If the db fetch failed, get the public key off the net. */
	if ( result == 0 ) {
		char *site = get_site( identity );
		result = fetch_public_key_net( pub, site, id.host, id.user );
		if ( result < 0 )
			return 0;

		/* Store it in the db. */
		result = store_public_key( mysql, identity, pub );
		if ( result < 0 )
			return 0;
	}

	rsa = RSA_new();
	rsa->n = base64_to_bn( pub.n );
	rsa->e = base64_to_bn( pub.e );

	return rsa;
}


RSA *load_key( MYSQL *mysql, const char *user )
{
	long query_res;
	MYSQL_RES *result;
	MYSQL_ROW row;
	RSA *rsa;

	query_res = exec_query( mysql,
		"SELECT rsa_n, rsa_e, rsa_d, rsa_p, rsa_q, rsa_dmp1, rsa_dmq1, rsa_iqmp "
		"FROM user WHERE user = %e", user );

	if ( query_res != 0 )
		goto query_fail;

	/* Check for a result. */
	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( !row ) {
		goto free_result;
	}

	/* Everythings okay. */
	rsa = RSA_new();
	rsa->n =    base64_to_bn( row[0] );
	rsa->e =    base64_to_bn( row[1] );
	rsa->d =    base64_to_bn( row[2] );
	rsa->p =    base64_to_bn( row[3] );
	rsa->q =    base64_to_bn( row[4] );
	rsa->dmp1 = base64_to_bn( row[5] );
	rsa->dmq1 = base64_to_bn( row[6] );
	rsa->iqmp = base64_to_bn( row[7] );

free_result:
	mysql_free_result( result );
query_fail:
	return rsa;
}

long sendMessageNow( MYSQL *mysql, bool prefriend, const char *from_user,
		const char *to_identity, const char *put_relid,
		const char *msg, char **result_msg )
{
	RSA *id_pub, *user_priv;
	Encrypt encrypt;
	int encrypt_res;

	id_pub = fetch_public_key( mysql, to_identity );
	user_priv = load_key( mysql, from_user );

	encrypt.load( id_pub, user_priv );

	/* Include the null in the message. */
	encrypt_res = encrypt.signEncrypt( (u_char*)msg, strlen(msg)+1 );

	message( "send_message_now sending to: %s\n", to_identity );
	return send_message_net( mysql, prefriend, from_user, to_identity, put_relid, encrypt.sym,
			strlen(encrypt.sym), result_msg );
}

bool friend_claim_exists( MYSQL *mysql, const char *user, const char *identity )
{
	MYSQL_RES *select_res;

	/* Check to see if there is already a friend claim. */
	exec_query( mysql, "SELECT user, friend_id FROM friend_claim "
		"WHERE user = %e AND friend_id = %e",
		user, identity );
	select_res = mysql_store_result( mysql );
	if ( mysql_num_rows( select_res ) != 0 )
		return true;

	return false;
}

bool friend_request_exists( MYSQL *mysql, const char *user, const char *identity )
{
	MYSQL_RES *select_res;

	exec_query( mysql, "SELECT for_user, from_id FROM friend_request "
		"WHERE for_user = %e AND from_id = %e",
		user, identity );
	select_res = mysql_store_result( mysql );
	if ( mysql_num_rows( select_res ) != 0 )
		return true;

	return false;
}

void relid_request( MYSQL *mysql, const char *user, const char *identity )
{
	/* a) verifies challenge response
	 * b) fetches $URI/id.asc (using SSL)
	 * c) randomly generates a one-way relationship id ($FR-RELID)
	 * d) randomly generates a one-way request id ($FR-REQID)
	 * e) encrypts $FR-RELID to friender and signs it
	 * f) makes message available at $FR-URI/friend-request/$FR-REQID.asc
	 * g) redirects the user's browser to $URI/return-relid?uri=$FR-URI&reqid=$FR-REQID
	 */

	int sigRes;
	RSA *user_priv, *id_pub;
	unsigned char requested_relid[RELID_SIZE], fr_reqid[REQID_SIZE];
	char *requested_relid_str, *reqid_str;
	Encrypt encrypt;

	/* Check to make sure this isn't ourselves. */
	String ourId( "%s%s/", c->CFG_URI, user );
	if ( strcasecmp( ourId.data, identity ) == 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_FRIEND_OURSELVES );
		return;
	}

	/* Check for the existence of a friend claim. */
	if ( friend_claim_exists( mysql, user, identity ) ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_FRIEND_CLAIM_EXISTS );
		return;
	}

	/* Check for the existence of a friend request. */
	if ( friend_request_exists( mysql, user, identity ) ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_FRIEND_REQUEST_EXISTS );
		return;
	}

	/* Get the public key for the identity. */
	id_pub = fetch_public_key( mysql, identity );
	if ( id_pub == 0 ) {
		BIO_printf( bioOut, "ERROR %d\n", ERROR_PUBLIC_KEY );
		return;
	}

	/* Load the private key for the user the request is for. */
	user_priv = load_key( mysql, user );

	/* Generate the relationship and request ids. */
	RAND_bytes( requested_relid, RELID_SIZE );
	RAND_bytes( fr_reqid, REQID_SIZE );

	/* Encrypt and sign the relationship id. */
	encrypt.load( id_pub, user_priv );
	sigRes = encrypt.signEncrypt( requested_relid, RELID_SIZE );
	if ( sigRes < 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_ENCRYPT_SIGN );
		return;
	}
	
	/* Store the request. */
	requested_relid_str = bin_to_base64( requested_relid, RELID_SIZE );
	reqid_str = bin_to_base64( fr_reqid, REQID_SIZE );

	exec_query( mysql,
		"INSERT INTO relid_request "
		"( for_user, from_id, requested_relid, reqid, msg_sym ) "
		"VALUES( %e, %e, %e, %e, %e )",
		user, identity, requested_relid_str, reqid_str, encrypt.sym );

	/* Return the request id for the requester to use. */
	BIO_printf( bioOut, "OK %s\r\n", reqid_str );

	delete[] requested_relid_str;
	delete[] reqid_str;
}

void fetch_requested_relid( MYSQL *mysql, const char *reqid )
{
	long query_res;
	MYSQL_RES *select_res;
	MYSQL_ROW row;

	query_res = exec_query( mysql,
		"SELECT msg_sym FROM relid_request WHERE reqid = %e", reqid );

	if ( query_res != 0 ) {
		BIO_printf( bioOut, "ERR\r\n" );
		return;
	}

	/* Check for a result. */
	select_res = mysql_store_result( mysql );
	row = mysql_fetch_row( select_res );
	if ( row )
		BIO_printf( bioOut, "OK %s\r\n", row[0] );
	else
		BIO_printf( bioOut, "ERROR\r\n" );

	/* Done. */
	mysql_free_result( select_res );
}

long store_relid_response( MYSQL *mysql, const char *identity, const char *fr_relid_str,
		const char *fr_reqid_str, const char *relid_str, const char *reqid_str, 
		const char *sym )
{
	int result = exec_query( mysql,
		"INSERT INTO relid_response "
		"( from_id, requested_relid, returned_relid, reqid, msg_sym ) "
		"VALUES ( %e, %e, %e, %e, %e )",
		identity, fr_relid_str, relid_str, 
		reqid_str, sym );
	
	return result;
}

AllocString make_id_hash( const char *salt, const char *identity )
{
	/* Make a hash for the identity. */
	long len = strlen(salt) + strlen(identity) + 1;
	char *total = new char[len];
	sprintf( total, "%s%s", salt, identity );
	unsigned char friend_hash[SHA_DIGEST_LENGTH];
	SHA1( (unsigned char*)total, len, friend_hash );
	return bin_to_base64( friend_hash, SHA_DIGEST_LENGTH );
}

long store_friend_claim( MYSQL *mysql, const char *user, 
		const char *identity, const char *id_salt, const char *put_relid, const char *get_relid )
{
	char *friend_hash_str = make_id_hash( id_salt, identity );

	/* Insert the friend claim. */
	exec_query( mysql, "INSERT INTO friend_claim "
		"( user, friend_id, friend_salt, friend_hash, put_relid, get_relid ) "
		"VALUES ( %e, %e, %e, %e, %e, %e );",
		user, identity, id_salt, friend_hash_str, put_relid, get_relid );

	return 0;
}

void relid_response( MYSQL *mysql, const char *user, 
		const char *fr_reqid_str, const char *identity )
{
	/*  a) verifies browser is logged in as owner
	 *  b) fetches $FR-URI/id.asc (using SSL)
	 *  c) fetches $FR-URI/friend-request/$FR-REQID.asc 
	 *  d) decrypts and verifies $FR-RELID
	 *  e) randomly generates $RELID
	 *  f) randomly generates $REQID
	 *  g) encrypts "$FR-RELID $RELID" to friendee and signs it
	 *  h) makes message available at $URI/request-return/$REQID.asc
	 *  i) redirects the friender to $FR-URI/friend-final?uri=$URI&reqid=$REQID
	 */

	int verifyRes, fetchRes, sigRes;
	RSA *user_priv, *id_pub;
	unsigned char *requested_relid;
	unsigned char response_relid[RELID_SIZE], response_reqid[REQID_SIZE];
	char *requested_relid_str, *response_relid_str, *response_reqid_str;
	unsigned char message[RELID_SIZE*2];
	Encrypt encrypt;

	Identity id( identity );
	id.parse();

	/* Get the public key for the identity. */
	id_pub = fetch_public_key( mysql, id.identity );
	if ( id_pub == 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_PUBLIC_KEY );
		return;
	}

	RelidEncSig encsig;
	fetchRes = fetch_requested_relid_net( encsig, id.site, id.host, fr_reqid_str );
	if ( fetchRes < 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_FETCH_REQUESTED_RELID );
		return;
	}

	/* Load the private key for the user the request is for. */
	user_priv = load_key( mysql, user );

	/* Decrypt and verify the requested_relid. */
	encrypt.load( id_pub, user_priv );

	verifyRes = encrypt.decryptVerify( encsig.sym );
	if ( verifyRes < 0 ) {
		::message("relid_response: ERROR_DECRYPT_VERIFY\n" );
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_DECRYPT_VERIFY );
		return;
	}

	/* Verify the message is the right size. */
	if ( encrypt.decLen != RELID_SIZE ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_DECRYPTED_SIZE );
		return;
	}

	/* This should not be deleted as long as we don't do any more decryption. */
	requested_relid = encrypt.decrypted;
	
	/* Generate the relationship and request ids. */
	RAND_bytes( response_relid, RELID_SIZE );
	RAND_bytes( response_reqid, REQID_SIZE );

	memcpy( message, requested_relid, RELID_SIZE );
	memcpy( message+RELID_SIZE, response_relid, RELID_SIZE );

	/* Encrypt and sign using the same credentials. */
	sigRes = encrypt.signEncrypt( message, RELID_SIZE*2 );
	if ( sigRes < 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_ENCRYPT_SIGN );
		return;
	}

	/* Store the request. */
	requested_relid_str = bin_to_base64( requested_relid, RELID_SIZE );
	response_relid_str = bin_to_base64( response_relid, RELID_SIZE );
	response_reqid_str = bin_to_base64( response_reqid, REQID_SIZE );

	store_relid_response( mysql, identity, requested_relid_str, fr_reqid_str, 
			response_relid_str, response_reqid_str, encrypt.sym );

	/* Insert the friend claim. */
	exec_query( mysql, "INSERT INTO sent_friend_request "
		"( from_user, for_id, requested_relid, returned_relid ) "
		"VALUES ( %e, %e, %e, %e );",
		user, identity, requested_relid_str, response_relid_str );

	String args( "sent_friend_request %s %s", user, identity );
	app_notification( args, 0, 0 );
	
	/* Return the request id for the requester to use. */
	BIO_printf( bioOut, "OK %s\r\n", response_reqid_str );

	delete[] requested_relid_str;
	delete[] response_relid_str;
	delete[] response_reqid_str;
}

void fetch_response_relid( MYSQL *mysql, const char *reqid )
{
	long query_res;
	MYSQL_RES *select_res;
	MYSQL_ROW row;

	/* Execute the query. */
	query_res = exec_query( mysql,
		"SELECT msg_sym FROM relid_response WHERE reqid = %e;", reqid );
	
	if ( query_res != 0 ) {
		BIO_printf( bioOut, "ERR\r\n" );
		return;
	}

	/* Check for a result. */
	select_res = mysql_store_result( mysql );
	row = mysql_fetch_row( select_res );
	if ( row )
		BIO_printf( bioOut, "OK %s\r\n", row[0] );
	else
		BIO_printf( bioOut, "ERR\r\n" );

	/* Done. */
	mysql_free_result( select_res );
}

long verify_returned_fr_relid( MYSQL *mysql, unsigned char *fr_relid )
{
	long result = 0;
	char *requested_relid_str = bin_to_base64( fr_relid, RELID_SIZE );
	int query_res;
	MYSQL_RES *select_res;
	MYSQL_ROW row;

	query_res = exec_query( mysql,
		"SELECT from_id FROM relid_request WHERE requested_relid = %e", 
		requested_relid_str );

	/* Execute the query. */
	if ( query_res != 0 ) {
		result = -1;
		goto query_fail;
	}

	/* Check for a result. */
	select_res = mysql_store_result( mysql );
	row = mysql_fetch_row( select_res );
	if ( row )
		result = 1;

	mysql_free_result( select_res );

query_fail:
	return result;
}

void friend_final( MYSQL *mysql, const char *user, const char *reqid_str, const char *identity )
{
	/* a) fetches $URI/request-return/$REQID.asc 
	 * b) decrypts and verifies message, must contain correct $FR-RELID
	 * c) stores request for friendee to accept/deny
	 */

	int verifyRes, fetchRes;
	RSA *user_priv, *id_pub;
	unsigned char *message;
	unsigned char requested_relid[RELID_SIZE], returned_relid[RELID_SIZE];
	char *requested_relid_str, *returned_relid_str;
	unsigned char user_reqid[REQID_SIZE];
	char *user_reqid_str;
	Encrypt encrypt;
	Identity id( identity );
	id.parse();

	/* Get the public key for the identity. */
	id_pub = fetch_public_key( mysql, identity );
	if ( id_pub == 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_PUBLIC_KEY );
		return;
	}

	RelidEncSig encsig;
	fetchRes = fetch_response_relid_net( encsig, id.site, id.host, reqid_str );
	if ( fetchRes < 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_FETCH_RESPONSE_RELID );
		return;
	}
	
	/* Load the private key for the user the request is for. */
	user_priv = load_key( mysql, user );

	encrypt.load( id_pub, user_priv );

	verifyRes = encrypt.decryptVerify( encsig.sym );
	if ( verifyRes < 0 ) {
		::message("friend_final: ERROR_DECRYPT_VERIFY\n" );
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_DECRYPT_VERIFY );
		return;
	}

	/* Verify that the message is the right size. */
	if ( encrypt.decLen != RELID_SIZE*2 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_DECRYPTED_SIZE );
		return;
	}

	message = encrypt.decrypted;

	memcpy( requested_relid, message, RELID_SIZE );
	memcpy( returned_relid, message+RELID_SIZE, RELID_SIZE );

	verifyRes = verify_returned_fr_relid( mysql, requested_relid );
	if ( verifyRes != 1 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_REQUESTED_RELID_MATCH );
		return;
	}
		
	requested_relid_str = bin_to_base64( requested_relid, RELID_SIZE );
	returned_relid_str = bin_to_base64( returned_relid, RELID_SIZE );

	/* Make a user request id. */
	RAND_bytes( user_reqid, REQID_SIZE );
	user_reqid_str = bin_to_base64( user_reqid, REQID_SIZE );

	exec_query( mysql, 
		"INSERT INTO friend_request "
		" ( for_user, from_id, reqid, requested_relid, returned_relid ) "
		" VALUES ( %e, %e, %e, %e, %e ) ",
		user, identity, user_reqid_str, requested_relid_str, returned_relid_str );
	
	String args( "friend_request %s %s %s %s %s",
		user, identity, user_reqid_str, requested_relid_str, returned_relid_str );
	app_notification( args, 0, 0 );
	
	/* Return the request id for the requester to use. */
	BIO_printf( bioOut, "OK\r\n" );

	delete[] requested_relid_str;
	delete[] returned_relid_str;
}

long delete_friend_request( MYSQL *mysql, const char *user, const char *user_reqid )
{
	/* Insert the friend claim. */
	exec_query( mysql, 
		"DELETE FROM friend_request WHERE for_user = %e AND reqid = %e;",
		user, user_reqid );

	return 0;
}


int send_current_broadcast_key( MYSQL *mysql, const char *user, const char *identity )
{
	long long generation;
	String broadcast_key;

	/* Get the latest put session key. */
	currentPutBk( mysql, user, generation, broadcast_key );
	int send_res = send_broadcast_key( mysql, user, identity, generation, broadcast_key );
	if ( send_res < 0 )
		error( "sending failed %d\n", send_res );

	return 0;
}

void accept_friend( MYSQL *mysql, const char *user, const char *user_reqid )
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	char buf[2048], *result_message = 0;
	char *from_id, *requested_relid, *returned_relid;
	char *id_salt;

	/* Execute the query. */
	exec_query( mysql, "SELECT id_salt FROM user WHERE user = %e", user );

	/* Check for a result. */
	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( !row ) {
		BIO_printf( bioOut, "ERROR request not found\r\n" );
		return;
	}
	id_salt = row[0];

	/* Execute the query. */
	exec_query( mysql, "SELECT from_id, requested_relid, returned_relid "
		"FROM friend_request "
		"WHERE for_user = %e AND reqid = %e;",
		user, user_reqid );

	/* Check for a result. */
	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( !row ) {
		BIO_printf( bioOut, "ERROR request not found\r\n" );
		return;
	}

	from_id = row[0];
	requested_relid = row[1];
	returned_relid = row[2];

	/* Notify the requester. */
	sprintf( buf, "notify_accept %s %s %s\r\n", id_salt, requested_relid, returned_relid );
	message( "accept_friend sending: %s to %s from %s\n", buf, from_id, user  );
	int nfa = sendMessageNow( mysql, true, user, from_id, requested_relid, buf, &result_message );

	if ( nfa < 0 ) {
		BIO_printf( bioOut, "ERROR accept failed with %d\r\n", nfa );
		return;
	}

	notify_accept_result_parser( mysql, user, user_reqid, from_id, 
			requested_relid, returned_relid, result_message );
}

void notify_accept_returned_id_salt( MYSQL *mysql, const char *user, const char *user_reqid, 
		const char *from_id, const char *requested_relid, 
		const char *returned_relid, const char *returned_id_salt )
{
	char buf[2048];
	message( "accept_friend received: %s\n", returned_id_salt );

	/* The friendship has been accepted. Store the claim. The fr_relid is the
	 * one that we made on this end. It becomes the put_relid. */
	store_friend_claim( mysql, user, from_id, returned_id_salt, requested_relid, returned_relid );

	/* Notify the requester. */
	sprintf( buf, "registered %s %s\r\n", requested_relid, returned_relid );
	message( "accept_friend sending: %s to %s from %s\n", buf, from_id, user  );
	sendMessageNow( mysql, true, user, from_id, requested_relid, buf, 0 );

	/* Remove the user friend request. */
	delete_friend_request( mysql, user, user_reqid );

	forward_tree_insert( mysql, user, from_id, requested_relid );

	String args( "friend_request_accepted %s %s", user, from_id );
	app_notification( args, 0, 0 );

	//obtainFriendProof( mysql, user, from_id );
	BIO_printf( bioOut, "OK\r\n" );
}


long store_ftoken( MYSQL *mysql, const char *user, const char *identity,
		const char *token_str, const char *reqid_str, const char *msg_sym )
{
	long result = 0;
	int query_res;

	query_res = exec_query( mysql,
		"INSERT INTO ftoken_request "
		"( user, from_id, token, reqid, msg_sym ) "
		"VALUES ( %e, %e, %e, %e, %e ) ",
		user, identity, token_str, reqid_str, msg_sym );

	if ( query_res != 0 )
		result = ERR_QUERY_ERROR;

	return result;
}

long check_friend_claim( Identity &identity, MYSQL *mysql, const char *user, 
		const char *friend_hash )
{
	long result = 0;
	long query_res;
	MYSQL_RES *select_res;
	MYSQL_ROW row;

	query_res = exec_query( mysql,
		"SELECT friend_id FROM friend_claim WHERE user = %e AND friend_hash = %e",
		user, friend_hash );

	if ( query_res != 0 ) {
		result = ERR_QUERY_ERROR;
		goto query_fail;
	}

	select_res = mysql_store_result( mysql );
	row = mysql_fetch_row( select_res );
	if ( row ) {
		identity.identity = strdup( row[0] );
		identity.parse();
		result = 1;
	}

	/* Done. */
	mysql_free_result( select_res );

query_fail:
	return result;
}

char *user_identity_hash( MYSQL *mysql, const char *user )
{
	long result = 0;
	long query_res;
	char *identity, *id_hash_str = 0;
	MYSQL_RES *select_res;
	MYSQL_ROW row;

	query_res = exec_query( mysql,
		"SELECT id_salt FROM user WHERE user = %e", user );

	if ( query_res != 0 ) {
		result = ERR_QUERY_ERROR;
		goto query_fail;
	}

	select_res = mysql_store_result( mysql );
	row = mysql_fetch_row( select_res );
	if ( row ) {
		identity = new char[strlen(c->CFG_URI) + strlen(user) + 2];
		sprintf( identity, "%s%s/", c->CFG_URI, user );
		id_hash_str = make_id_hash( row[0], identity );
	}

	/* Done. */
	mysql_free_result( select_res );

query_fail:
	return id_hash_str;
}

void ftoken_request( MYSQL *mysql, const char *user, const char *hash )
{
	int sigRes;
	RSA *user_priv, *id_pub;
	unsigned char flogin_token[TOKEN_SIZE], reqid[REQID_SIZE];
	char *flogin_token_str, *reqid_str;
	long friend_claim;
	Identity friend_id;
	Encrypt encrypt;

	/* Check if this identity is our friend. */
	friend_claim = check_friend_claim( friend_id, mysql, user, hash );
	if ( friend_claim <= 0 ) {
		::message("ftoken_request: hash %s for user %s is not valid\n", hash, user );

		/* No friend claim ... send back a reqid anyways. Don't want to give
		 * away that there is no claim. FIXME: Would be good to fake this with
		 * an appropriate time delay. */
		RAND_bytes( reqid, RELID_SIZE );
		reqid_str = bin_to_base64( reqid, RELID_SIZE );

		/*FIXME: Hang for a bit here instead. */
		BIO_printf( bioOut, "OK %s\r\n", reqid_str );
		return;
	}

	/* Get the public key for the identity. */
	id_pub = fetch_public_key( mysql, friend_id.identity );
	if ( id_pub == 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_PUBLIC_KEY );
		return;
	}

	/* Load the private key for the user. */
	user_priv = load_key( mysql, user );

	/* Generate the login request id and relationship and request ids. */
	RAND_bytes( flogin_token, TOKEN_SIZE );
	RAND_bytes( reqid, REQID_SIZE );

	encrypt.load( id_pub, user_priv );

	/* Encrypt it. */
	sigRes = encrypt.signEncrypt( flogin_token, TOKEN_SIZE );
	if ( sigRes < 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_ENCRYPT_SIGN );
		return;
	}

	/* Store the request. */
	flogin_token_str = bin_to_base64( flogin_token, TOKEN_SIZE );
	reqid_str = bin_to_base64( reqid, REQID_SIZE );

	store_ftoken( mysql, user, friend_id.identity, 
			flogin_token_str, reqid_str, encrypt.sym );

	::message("ftoken_request: %s %s\n", reqid_str, friend_id.identity );

	/* Return the request id for the requester to use. */
	BIO_printf( bioOut, "OK %s %s %s\r\n", reqid_str,
			friend_id.identity, user_identity_hash( mysql, user ) );
}

void fetch_ftoken( MYSQL *mysql, const char *reqid )
{
	long query_res;
	MYSQL_RES *select_res;
	MYSQL_ROW row;

	query_res = exec_query( mysql,
		"SELECT msg_sym FROM ftoken_request WHERE reqid = %e", reqid );

	/* Execute the query. */
	if ( query_res != 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_DB_ERROR );
		return;
	}

	/* Check for a result. */
	select_res = mysql_store_result( mysql );
	row = mysql_fetch_row( select_res );
	if ( row )
		BIO_printf( bioOut, "OK %s\r\n", row[0] );
	else
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_NO_FTOKEN );

	/* Done. */
	mysql_free_result( select_res );

	message("fetch_ftoken finished\n");
}

void ftoken_response( MYSQL *mysql, const char *user, const char *hash, 
		const char *flogin_reqid_str )
{
	/*
	 * a) checks that $FR-URI is a friend
	 * b) if browser is not logged in fails the process (no redirect).
	 * c) fetches $FR-URI/tokens/$FR-RELID.asc
	 * d) decrypts and verifies the token
	 * e) redirects the browser to $FP-URI/submit-token?uri=$URI&token=$TOK
	 */
	int verifyRes, fetchRes;
	RSA *user_priv, *id_pub;
	unsigned char *flogin_token;
	char *flogin_token_str;
	long friend_claim;
	Identity friend_id;
	char *site;
	Encrypt encrypt;

	message("ftoken_response\n");

	/* Check if this identity is our friend. */
	friend_claim = check_friend_claim( friend_id, mysql, user, hash );
	if ( friend_claim <= 0 ) {
		/* No friend claim ... we can reveal this since ftoken_response requires
		 * that the user be logged in. */
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_NOT_A_FRIEND );
		return;
	}

	/* Get the public key for the identity. */
	id_pub = fetch_public_key( mysql, friend_id.identity );
	if ( id_pub == 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_PUBLIC_KEY );
		return;
	}

	site = get_site( friend_id.identity );

	RelidEncSig encsig;
	fetchRes = fetch_ftoken_net( encsig, site, friend_id.host, flogin_reqid_str );
	if ( fetchRes < 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_FETCH_FTOKEN );
		return;
	}

	/* Load the private key for the user the request is for. */
	user_priv = load_key( mysql, user );

	encrypt.load( id_pub, user_priv );

	/* Decrypt the flogin_token. */
	verifyRes = encrypt.decryptVerify( encsig.sym );
	if ( verifyRes < 0 ) {
		::message("ftoken_response: ERROR_DECRYPT_VERIFY\n" );
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_DECRYPT_VERIFY );
		return;
	}

	/* Check the size. */
	if ( encrypt.decLen != REQID_SIZE ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_DECRYPTED_SIZE );
		return;
	}

	flogin_token = encrypt.decrypted;
	flogin_token_str = bin_to_base64( flogin_token, RELID_SIZE );

	exec_query( mysql,
		"INSERT INTO remote_flogin_token "
		"( user, identity, login_token ) "
		"VALUES ( %e, %e, %e )",
		user, friend_id.identity, flogin_token_str );

	/* Return the login token for the requester to use. */
	BIO_printf( bioOut, "OK %s %s\r\n", flogin_token_str, friend_id.identity );

	free( flogin_token_str );
}

void add_get_tree( MYSQL *mysql, const char *user, const char *friend_id, long long generation )
{
	DbQuery check( mysql,
		"SELECT user FROM get_tree "
		"WHERE user = %e AND friend_id = %e AND generation = %L",
		user, friend_id, generation );
	
	if ( check.rows() == 0 ) {
		/* Insert an entry for this relationship. */
		exec_query( mysql, 
			"INSERT INTO get_tree "
			"( user, friend_id, generation )"
			"VALUES ( %e, %e, %L )", user, friend_id, generation );
	}
}

void broadcast_key( MYSQL *mysql, const char *relid, const char *user,
		const char *friend_id, long long generation, const char *bk )
{
	add_get_tree( mysql, user, friend_id, generation );

	/* Make the query. */
	exec_query( mysql, 
			"UPDATE get_tree "
			"SET broadcast_key = %e "
			"WHERE user = %e AND friend_id = %e AND generation = %L",
			bk, user, friend_id, generation );
	
	BIO_printf( bioOut, "OK\n" );
}

void forward_to( MYSQL *mysql, const char *user, const char *friend_id,
		int child_num, long long generation, const char *to_site, const char *relid )
{
	add_get_tree( mysql, user, friend_id, generation );

	switch ( child_num ) {
	case 0:
		exec_query( mysql, 
			"UPDATE get_tree "
			"SET site_ret = %e, relid_ret = %e "
			"WHERE user = %e AND friend_id = %e AND generation = %L",
			to_site, relid, user, friend_id, generation );
		break;
	case 1:
		exec_query( mysql, 
			"UPDATE get_tree "
			"SET site1 = %e, relid1 = %e "
			"WHERE user = %e AND friend_id = %e AND generation = %L",
			to_site, relid, user, friend_id, generation );
		break;
	case 2:
		exec_query( mysql, 
			"UPDATE get_tree "
			"SET site2 = %e, relid2 = %e "
			"WHERE user = %e AND friend_id = %e AND generation = %L",
			to_site, relid, user, friend_id, generation );
		break;
	}

	BIO_printf( bioOut, "OK\n" );
}

long queue_message_db( MYSQL *mysql, const char *from_user,
		const char *to_identity, const char *relid, const char *message )
{
	exec_query( mysql,
		"INSERT INTO message_queue "
		"( from_user, to_id, relid, message, send_after ) "
		"VALUES ( %e, %e, %e, %e, NOW() ) ",
		from_user, to_identity, relid, message );

	return 0;
}

long queue_broadcast_db( MYSQL *mysql, const char *to_site,
		const char *relid, long long generation, const char *msg )
{
	exec_query( mysql,
		"INSERT INTO broadcast_queue "
		"( to_site, relid, generation, message, send_after ) "
		"VALUES ( %e, %e, %L, %e, NOW() ) ",
		to_site, relid, generation, msg );

	return 0;
}


long queue_message( MYSQL *mysql, const char *from_user,
		const char *to_identity, const char *message )
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	RSA *id_pub, *user_priv;
	Encrypt encrypt;
	int encrypt_res;
	const char *relid;

	exec_query( mysql, 
		"SELECT put_relid FROM friend_claim "
		"WHERE user = %e AND friend_id = %e ",
		from_user, to_identity );

	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( row == 0 )
		goto free_result;
	relid = row[0];

	id_pub = fetch_public_key( mysql, to_identity );
	user_priv = load_key( mysql, from_user );

	encrypt.load( id_pub, user_priv );

	/* Include the null in the message. */
	encrypt_res = encrypt.signEncrypt( (u_char*)message, strlen(message)+1 );

	queue_message_db( mysql, from_user, to_identity, relid, encrypt.sym );
free_result:
	mysql_free_result( result );
	return 0;
}

long send_broadcast_key( MYSQL *mysql, const char *from_user, const char *to_identity, 
		long long generation, const char *broadcast_key )
{
	static char buf[8192];

	sprintf( buf,
		"broadcast_key %lld %s\r\n", 
		generation, broadcast_key );

	return queue_message( mysql, from_user, to_identity, buf );
}

long send_forward_to( MYSQL *mysql, const char *from_user, const char *to_identity, 
		int childNum, long long generation, const char *forwardToSite, const char *relid )
{
	static char buf[8192];

	sprintf( buf, 
		"forward_to %d %lld %s %s\r\n", 
		childNum, generation, forwardToSite, relid );

	return queue_message( mysql, from_user, to_identity, buf );
}

void prefriend_message( MYSQL *mysql, const char *relid, const char *msg )
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	RSA *id_pub, *user_priv;
	Encrypt encrypt;
	int decrypt_res;
	const char *user, *friend_id;

	exec_query( mysql, 
		"SELECT from_user, for_id FROM sent_friend_request "
		"WHERE requested_relid = %e",
		relid );

	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( row == 0 ) {
		message("prefriend_message: could not locate friend via sent_friend_request\n");
		BIO_printf( bioOut, "ERROR finding friend\r\n" );
		goto free_result;
	}

	user = row[0];
	friend_id = row[1];

	user_priv = load_key( mysql, user );
	id_pub = fetch_public_key( mysql, friend_id );

	encrypt.load( id_pub, user_priv );
	decrypt_res = encrypt.decryptVerify( msg );

	if ( decrypt_res < 0 ) {
		message("prefriend_message: decrypting message failed\n");
		BIO_printf( bioOut, "ERROR %s\r\n", encrypt.err );
		goto free_result;
	}

	prefriend_message_parser( mysql, relid, user, friend_id, (char*)encrypt.decrypted );

free_result:
	mysql_free_result( result );
	return;
}

void receive_message( MYSQL *mysql, const char *relid, const char *message )
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	RSA *id_pub, *user_priv;
	Encrypt encrypt;
	int decrypt_res;
	const char *user, *friend_id;

	exec_query( mysql, 
		"SELECT user, friend_id FROM friend_claim "
		"WHERE get_relid = %e",
		relid );

	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( row == 0 ) {
		BIO_printf( bioOut, "ERROR finding friend\r\n" );
		return;
	}
	user = row[0];
	friend_id = row[1];

	user_priv = load_key( mysql, user );
	id_pub = fetch_public_key( mysql, friend_id );

	encrypt.load( id_pub, user_priv );
	decrypt_res = encrypt.decryptVerify( message );

	if ( decrypt_res < 0 ) {
		BIO_printf( bioOut, "ERROR %s\r\n", encrypt.err );
		return;
	}

	int parseResult = message_parser( mysql, relid, user, friend_id, (char*)encrypt.decrypted );
	if ( parseResult < 0 ) {
		BIO_printf( bioOut, "ERROR\r\n" );
		return;
	}
}

void login( MYSQL *mysql, const char *user, const char *pass )
{
	const long lasts = LOGIN_TOKEN_LASTS;

	DbQuery login( mysql, 
		"SELECT user, pass_salt, pass, id_salt FROM user WHERE user = %e", user );

	if ( login.rows() == 0 ) {
		message( "login of %s failed, user not found\n", user );
		BIO_printf( bioOut, "ERROR\r\n" );
		return;
	}

	MYSQL_ROW row = login.fetchRow();
	char *salt_str = row[1];
	char *pass_str = row[2];
	char *id_salt_str = row[3];

	/* Hash the password using the sale found in the DB. */
	u_char pass_salt[SALT_SIZE];
	base64_to_bin( pass_salt, 0, salt_str );
	String pass_hashed = pass_hash( pass_salt, pass );

	/* Check the login. */
	if ( strcmp( pass_hashed, pass_str ) != 0 ) {
		message("login of %s failed, pass hashes do not match\n", user );
		BIO_printf( bioOut, "ERROR\r\n" );
		return;
	}

	/* Login successful. Make a token. */
	u_char token[TOKEN_SIZE];
	RAND_bytes( token, TOKEN_SIZE );
	String token_str = bin_to_base64( token, TOKEN_SIZE );

	/* Record the token. */
	DbQuery loginToken( mysql, 
		"INSERT INTO login_token ( user, login_token, expires ) "
		"VALUES ( %e, %e, date_add( now(), interval %l second ) )", 
		user, token_str.data, lasts );

	String identity( "%s%s/", c->CFG_URI, user );
	String id_hash_str = make_id_hash( id_salt_str, identity );

	BIO_printf( bioOut, "OK %s %s %ld\r\n", id_hash_str.data, token_str.data, lasts );

	message("login of %s successful\n", user );
}

void submit_ftoken( MYSQL *mysql, const char *token )
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	long lasts = LOGIN_TOKEN_LASTS;
	char *user, *from_id, *hash;

	exec_query( mysql,
		"SELECT user, from_id FROM ftoken_request WHERE token = %e",
		token );

	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( row == 0 ) {
		BIO_printf( bioOut, "ERROR\r\n" );
		goto free_result;
	}
	user = row[0];
	from_id = row[1];

	exec_query( mysql, 
		"INSERT INTO flogin_token ( user, identity, login_token, expires ) "
		"VALUES ( %e, %e, %e, date_add( now(), interval %l second ) )", 
		user, from_id, token, lasts );

	exec_query( mysql,
		"SELECT friend_hash FROM friend_claim WHERE friend_id = %e", from_id );

	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( row == 0 ) {
		BIO_printf( bioOut, "ERROR\r\n" );
		goto free_result;
	}
	hash = row[0];

	BIO_printf( bioOut, "OK %s %ld %s\r\n", hash, lasts, from_id );

free_result:
	mysql_free_result( result );
}

void encrypt_remote_broadcast( MYSQL *mysql, const char *user,
		const char *subject_id, const char *token,
		long long seq_num, const char *msg )
{
	Encrypt encrypt;
	RSA *user_priv, *id_pub;
	int sigRes;
	String broadcast_key;
	long long generation;

	long mLen = strlen(msg);

	message( "entering encrypt_remote_broadcast( %s, %s, %s, %lld, %s)\n", 
		user, subject_id, token, seq_num, msg );

	DbQuery flogin( mysql,
		"SELECT user FROM remote_flogin_token "
		"WHERE user = %e AND identity = %e AND login_token = %e",
		user, subject_id, token );

	if ( flogin.rows() == 0 ) {
		message("failed to find user from provided login token\n");
		BIO_printf( bioOut, "ERROR\r\n" );
		return;
	}

	/* Get the current time. */
	String timeStr = timeNow();

	user_priv = load_key( mysql, user );
	id_pub = fetch_public_key( mysql, subject_id );

	/* Notifiy the frontend. */
	String args( "remote_publication %s %s %ld", 
			user, subject_id, mLen );
	app_notification( args, msg, mLen );

	/* Find current generation and youngest broadcast key */
	currentPutBk( mysql, user, generation, broadcast_key );
	message("current put_bk: %lld %s\n", generation, broadcast_key.data );

	/* Make the full message. */
	String command( "remote_inner %lld %s %ld\r\n", seq_num, timeStr.data, mLen );
	String full = addMessageData( command, msg, mLen );

	encrypt.load( id_pub, user_priv );
	sigRes = encrypt.bkSignEncrypt( broadcast_key, (u_char*)full.data, full.length );
	if ( sigRes < 0 ) {
		BIO_printf( bioOut, "ERROR %d\r\n", ERROR_ENCRYPT_SIGN );
		return;
	}

	message( "encrypt_remote_broadcast enc: %s\n", encrypt.sym );

	u_char reqid[REQID_SIZE];
	RAND_bytes( reqid, REQID_SIZE );
	const char *reqid_str = bin_to_base64( reqid, REQID_SIZE );

	exec_query( mysql,
		"INSERT INTO remote_broadcast_request "
		"	( user, identity, reqid, generation, sym ) "
		"VALUES ( %e, %e, %e, %L, %e )",
		user, subject_id, reqid_str, generation, encrypt.sym );

	BIO_printf( bioOut, "REQID %s\r\n", reqid_str );
}

void friendProofRequest( MYSQL *mysql, const char *user, const char *friend_id )
{
	Encrypt encrypt;
	int sigRes;
	String broadcast_key;
	long long generation;

	message( "received friend proof request\n" );

	DbQuery friendClaim( mysql,
		"SELECT user FROM friend_claim "
		"WHERE user = %e AND friend_id = %e",
		user, friend_id );

	if ( friendClaim.rows() == 0 ) {
		message("friend_proof_request for non user-friend pair\n");
		BIO_printf( bioOut, "ERROR\r\n" );
		return;
	}

	RSA *user_priv = load_key( mysql, user );
	RSA *id_pub = fetch_public_key( mysql, friend_id );

	/* Find current generation and youngest broadcast key */
	currentPutBk( mysql, user, generation, broadcast_key );
	message("current put_bk: %lld %s\n", generation, broadcast_key.data );

	/* Get the current time. */
	String timeStr = timeNow();
	String command( "friend_proof %s\r\n", timeStr.data );

	/* Find current generation and youngest broadcast key */
	currentPutBk( mysql, user, generation, broadcast_key );

	encrypt.load( id_pub, user_priv );
	sigRes = encrypt.bkSignEncrypt( broadcast_key, (u_char*)command.data, command.length );
	if ( sigRes < 0 ) {
			BIO_printf( bioOut, "ERROR %d\r\n", ERROR_ENCRYPT_SIGN );
			return;
	}

	message( "friend_proof_request enc: %s\n", encrypt.sym );
	String resultCmd( "encrypted_broadcast %lld %s\r\n", generation, encrypt.sym );

	encrypt.signEncrypt( (u_char*)resultCmd.data, resultCmd.length+1 );

	BIO_printf( bioOut, "RESULT %d\r\n", strlen(encrypt.sym) );
	BIO_write( bioOut, encrypt.sym, strlen(encrypt.sym) );
}

char *decrypt_result( MYSQL *mysql, const char *from_user, 
		const char *to_identity, const char *user_message )
{
	RSA *id_pub, *user_priv;
	Encrypt encrypt;
	int decrypt_res;

	::message( "decrypting result %s %s %s\n", from_user, to_identity, user_message );

	user_priv = load_key( mysql, from_user );
	id_pub = fetch_public_key( mysql, to_identity );

	encrypt.load( id_pub, user_priv );
	message( "about to\n");
	decrypt_res = encrypt.decryptVerify( user_message );

	if ( decrypt_res < 0 ) {
		message( "decrypt_verify failed\n");
		return 0;
	}

	message( "decrypt_result: %s\n", encrypt.decrypted );

	return strdup((char*)encrypt.decrypted);
}

long notify_accept( MYSQL *mysql, const char *for_user, const char *from_id,
		const char *id_salt, const char *requested_relid, const char *returned_relid )
{
	RSA *id_pub, *user_priv;
	Encrypt encrypt;
	MYSQL_RES *result;
	MYSQL_ROW row;
	char *returned_id_salt;

	/* Verify that there is a friend request. */
	DbQuery checkSentRequest( mysql, 
		"SELECT from_user FROM sent_friend_request "
		"WHERE from_user = %e AND for_id = %e AND requested_relid = %e and returned_relid = %e",
		for_user, from_id, requested_relid, returned_relid );

	if ( checkSentRequest.rows() != 1 ) {
		message("accept friendship failed: could not find send_friend_request\r\n");
		BIO_printf( bioOut, "ERROR request not found\r\n");
		return 0;
	}

	user_priv = load_key( mysql, for_user );
	id_pub = fetch_public_key( mysql, from_id );

	/* The relid is the one we made on this end. It becomes the put_relid. */
	store_friend_claim( mysql, for_user, from_id, id_salt, returned_relid, requested_relid );

	/* Clear the sent_freind_request. */
	exec_query( mysql, "SELECT id_salt FROM user WHERE user = %e", for_user );

	/* Check for a result. */
	result = mysql_store_result( mysql );
	row = mysql_fetch_row( result );
	if ( !row ) {
		BIO_printf( bioOut, "ERROR request not found\r\n" );
		return 0;
	}
	returned_id_salt = row[0];

	String resultCommand( "returned_id_salt %s\r\n", returned_id_salt );

	encrypt.load( id_pub, user_priv );
	encrypt.signEncrypt( (u_char*)resultCommand.data, resultCommand.length+1 );

	BIO_printf( bioOut, "RESULT %d\r\n", strlen(encrypt.sym) );
	BIO_write( bioOut, encrypt.sym, strlen(encrypt.sym) );

	return 0;
}

long registered( MYSQL *mysql, const char *for_user, const char *from_id,
		const char *requested_relid, const char *returned_relid )
{
	::message("registered: starting\n");

	forward_tree_insert( mysql, for_user, from_id, returned_relid );

	DbQuery removeSentRequest( mysql, 
		"DELETE FROM sent_friend_request "
		"WHERE from_user = %e AND for_id = %e AND requested_relid = %e AND "
		"	returned_relid = %e",
		for_user, from_id, requested_relid, returned_relid );

	BIO_printf( bioOut, "OK\r\n" );

	::message("registered: finished\n");

	String args( "sent_friend_request_accepted %s %s", for_user, from_id );
	app_notification( args, 0, 0 );

	//obtainFriendProof( mysql, for_user, from_id );
	return 0;
}

int maxArgs( const char *src )
{
	/* Assume at least one. */
	int n = 1;
	for ( ; *src != 0; src++ ) {
		if ( *src == ' ' )
			n++;
	}
	return n;
}

void parseArgs( char **argv, long &dst, const char *args )
{
	const char *last = args;
	for ( const char *src = args; ; src++ ) {
		if ( *src == ' ' || *src == 0 ) {
			argv[dst] = new char[src-last+1];
			memcpy( argv[dst], last, src-last );
			argv[dst][src-last] = 0;

			dst++;
			last = src + 1;
		}

		if ( *src == 0 )
			break;
	}
}

char *const*make_notif_argv( const char *args )
{
	int n = maxArgs(c->CFG_NOTIFICATION) + 2 + maxArgs(args) + 1;
	char **argv = new char*[n];
	long dst = 0;
	parseArgs( argv, dst, c->CFG_NOTIFICATION );
	argv[dst++] = strdup(c->CFG_HOST);
	argv[dst++] = strdup(c->CFG_PATH);
	parseArgs( argv, dst, args );
	argv[dst++] = 0;

	return argv;
}

void app_notification( const char *args, const char *data, long length )
{
	message( "notification callout with args %s\n", args );

	int fds[2];	
	int res = pipe( fds );
	if ( res < 0 ) {
		error("pipe creation failed\n");
		return;
	}

	pid_t pid = fork();
	if ( pid < 0 ) {
		error("error forking for app notification\n");
	}
	else if ( pid == 0 ) {
		close( fds[1] );
		FILE *log = fopen(NOTIF_LOG_FILE, "at");
		if ( log == 0 ) {
			error( "could not open notification log file, using /dev/null\n" );
			log = fopen("/dev/null", "wt");
			if ( log == 0 )
				fatal( "could not open /dev/null\n" );
		}

		dup2( fds[0], 0 );
		dup2( fileno(log), 1 );
		dup2( fileno(log), 2 );
		execvp( "php", make_notif_argv( args ) );
		exit(0);
	}
	
	close( fds[0] );

	FILE *p = fdopen( fds[1], "wb" );
	if ( length > 0 ) 
		fwrite( data, 1, length, p );
	fclose( p );
	wait( 0 );
}