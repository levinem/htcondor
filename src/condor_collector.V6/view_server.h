/***************************Copyright-DO-NOT-REMOVE-THIS-LINE**
  *
  * Condor Software Copyright Notice
  * Copyright (C) 1990-2006, Condor Team, Computer Sciences Department,
  * University of Wisconsin-Madison, WI.
  *
  * This source code is covered by the Condor Public License, which can
  * be found in the accompanying LICENSE.TXT file, or online at
  * www.condorproject.org.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  * AND THE UNIVERSITY OF WISCONSIN-MADISON "AS IS" AND ANY EXPRESS OR
  * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  * WARRANTIES OF MERCHANTABILITY, OF SATISFACTORY QUALITY, AND FITNESS
  * FOR A PARTICULAR PURPOSE OR USE ARE DISCLAIMED. THE COPYRIGHT
  * HOLDERS AND CONTRIBUTORS AND THE UNIVERSITY OF WISCONSIN-MADISON
  * MAKE NO MAKE NO REPRESENTATION THAT THE SOFTWARE, MODIFICATIONS,
  * ENHANCEMENTS OR DERIVATIVE WORKS THEREOF, WILL NOT INFRINGE ANY
  * PATENT, COPYRIGHT, TRADEMARK, TRADE SECRET OR OTHER PROPRIETARY
  * RIGHT.
  *
  ****************************Copyright-DO-NOT-REMOVE-THIS-LINE**/
#ifndef _VIEW_SERVER_H_
#define _VIEW_SERVER_H_

//---------------------------------------------------

#include "collector.h"
#include "Set.h"
#include "HashTable.h"

//---------------------------------------------------

// Note: This should be kept in sync with condor_state.h,  and with
// StartdScanFunc() of view_server.C, and with
// ResConvStr() of condor_tools/stats.C .
// Also, make sure that you keep VIEW_STATE_MAX pointing at
// the last "normal" VIEW_STATE
typedef enum {
	VIEW_STATE_UNDEFINED = 0,
	VIEW_STATE_UNCLAIMED = 1,
	VIEW_STATE_MATCHED = 2,
	VIEW_STATE_CLAIMED = 3,
	VIEW_STATE_PREEMPTING = 4,
	VIEW_STATE_OWNER = 5,
	VIEW_STATE_SHUTDOWN = 6,
	VIEW_STATE_DELETE = 7,
	VIEW_STATE_BACKFILL = 8,

	VIEW_STATE_MAX = VIEW_STATE_BACKFILL,
	VIEW_STATE_MAX_OFFSET = VIEW_STATE_MAX - 1,
} ViewStates;
struct GeneralRecord {
	float Data[VIEW_STATE_MAX];
	GeneralRecord() {
		for( int i=0; i<(int)VIEW_STATE_MAX_OFFSET; i++ ) {
			Data[i] = 0.0;
		}
	};
};

//---------------------------------------------------

typedef HashTable<MyString, GeneralRecord*> AccHash;

//---------------------------------------------------

struct DataSetInfo {
	MyString OldFileName, NewFileName;
	int OldStartTime, NewStartTime;
	int NumSamples, MaxSamples;
	AccHash* AccData;
};

//---------------------------------------------------

class ViewServer : public CollectorDaemon {

public:

	ViewServer();			 // constructor

	void Init();             // main_init
	void Config();           // main_config
	void Exit();             // main__shutdown_fast
	void Shutdown();         // main_shutdown_graceful

	static int ReceiveHistoryQuery(Service*, int, Stream*);
	static int HandleQuery(Stream*, int cmd, int FromDate, int ToDate, int Options, MyString Arg);
	static int SendListReply(Stream*,const MyString& FileName, int FromDate, int ToDatei, Set<MyString>& Names);
	static int SendDataReply(Stream*,const MyString& FileName, int FromDate, int ToDate, int Options, const MyString& Arg);

	static void WriteHistory();
	static int SubmittorScanFunc(ClassAd* ad);
	static int SubmittorTotalFunc(void);
	static int StartdScanFunc(ClassAd* ad);
	static int StartdTotalFunc(void);
	static int CkptScanFunc(ClassAd* ad);

	static int HashFunc(const MyString& Key, int TableSize);

private:

	// Configuration variables

	static int HistoryInterval;
	static int MaxFileSize;

	// Constants

	enum { HistoryLevels=3 };
	enum { SubmittorData, StartdData, GroupsData, SubmittorGroupsData, CkptData, DataSetCount };
	const int MaxGroups;

	// State variables - data set information

	static DataSetInfo DataSet[DataSetCount][HistoryLevels];
	static MyString DataFormat[DataSetCount];
	
	// Variables used during iteration

	static int TimeStamp;
	static AccHash* GroupHash;

	static GeneralRecord* GetAccData(AccHash* AccData,const MyString& Key);

	// misc variables

	static int HistoryTimer;
	static int KeepHistory;

	// Utility functions

	static int ReadTime(char* Line);
	static int ReadTimeAndName(char* Line, MyString& Name);
	static int ReadTimeChkName(char* Line, const MyString& Name);
	static int FindFileStartTime(const char *Name);
};


#endif
