/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"
#include "transfer_queue.h"
#include "condor_debug.h"
#include "condor_daemon_core.h"
#include "condor_commands.h"
#include "condor_config.h"
#include "dc_transfer_queue.h"
#include "condor_email.h"

TransferQueueRequest::TransferQueueRequest(ReliSock *sock,char const *fname,char const *jobid,char const *queue_user,bool downloading,time_t max_queue_age):
	m_sock(sock),
	m_queue_user(queue_user),
	m_jobid(jobid),
	m_fname(fname),
	m_downloading(downloading),
	m_max_queue_age(max_queue_age)
{
	m_gave_go_ahead = false;
	m_time_born = time(NULL);
	m_time_go_ahead = 0;

		// the up_down_queue_user name uniquely identifies the user and the direction of transfer
	if( m_downloading ) {
		m_up_down_queue_user = "D";
	}
	else {
		m_up_down_queue_user = "U";
	}
	m_up_down_queue_user += m_queue_user;
}

TransferQueueRequest::~TransferQueueRequest() {
	if( m_sock ) {
		daemonCore->Cancel_Socket( m_sock );
		delete m_sock;
		m_sock = NULL;
	}
}

char const *
TransferQueueRequest::Description() {
	m_description.formatstr("%s %s job %s for %s (initial file %s)",
					m_sock ? m_sock->peer_description() : "",
					m_downloading ? "downloading" : "uploading",
					m_jobid.Value(),
					m_queue_user.Value(),
					m_fname.Value());
	return m_description.Value();
}

bool
TransferQueueRequest::SendGoAhead(XFER_QUEUE_ENUM go_ahead,char const *reason) {
	ASSERT( m_sock );

	m_sock->encode();
	ClassAd msg;
	msg.Assign(ATTR_RESULT,(int)go_ahead);
	if( reason ) {
		msg.Assign(ATTR_ERROR_STRING,reason);
	}

		// how often should transfer processes send a report of I/O activity
		//   0 means never
	int report_interval = param_integer("TRANSFER_IO_REPORT_INTERVAL",10,0);
	msg.Assign(ATTR_REPORT_INTERVAL,report_interval);

	if(!putClassAd( m_sock, msg ) || !m_sock->end_of_message()) {
		dprintf(D_ALWAYS,
				"TransferQueueRequest: failed to send GoAhead to %s\n",
				Description() );
		return false;
	}

	m_gave_go_ahead = true;
	m_time_go_ahead = time(NULL);
	return true;
}

TransferQueueManager::TransferQueueManager() {
	m_max_uploads = 0;
	m_max_downloads = 0;
	m_check_queue_timer = -1;
	m_default_max_queue_age = 0;
	m_uploading = 0;
	m_downloading = 0;
	m_waiting_to_upload = 0;
	m_waiting_to_download = 0;
	m_upload_wait_time = 0;
	m_download_wait_time = 0;
	m_round_robin_counter = 0;
	m_round_robin_garbage_counter = 0;
	m_round_robin_garbage_time = time(NULL);
	m_update_iostats_interval = 0;
	m_update_iostats_timer = -1;
	m_publish_flags = 0;

	m_stat_pool.AddProbe(ATTR_TRANSFER_QUEUE_NUM_UPLOADING,&m_uploading_stat,NULL,IF_BASICPUB|m_uploading_stat.PubDefault);
	m_stat_pool.AddProbe(ATTR_TRANSFER_QUEUE_NUM_DOWNLOADING,&m_downloading_stat,NULL,IF_BASICPUB|m_downloading_stat.PubDefault);
	m_stat_pool.AddProbe(ATTR_TRANSFER_QUEUE_NUM_WAITING_TO_UPLOAD,&m_waiting_to_upload_stat,NULL,IF_BASICPUB|m_waiting_to_upload_stat.PubDefault);
	m_stat_pool.AddProbe(ATTR_TRANSFER_QUEUE_NUM_WAITING_TO_DOWNLOAD,&m_waiting_to_download_stat,NULL,IF_BASICPUB|m_waiting_to_download_stat.PubDefault);
	m_stat_pool.AddProbe(ATTR_TRANSFER_QUEUE_UPLOAD_WAIT_TIME,&m_upload_wait_time_stat,NULL,IF_BASICPUB|m_upload_wait_time_stat.PubDefault);
	m_stat_pool.AddProbe(ATTR_TRANSFER_QUEUE_DOWNLOAD_WAIT_TIME,&m_download_wait_time_stat,NULL,IF_BASICPUB|m_download_wait_time_stat.PubDefault);
	RegisterStats(NULL,m_iostats);
}

TransferQueueManager::~TransferQueueManager() {
	TransferQueueRequest *client = NULL;

	m_xfer_queue.Rewind();
	while( m_xfer_queue.Next( client ) ) {
		delete client;
	}
	m_xfer_queue.Clear();

	if( m_check_queue_timer != -1 ) {
		daemonCore->Cancel_Timer( m_check_queue_timer );
	}
	if( m_update_iostats_timer != -1 ) {
		daemonCore->Cancel_Timer( m_update_iostats_timer );
	}
}

void
TransferQueueManager::InitAndReconfig() {
	m_max_downloads = param_integer("MAX_CONCURRENT_DOWNLOADS",10,0);
	m_max_uploads = param_integer("MAX_CONCURRENT_UPLOADS",10,0);
	m_default_max_queue_age = param_integer("MAX_TRANSFER_QUEUE_AGE",3600*2,0);

	m_update_iostats_interval = param_integer("TRANSFER_IO_REPORT_INTERVAL",10,0);
	if( m_update_iostats_interval != 0 ) {
		if( m_update_iostats_timer != -1 ) {
			ASSERT( daemonCore->Reset_Timer_Period(m_update_iostats_timer,m_update_iostats_interval) == 0 );
		}
		else {
			m_update_iostats_timer = daemonCore->Register_Timer(
				m_update_iostats_interval,
				m_update_iostats_interval,
				(TimerHandlercpp)&TransferQueueManager::UpdateIOStats,
				"UpdateIOStats",this);
			ASSERT( m_update_iostats_timer != -1 );
		}
	}

	m_publish_flags = IF_BASICPUB;
	std::string publish_config;
	if( param(publish_config,"STATISTICS_TO_PUBLISH") ) {
		m_publish_flags = generic_stats_ParseConfigString(publish_config.c_str(), "TRANSFER", "TRANSFER", m_publish_flags);
	}

	std::string iostat_timespans;
	param(iostat_timespans,"TRANSFER_IO_REPORT_TIMESPANS");

	std::string iostat_timespans_err;
	if( !ParseEMAHorizonConfiguration(iostat_timespans.c_str(),ema_config,iostat_timespans_err) ) {
		EXCEPT("Error in TRANSFER_IO_REPORT_TIMESPANS=%s: %s",iostat_timespans.c_str(),iostat_timespans_err.c_str());
	}

	m_iostats.ConfigureEMAHorizons(ema_config);

	for( QueueUserMap::iterator user_itr = m_queue_users.begin();
		 user_itr != m_queue_users.end();
		 ++user_itr )
	{
		user_itr->second.iostats.ConfigureEMAHorizons(ema_config);
	}
}

void
TransferQueueManager::RegisterHandlers() {
	int rc = daemonCore->Register_Command(
		TRANSFER_QUEUE_REQUEST,
		"TRANSFER_QUEUE_REQUEST",
		(CommandHandlercpp)&TransferQueueManager::HandleRequest,
		"TransferQueueManager::HandleRequest",
		this,
		WRITE );
	ASSERT( rc >= 0 );
}

int TransferQueueManager::HandleRequest(int cmd,Stream *stream)
{
	ReliSock *sock = (ReliSock *)stream;
	ASSERT( cmd == TRANSFER_QUEUE_REQUEST );

	ClassAd msg;
	sock->decode();
	if( !msg.initFromStream( *sock ) || !sock->end_of_message() ) {
		dprintf(D_ALWAYS,
				"TransferQueueManager: failed to receive transfer request "
				"from %s.\n", sock->peer_description() );
		return FALSE;
	}

	bool downloading = false;
	MyString fname;
	MyString jobid;
	MyString queue_user;
	if( !msg.LookupBool(ATTR_DOWNLOADING,downloading) ||
		!msg.LookupString(ATTR_FILE_NAME,fname) ||
		!msg.LookupString(ATTR_JOB_ID,jobid) ||
		!msg.LookupString(ATTR_USER,queue_user) )
	{
		MyString msg_str;
		sPrintAd(msg_str, msg);
		dprintf(D_ALWAYS,"TransferQueueManager: invalid request from %s: %s\n",
				sock->peer_description(), msg_str.Value());
		return FALSE;
	}

		// Currently, we just create the client with the default max queue
		// age.  If it becomes necessary to customize the maximum age
		// on a case-by-case basis, it should be easy to adjust.

	TransferQueueRequest *client =
		new TransferQueueRequest(
			sock,
			fname.Value(),
			jobid.Value(),
			queue_user.Value(),
			downloading,
			m_default_max_queue_age);

	if( !AddRequest( client ) ) {
		delete client;
		return KEEP_STREAM; // we have already closed this socket
	}

	return KEEP_STREAM;
}

bool
TransferQueueManager::AddRequest( TransferQueueRequest *client ) {
	ASSERT( client );

	MyString error_desc;
	if( daemonCore->TooManyRegisteredSockets(client->m_sock->get_file_desc(),&error_desc))
	{
		dprintf(D_FULLDEBUG,"TransferQueueManager: rejecting %s to avoid overload: %s\n",
				client->Description(),
				error_desc.Value());
		client->SendGoAhead(XFER_QUEUE_NO_GO,error_desc.Value());
		return false;
	}

	dprintf(D_FULLDEBUG,
			"TransferQueueManager: enqueueing %s.\n",
			client->Description());

	int rc = daemonCore->Register_Socket(client->m_sock,
		"<file transfer request>",
		(SocketHandlercpp)&TransferQueueManager::HandleReport,
		"HandleReport()", this, ALLOW);

	if( rc < 0 ) {
		dprintf(D_ALWAYS,
				"TransferQueueManager: failed to register socket for %s.\n",
				client->Description());
		return false;
	}

	ASSERT( daemonCore->Register_DataPtr( client ) );

	m_xfer_queue.Append( client );

	TransferQueueChanged();

	return true;
}

int
TransferQueueManager::HandleReport( Stream *sock )
{
	TransferQueueRequest *client;
	m_xfer_queue.Rewind();
	while( m_xfer_queue.Next( client ) ) {
		if( client->m_sock == sock ) {
			if( !client->ReadReport(this) ) {
				dprintf(D_FULLDEBUG,
						"TransferQueueManager: dequeueing %s.\n",
						client->Description());

				delete client;
				m_xfer_queue.DeleteCurrent();

				TransferQueueChanged();
			}
			return KEEP_STREAM;
		}
	}

		// should never get here
	m_xfer_queue.Rewind();
	MyString clients;
	while( m_xfer_queue.Next( client ) ) {
		clients.formatstr_cat(" (%p) %s\n",
					 client->m_sock,client->m_sock->peer_description());
	}
	EXCEPT("TransferQueueManager: ERROR: disconnect from client (%p) %s;"
		   " not found in list: %s\n",
		   sock,
		   sock->peer_description(),
		   clients.Value());
	return FALSE; // close socket
}

bool
TransferQueueRequest::ReadReport(TransferQueueManager *manager)
{
	MyString report;
	m_sock->decode();
	if( !m_sock->get(report) ||
		!m_sock->end_of_message() )
	{
		return false;
	}

	if( report.IsEmpty() ) {
		return false;
	}

	unsigned report_time;
	unsigned report_interval_usec;
	unsigned recent_bytes_sent;
	unsigned recent_bytes_received;
	unsigned recent_usec_file_read;
	unsigned recent_usec_file_write;
	unsigned recent_usec_net_read;
	unsigned recent_usec_net_write;
	IOStats iostats;

	int rc = sscanf(report.Value(),"%u %u %u %u %u %u %u %u",
					&report_time,
					&report_interval_usec,
					&recent_bytes_sent,
					&recent_bytes_received,
					&recent_usec_file_read,
					&recent_usec_file_write,
					&recent_usec_net_read,
					&recent_usec_net_write);
	if( rc != 8 ) {
		dprintf(D_ALWAYS,"Failed to parse I/O report from file transfer worker %s: %s.\n",
				m_sock->peer_description(),report.Value());
		return false;
	}

	iostats.bytes_sent = (double)recent_bytes_sent;
	iostats.bytes_received = (double)recent_bytes_received;
	iostats.file_read = (double)recent_usec_file_read/1000000;
	iostats.file_write = (double)recent_usec_file_write/1000000;
	iostats.net_read = (double)recent_usec_net_read/1000000;
	iostats.net_write = (double)recent_usec_net_write/1000000;

	manager->AddRecentIOStats(iostats,m_up_down_queue_user);
	return true;
}

void
TransferQueueManager::TransferQueueChanged() {
	if( m_check_queue_timer != -1 ) {
			// already have a timer set
		return;
	}
	m_check_queue_timer = daemonCore->Register_Timer(
		0,(TimerHandlercpp)&TransferQueueManager::CheckTransferQueue,
		"CheckTransferQueue",this);
}

void
TransferQueueManager::SetRoundRobinRecency(const std::string &user)
{
	unsigned int old_counter = m_round_robin_counter;

	GetUserRec(user).recency = ++m_round_robin_counter;

		// clear history if the counter wraps, so we do not keep favoring those who have wrapped
	if( m_round_robin_counter < old_counter ) {
		ClearRoundRobinRecency();
	}
}

void
TransferQueueManager::ClearRoundRobinRecency()
{
	for( QueueUserMap::iterator itr = m_queue_users.begin();
		 itr != m_queue_users.end();
		 itr++ )
	{
		itr->second.recency = 0;
	}
}

bool
TransferQueueManager::TransferQueueUser::Stale(unsigned int stale_recency)
{
		// If this user has recently done anything, this record is not stale
	if( !(recency < stale_recency && running==0 && idle==0) ) {
		return false; // not stale
	}
		// Check for non-negligible iostat moving averages, so we
		// don't lose interesting data about big I/O users.
	if( iostats.bytes_sent.BiggestEMARate()     > 100000 ||
		iostats.bytes_received.BiggestEMARate() > 100000 ||
		iostats.file_read.BiggestEMARate()      > 0.1 ||
		iostats.file_write.BiggestEMARate()     > 0.1 ||
		iostats.net_read.BiggestEMARate()       > 0.1 ||
		iostats.net_write.BiggestEMARate()      > 0.1 )
	{
		return false; // not stale
	}
	return true;
}

void
TransferQueueManager::CollectUserRecGarbage(ClassAd *unpublish_ad)
{
		// To prevent unbounded growth, remove user records that have
		// not been touched in the past hour.

	time_t now = time(NULL);
		// use abs() here so big clock jumps do not cause long
		// periods of no garbage collection
	if( abs((int)(now - m_round_robin_garbage_time)) > 3600 ) {
		int num_removed = 0;

		for( QueueUserMap::iterator itr = m_queue_users.begin();
			 itr != m_queue_users.end(); )
		{
			TransferQueueUser &u = itr->second;
			if( u.Stale(m_round_robin_garbage_counter) ) {
				UnregisterStats(itr->first.c_str(),u.iostats,unpublish_ad);
					// increment the iterator before calling erase, while it is still valid
				m_queue_users.erase(itr++);
				++num_removed;
			}
			else {
				++itr;
			}
		}

		if( num_removed ) {
			dprintf(D_ALWAYS,"TransferQueueManager::CollectUserRecGarbage: removed %d entries.\n",num_removed);
		}

		m_round_robin_garbage_time = now;
		m_round_robin_garbage_counter = m_round_robin_counter;
	}
}

TransferQueueManager::TransferQueueUser &
TransferQueueManager::GetUserRec(const std::string &user)
{
	QueueUserMap::iterator itr;

	itr = m_queue_users.find(user);
	if( itr == m_queue_users.end() ) {
		itr = m_queue_users.insert(QueueUserMap::value_type(user,TransferQueueUser())).first;
		itr->second.iostats.ConfigureEMAHorizons(ema_config);
		RegisterStats(user.c_str(),itr->second.iostats);
	}
	return itr->second;
}

void
TransferQueueManager::RegisterStats(char const *user,IOStats &iostats,bool unregister,ClassAd *unpublish_ad)
{
	std::string attr;
	bool downloading = true;
	bool uploading = true;
	std::string user_attr;
	int flags = IF_BASICPUB;
	if( user && user[0] ) {
		flags = IF_VERBOSEPUB;
			// The first character of the up_down_user tells us if it is uploading or downloading
		if( user[0] == 'U' ) downloading = false;
		if( user[0] == 'D' ) uploading = false;
		char const *at = strchr(user,'@');
		if( at ) {
			user_attr.append(user+1,at-(user+1));
		}
		else {
			user_attr = user+1;
		}
		user_attr += "_";
	}

	flags |= stats_entry_sum_ema_rate<double>::PubDefault;

	if( downloading ) {
		formatstr(attr,"%sFileTransferDownloadBytes",user_attr.c_str());
		if( unregister ) {
			m_stat_pool.RemoveProbe(attr.c_str());
			iostats.bytes_received.Unpublish(*unpublish_ad,attr.c_str());
		}
		else {
			m_stat_pool.AddProbe(attr.c_str(),&iostats.bytes_received,NULL,flags);
		}
		formatstr(attr,"%sFileTransferFileWriteSeconds",user_attr.c_str());
		if( unregister ) {
			m_stat_pool.RemoveProbe(attr.c_str());
			iostats.file_write.Unpublish(*unpublish_ad,attr.c_str());
		}
		else {
			m_stat_pool.AddProbe(attr.c_str(),&iostats.file_write,NULL,flags);
		}
		formatstr(attr,"%sFileTransferNetReadSeconds",user_attr.c_str());
		if( unregister ) {
			m_stat_pool.RemoveProbe(attr.c_str());
			iostats.net_read.Unpublish(*unpublish_ad,attr.c_str());
		}
		else {
			m_stat_pool.AddProbe(attr.c_str(),&iostats.net_read,NULL,flags);
		}
	}
	if( uploading ) {
		formatstr(attr,"%sFileTransferUploadBytes",user_attr.c_str());
		if( unregister ) {
			m_stat_pool.RemoveProbe(attr.c_str());
			iostats.bytes_sent.Unpublish(*unpublish_ad,attr.c_str());
		}
		else {
			m_stat_pool.AddProbe(attr.c_str(),&iostats.bytes_sent,NULL,flags);
		}
		formatstr(attr,"%sFileTransferFileReadSeconds",user_attr.c_str());
		if( unregister ) {
			m_stat_pool.RemoveProbe(attr.c_str());
			iostats.file_read.Unpublish(*unpublish_ad,attr.c_str());
		}
		else {
			m_stat_pool.AddProbe(attr.c_str(),&iostats.file_read,NULL,flags);
		}
		formatstr(attr,"%sFileTransferNetWriteSeconds",user_attr.c_str());
		if( unregister ) {
			m_stat_pool.RemoveProbe(attr.c_str());
			iostats.net_write.Unpublish(*unpublish_ad,attr.c_str());
		}
		else {
			m_stat_pool.AddProbe(attr.c_str(),&iostats.net_write,NULL,flags);
		}
	}
}

void
TransferQueueManager::ClearTransferCounts()
{
	m_waiting_to_upload = 0;
	m_waiting_to_download = 0;
	m_upload_wait_time = 0;
	m_download_wait_time = 0;

	for( QueueUserMap::iterator itr = m_queue_users.begin();
		 itr != m_queue_users.end();
		 itr++ )
	{
		itr->second.running = 0;
		itr->second.idle = 0;
	}
}

void
TransferQueueManager::CheckTransferQueue() {
	TransferQueueRequest *client = NULL;
	int downloading = 0;
	int uploading = 0;
	bool clients_waiting = false;

	m_check_queue_timer = -1;

	ClearTransferCounts();

	m_xfer_queue.Rewind();
	while( m_xfer_queue.Next(client) ) {
		if( client->m_gave_go_ahead ) {
			GetUserRec(client->m_up_down_queue_user).running++;
			if( client->m_downloading ) {
				downloading += 1;
			}
			else {
				uploading += 1;
			}
		}
		else {
			GetUserRec(client->m_up_down_queue_user).idle++;
		}
	}


		// schedule new transfers
	while( uploading < m_max_uploads || m_max_uploads <= 0 ||
		   downloading < m_max_downloads || m_max_downloads <= 0 )
	{
		TransferQueueRequest *best_client = NULL;
		int best_recency = 0;
		unsigned int best_running_count = 0;

		m_xfer_queue.Rewind();
		while( m_xfer_queue.Next(client) ) {
			if( client->m_gave_go_ahead ) {
				continue;
			}
			if( (client->m_downloading && 
				(downloading < m_max_downloads || m_max_downloads <= 0)) ||
				((!client->m_downloading) &&
				(uploading < m_max_uploads || m_max_uploads <= 0)) )
			{
				TransferQueueUser &this_user = GetUserRec(client->m_up_down_queue_user);
				unsigned int this_user_active_count = this_user.running;
				int this_user_recency = this_user.recency;

				bool this_client_is_better = false;
				if( !best_client ) {
					this_client_is_better = true;
				}
				else if( best_client->m_downloading != client->m_downloading ) {
						// effectively treat up/down queues independently
					if( client->m_downloading ) {
						this_client_is_better = true;
					}
				}
				else if( best_running_count > this_user_active_count ) {
						// prefer users with fewer active transfers
						// (only counting transfers in one direction for this comparison)
					this_client_is_better = true;
				}
				else if( best_recency > this_user_recency ) {
						// if still tied: round robin
					this_client_is_better = true;
				}

				if( this_client_is_better ) {
					best_client = client;
					best_running_count = this_user_active_count;
					best_recency = this_user_recency;
				}
			}
		}

		client = best_client;
		if( !client ) {
			break;
		}

		dprintf(D_FULLDEBUG,
				"TransferQueueManager: sending GoAhead to %s.\n",
				client->Description() );

		if( !client->SendGoAhead() ) {
			dprintf(D_FULLDEBUG,
					"TransferQueueManager: failed to send GoAhead; "
					"dequeueing %s.\n",
					client->Description() );

			delete client;
			m_xfer_queue.Delete(client);

			TransferQueueChanged();
		}
		else {
			SetRoundRobinRecency(client->m_up_down_queue_user);
			TransferQueueUser &user = GetUserRec(client->m_up_down_queue_user);
			user.running += 1;
			user.idle -= 1;
			if( client->m_downloading ) {
				downloading += 1;
			}
			else {
				uploading += 1;
			}
		}
	}


		// now that we have finished scheduling new transfers,
		// examine requests that are still waiting
	m_xfer_queue.Rewind();
	while( m_xfer_queue.Next(client) ) {
		if( !client->m_gave_go_ahead
			&& ( (client->m_downloading && downloading==0)
 			     || (!client->m_downloading && uploading==0) ) )
		{
				// This request has not been granted, but no requests
				// are active.  This shouldn't happen for simple
				// upload/download requests, but for future generality, we
				// check for this case and treat it appropriately.
			dprintf(D_ALWAYS,"TransferQueueManager: forcibly "
					"dequeueing entry for %s, "
					"because it is not allowed by the queue policy.\n",
					client->Description());
			delete client;
			m_xfer_queue.DeleteCurrent();
			TransferQueueChanged();
			continue;
		}

		if( !client->m_gave_go_ahead ) {
			clients_waiting = true;

			int age = time(NULL) - client->m_time_born;
			if( client->m_downloading ) {
				m_waiting_to_download++;
				if( age > m_download_wait_time ) {
					m_download_wait_time = age;
				}
			}
			else {
				m_waiting_to_upload++;
				if( age > m_upload_wait_time ) {
					m_upload_wait_time = age;
				}
			}
		}
	}

	m_uploading = uploading;
	m_downloading = downloading;


	if( clients_waiting ) {
			// queue is full; check for ancient clients
		m_xfer_queue.Rewind();
		while( m_xfer_queue.Next(client) ) {
			if( client->m_gave_go_ahead ) {
				int age = time(NULL) - client->m_time_go_ahead;
				int max_queue_age = client->m_max_queue_age;
				if( max_queue_age > 0 && max_queue_age < age ) {
						// Killing this client will not stop the current
						// file that is being transfered by it (which
						// presumably has stalled for some reason).  However,
						// it should prevent any additional files in the
						// sandbox from being transferred.
					dprintf(D_ALWAYS,"TransferQueueManager: forcibly "
							"dequeueing  ancient (%ds old) entry for %s, "
							"because it is older than "
							"MAX_TRANSFER_QUEUE_AGE=%ds.\n",
							age,
							client->Description(),
							max_queue_age);


					FILE *email = email_admin_open("file transfer took too long");
					if( !email ) {
							// Error sending the message
						dprintf( D_ALWAYS, 
								 "ERROR: Can't send email to the Condor "
								 "Administrator\n" );
					} else {
						fprintf( email,
								 "A file transfer for\n%s\ntook longer than MAX_TRANSFER_QUEUE_AGE=%ds,\n"
								 "so this transfer is being removed from the transfer queue,\n"
								 "which will abort further transfers for this attempt to run this job.\n\n"
								 "To avoid this timeout, MAX_TRANSFER_QUEUE_AGE may be increased,\n"
								 "but be aware that transfers which take a long time will delay other\n"
								 "transfers from starting if the maximum number of concurrent transfers\n"
								 "is exceeded.  Therefore, it is advisable to also review the settings\n"
								 "of MAX_CONCURRENT_UPLOADS and/or MAX_CONCURRENT_DOWNLOADS.\n\n"
								 "The transfer queue currently has %d/%d uploads,\n"
								 "%d/%d downloads, %d transfers waiting %ds to upload,\n"
								 "and %d transfers waiting %ds to download.\n",
								 client->Description(),
								 max_queue_age,
								 m_uploading,
								 m_max_uploads,
								 m_downloading,
								 m_max_downloads,
								 m_waiting_to_upload,
								 m_upload_wait_time,
								 m_waiting_to_download,
								 m_download_wait_time
								 );

						email_close ( email );
					}

					delete client;
					m_xfer_queue.DeleteCurrent();
					TransferQueueChanged();
						// Only delete more ancient clients if the
						// next pass of this function finds there is pressure
						// on the queue.
					break;
				}
			}
		}
	}
}

bool
TransferQueueManager::GetContactInfo(char const *command_sock_addr, std::string &contact_str)
{
	TransferQueueContactInfo contact(
		command_sock_addr,
		m_max_uploads == 0,
		m_max_downloads == 0);
	return contact.GetStringRepresentation(contact_str);
}

void
IOStats::Add(IOStats &s) {
	bytes_sent += s.bytes_sent.value;
	bytes_received += s.bytes_received.value;
	file_read += s.file_read.value;
	file_write += s.file_write.value;
	net_read += s.net_read.value;
	net_write += s.net_write.value;
}

void
IOStats::ConfigureEMAHorizons(classy_counted_ptr<stats_ema_config> config) {
	bytes_sent.ConfigureEMAHorizons(config);
	bytes_received.ConfigureEMAHorizons(config);
	file_read.ConfigureEMAHorizons(config);
	file_write.ConfigureEMAHorizons(config);
	net_read.ConfigureEMAHorizons(config);
	net_write.ConfigureEMAHorizons(config);
}

void
TransferQueueManager::AddRecentIOStats(IOStats &s,const std::string up_down_queue_user)
{
	m_iostats.Add(s);
	GetUserRec(up_down_queue_user).iostats.Add(s);
}

void
TransferQueueManager::UpdateIOStats()
{
	m_uploading_stat = m_uploading;
	m_downloading_stat = m_downloading;
	m_waiting_to_upload_stat = m_waiting_to_upload;
	m_waiting_to_download_stat = m_waiting_to_download;
	m_upload_wait_time_stat = m_upload_wait_time;
	m_download_wait_time_stat = m_download_wait_time;

	m_stat_pool.Advance(1);
}

void
TransferQueueManager::publish(ClassAd *ad, char const *publish_config)
{
	int publish_flags = m_publish_flags;
	if (publish_config && publish_config[0]) {
		publish_flags = generic_stats_ParseConfigString(publish_config, "TRANSFER", "TRANSFER", publish_flags);
	}
	publish(ad,publish_flags);
}

void
TransferQueueManager::publish(ClassAd *ad)
{
	publish(ad,m_publish_flags);
}

void
TransferQueueManager::publish(ClassAd *ad,int pubflags)
{
	dprintf(D_ALWAYS,"TransferQueueManager stats: active up=%d/%d down=%d/%d; waiting up=%d down=%d; wait time up=%ds down=%ds\n",
			m_uploading,
			m_max_uploads,
			m_downloading,
			m_max_downloads,
			m_waiting_to_upload,
			m_waiting_to_download,
			m_upload_wait_time,
			m_download_wait_time);

	char const *ema_horizon = m_iostats.bytes_sent.ShortestHorizonEMARateName();
	if( ema_horizon ) {
		dprintf(D_ALWAYS,"TransferQueueManager upload %s I/O load: %.0f bytes/s  %.3f disk load  %.3f net load\n",
				ema_horizon,
				m_iostats.bytes_sent.EMARate(ema_horizon),
				m_iostats.file_read.EMARate(ema_horizon),
				m_iostats.net_write.EMARate(ema_horizon));

		dprintf(D_ALWAYS,"TransferQueueManager download %s I/O load: %.0f bytes/s  %.3f disk load  %.3f net load\n",
				ema_horizon,
				m_iostats.bytes_received.EMARate(ema_horizon),
				m_iostats.file_write.EMARate(ema_horizon),
				m_iostats.net_read.EMARate(ema_horizon));
	}

	ad->Assign(ATTR_TRANSFER_QUEUE_MAX_UPLOADING,m_max_uploads);
	ad->Assign(ATTR_TRANSFER_QUEUE_MAX_DOWNLOADING,m_max_downloads);

	m_stat_pool.Publish(*ad,pubflags);

	CollectUserRecGarbage(ad);
}
