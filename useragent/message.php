<?php
class Message
{
	# Outgoing message.
	var $message;

	# Incoming message envelope.
	var $user;
	var $subject;
	var $author;
	var $seqNum;
	var $date;
	var $time;
	var $length;

	# Incoming message header fields.
	var $type;
	var $contentType;

	function findFriendClaimId( $user, $identity )
	{
		$result = dbQuery(
			"SELECT friend_claim.id FROM friend_claim " .
			"JOIN user ON friend_claim.user_id = user.id " .
			"JOIN identity ON friend_claim.identity_id = identity.id " . 
			"WHERE user.user = %e AND friend_claim.iduri = %e",
			$user,
			$identity
		);

		if ( count( $result ) == 1 )
			return (int) $result[0]['id'];
		return -1;
	}

	function findRelationshipId( $user, $identity )
	{
		$result = dbQuery(
			"SELECT relationship.id " .
			"FROM relationship " .
			"JOIN user ON relationship.user_id = user.id " .
			"JOIN identity ON relationship.identity_id = identity.id " . 
			"WHERE user.user = %e AND identity.iduri = %e",
			$user,
			$identity
		);

		if ( count( $result ) == 1 )
			return (int) $result[0]['id'];
		return -1;
	}

	function findRelationshipSelf( $userId )
	{
		$result = dbQuery(
			"SELECT id FROM relationship " .
			"WHERE user_id = %l AND type = %l",
			$userId, REL_TYPE_SELF
		);

		if ( count( $result ) == 1 )
			return (int) $result[0]['id'];
		return -1;
	}

	function findUserId( $user )
	{
		$results = dbQuery(
			"SELECT id FROM user WHERE user.user = %e",
			$user
		);

		if ( count( $results ) == 1 )
			return (int) $results[0]['id'];
		return -1;
	}

	function findUserIdentity( $user )
	{
		$results = dbQuery(
			"SELECT identity FROM user WHERE user.user = %e",
			$user
		);

		if ( count( $results ) == 1 )
			return (int) $results[0]['id'];
		return -1;
	}

	function nameChange( $newName )
	{
		/* User message */
		$headers = 
			"Content-Type: text/plain\r\n" .
			"Type: name-change\r\n" .
			"\r\n";

		$this->message = $headers . $newName;
	}

	function recvNameChange( $user, $author_id, $seqNum, 
			$date, $time, $msg, $contextType )
	{
		if ( $contextType != 'text/plain' )
			return;

		$id = $this->findRelationshipId( $user, $author_id );
		dbQuery( 
			"UPDATE relationship SET name = %e " . 
			"WHERE id = %L",
			$msg[1], $id );
	}

	function photoUpload( $id, $fileName )
	{
		$headers = 
			"Content-Type: image/jpg\r\n" .
			"Resource-Id: $id\r\n" .
			"Type: photo-upload\r\n" .
			"\r\n";

		$MAX_BRD_PHOTO_SIZE = 16384;
		$file = fopen( $fileName, "rb" );
		$data = fread( $file, $MAX_BRD_PHOTO_SIZE );
		fclose( $file );

		$this->message = $headers . $data;
	}

	function recvPhotoUpload( $forUser, $author, $seqNum,
			$date, $time, $msg, $contextType )
	{
		global $DATA_DIR;

		/* Need a resource id. */
		if ( !isset( $msg[0]['resource-id'] ) )
			return;

		$local_resid = $seqNum;
		$remote_resid = (int) $msg[0]['resource-id'];

		$user_id = $this->findUserId( $forUser );
		$author_id = $this->findFriendClaimId( $forUser, $author );

		/* The message body is photo data. Write the photo to disk. */
		$path = sprintf( "%s/%s/pub-%ld.jpg", $DATA_DIR, $forUser, $seqNum );

		$length = strlen( $msg[1] );
		$f = fopen( $path, "wb" );
		fwrite( $f, $msg[1], $length );
		fclose( $f );

		$name = sprintf( "pub-%ld.jpg", $seqNum );

		dbQuery(
			"INSERT INTO activity " .
			"	( user_id, author_id, seq_num, time_published, " . 
			"		time_received, type, local_resid, remote_resid, message ) " .
			"VALUES ( %l, %l, %l, %e, now(), %e, %l, %l, %e )",
			$user_id,
			$author_id, 
			$seqNum, 
			$date . ' ' . $time,
			'PHT',
			$local_resid, 
			$remote_resid, 
			$name
		);
	}
	
	function broadcast( $text )
	{
		/* User message */
		$headers = 
			"Content-Type: text/plain\r\n" .
			"Type: broadcast\r\n" .
			"\r\n";

		$this->message = $headers . $text;
	}

	function recvBroadcast( $user, $author, $seqNum,
			$date, $time, $msg, $contextType )
	{
		/* Need a resource id. */
		if ( $contextType != 'text/plain' )
			return;

		$userId = $this->findUserId( $user );
		$authorId = $this->findRelationshipId( $user, $author );

		dbQuery(
			"INSERT INTO activity " .
			"	( user_id, author_id, seq_num, time_published, " . 
			"		time_received, type, message ) " .
			"VALUES ( %l, %l, %l, %e, now(), %e, %e )",
			$userId, $authorId,
			$seqNum, $date . ' ' . $time,
			'MSG', $msg[1]
		);
	}


	function boardPost( $text )
	{
		/* User message. */
		$headers = 
			"Content-Type: text/plain\r\n" .
			"Type: board-post\r\n" .
			"\r\n";

		$this->message = $headers . $text;
	}

	function recvBoardPost( $user, $subject, $author,
			$seqNum, $date, $time, $msg, $contextType )
	{
		$userId = $this->findUserId( $user );
		$authorId = $this->findRelationshipId( $user, $author );
		$subjectId = $this->findRelationshipId( $user, $subject );

		dbQuery(
			"INSERT INTO activity " .
			"	( " .
			"		user_id, author_id, subject_id, seq_num, time_published, " .
			"		time_received, type, message " .
			"	) " .
			"VALUES ( %l, %l, %l, %l, %e, now(), %e, %e )",
			$userId,
			$authorId, 
			$subjectId, 
			$seqNum, 
			$date . ' ' . $time,
			'BRD',
			$msg[1]
		);
	}


	function recvRemoteBoardPost( $user, $subject, $msg, $contextType )
	{
		$userId = $this->findUserId( $user );
		$authorId = $this->findRelationshipSelf( $userId );
		$subjectId = $this->findRelationshipId( $user, $subject );

		dbQuery(
			"INSERT INTO activity " .
			"	( user_id, author_id, subject_id, published, time_published, type, message ) " .
			"VALUES ( %l, %l, %l, true, now(), %e, %e )",
			$userId,
			$authorId,
			$subjectId, 
			'BRD',
			$msg[1]
		);
	}


	function parse( $file, $len )
	{
		$headers = array();
		$left = $len;
		while ( true ) {
			$line = fgets( $file );
			if ( $line === "\r\n" ) {
				$left -= 2;
				break;
			}
			$left -= strlen( $line );

			/* Parse headers. */
			$replaced = preg_replace( '/Content-Type:[\t ]*/i', '', $line );
			if ( $replaced !== $line )
				$headers['content-type'] = trim($replaced);

			$replaced = preg_replace( '/Resource-Id:[\t ]*/i', '', $line );
			if ( $replaced !== $line )
				$headers['resource-id'] = trim($replaced);

			$replaced = preg_replace( '/Type:[\t ]*/i', '', $line );
			if ( $replaced !== $line )
				$headers['type'] = trim($replaced);

			if ( $left <= 0 )
				break;
		}
		$msg = $left > 0 ? fread( $file, $left ) : "";
		return array( $headers, $msg );
	}

	function parseBroadcast( $user, $author,
		$seqNum, $date, $time, $length, $file )
	{
		$msg = $this->parse( $file, $length );

		if ( isset( $msg[0]['type'] ) && isset( $msg[0]['content-type'] ) ) {
			$type = $msg[0]['type'];
			$contextType = $msg[0]['content-type'];

			switch ( $type ) {
				case 'name-change':
					$this->recvNameChange( $user, $author,
							$seqNum, $date, $time,
							$msg, $contextType );
					break;
				case 'photo-upload':
					$this->recvPhotoUpload( $user, $author,
							$seqNum, $date, $time,
							$msg, $contextType );
					break;
				case 'broadcast':
					$this->recvBroadcast( $user, $author,
							$seqNum, $date, $time,
							$msg, $contextType );
					break;
			}
		}
	}

	function parseMessage( $use, $author, $date, $time, $length, $file )
	{
		$msg = $this->parse( $file, $length );

		if ( isset( $msg[0]['type'] ) && isset( $msg[0]['content-type'] ) ) {
			$type = $msg[0]['type'];
			$contextType = $msg[0]['content-type'];

			switch ( $type ) {
				default:
					break;
			}
		}
	}

	function parseRemoteMessage( $user, $subject, $author,
			$seqNum, $date, $time, $length, $file )
	{
		# Read the message from stdin.
		$msg = $this->parse( $file, $length );

		if ( isset( $msg[0]['type'] ) && isset( $msg[0]['content-type'] ) ) {
			$type = $msg[0]['type'];
			$contextType = $msg[0]['content-type'];

			switch ( $type ) {
				case 'board-post':
					$this->recvBoardPost( $user,
							$subject, $author, $seqNum,
							$date, $time, $msg, $contextType );
					break;
			}
		}
	}

	function parseRemotePublication( $user, $subject, $length, $file )
	{
		# Read the message from stdin.
		$msg = $this->parse( $file, $length );

		if ( isset( $msg[0]['type'] ) && isset( $msg[0]['content-type'] ) ) {
			$type = $msg[0]['type'];
			$contextType = $msg[0]['content-type'];

			switch ( $type ) {
				case 'board-post':
					$this->recvRemoteBoardPost( $user, 
							$subject, $msg, $contextType );
					break;
			}
		}
	}
};
?>
