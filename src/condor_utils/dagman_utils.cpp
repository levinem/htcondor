/***************************************************************
 *
 * Copyright (C) 1990-2018, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *	  http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_arglist.h"
#include "condor_attributes.h"
#include "condor_getcwd.h"
#include "condor_version.h"
#include "dagman_utils.h"
#include "my_popen.h"
#include "MyString.h"
#include "read_multiple_logs.h"
#include "tmp_dir.h"
#include "which.h"


static void
AppendError(MyString &errMsg, const MyString &newError)
{
	if ( errMsg != "" ) errMsg += "; ";
	errMsg += newError;
}

bool
EnvFilter::ImportFilter( const MyString &var, const MyString &val ) const
{
	if ( (var.find(";") >= 0) || (val.find(";") >= 0) ) {
		return false;
	}
	return IsSafeEnvV2Value( val.Value() );
}

bool 
DagmanUtils::writeSubmitFile( /* const */ SubmitDagDeepOptions &deepOpts,
			/* const */ SubmitDagShallowOptions &shallowOpts,
			/* const */ std::list<std::string> &dagFileAttrLines ) const
{
	FILE *pSubFile = safe_fopen_wrapper_follow(shallowOpts.strSubFile.Value(), "w");
	if (!pSubFile)
	{
		fprintf( stderr, "ERROR: unable to create submit file %s\n",
				 shallowOpts.strSubFile.Value() );
		return false;
	}

	const char *executable = NULL;
	MyString valgrindPath; // outside if so executable is valid!
	if ( shallowOpts.runValgrind ) {
		valgrindPath = which( valgrind_exe );
		if ( valgrindPath == "" ) {
			fprintf( stderr, "ERROR: can't find %s in PATH, aborting.\n",
						 valgrind_exe );
			fclose(pSubFile);
			return false;
		} else {
			executable = valgrindPath.Value();
		}
	} else {
		executable = deepOpts.strDagmanPath.Value();
	}

	fprintf(pSubFile, "# Filename: %s\n", shallowOpts.strSubFile.Value());

	fprintf(pSubFile, "# Generated by condor_submit_dag ");
	for (auto it = shallowOpts.dagFiles.begin(); it != shallowOpts.dagFiles.end(); ++it) {
		fprintf(pSubFile, "%s ", it->c_str());
	}
	fprintf(pSubFile, "\n");

	fprintf(pSubFile, "universe\t= scheduler\n");
	fprintf(pSubFile, "executable\t= %s\n", executable);
	fprintf(pSubFile, "getenv\t\t= True\n");
	fprintf(pSubFile, "output\t\t= %s\n", shallowOpts.strLibOut.Value());
	fprintf(pSubFile, "error\t\t= %s\n", shallowOpts.strLibErr.Value());
	fprintf(pSubFile, "log\t\t= %s\n", shallowOpts.strSchedLog.Value());
	if ( ! deepOpts.batchName.empty() ) {
		fprintf(pSubFile, "+%s\t= \"%s\"\n", ATTR_JOB_BATCH_NAME,
					deepOpts.batchName.c_str());
	}
	if ( ! deepOpts.batchId.empty() ) {
		fprintf(pSubFile, "+%s\t= \"%s\"\n", ATTR_JOB_BATCH_ID,
					deepOpts.batchId.c_str());
	}
#if !defined ( WIN32 )
	fprintf(pSubFile, "remove_kill_sig\t= SIGUSR1\n" );
#endif
	fprintf(pSubFile, "+%s\t= \"%s =?= $(cluster)\"\n",
				ATTR_OTHER_JOB_REMOVE_REQUIREMENTS, ATTR_DAGMAN_JOB_ID );

		// ensure DAGMan is automatically requeued by the schedd if it
		// exits abnormally or is killed (e.g., during a reboot)
	const char *defaultRemoveExpr = "( ExitSignal =?= 11 || "
				"(ExitCode =!= UNDEFINED && ExitCode >=0 && ExitCode <= 2))";
	MyString removeExpr(defaultRemoveExpr);
	char *tmpRemoveExpr = param( "DAGMAN_ON_EXIT_REMOVE" );
	if ( tmpRemoveExpr ) {
		removeExpr = tmpRemoveExpr;
		free(tmpRemoveExpr);
	}
	fprintf(pSubFile, "# Note: default on_exit_remove expression:\n");
	fprintf(pSubFile, "# %s\n", defaultRemoveExpr);
	fprintf(pSubFile, "# attempts to ensure that DAGMan is automatically\n");
	fprintf(pSubFile, "# requeued by the schedd if it exits abnormally or\n");
	fprintf(pSubFile, "# is killed (e.g., during a reboot).\n");
	fprintf(pSubFile, "on_exit_remove\t= %s\n", removeExpr.Value() );

	if (!usingPythonBindings) {
		fprintf(pSubFile, "copy_to_spool\t= %s\n", shallowOpts.copyToSpool ?
				"True" : "False" );
	}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Be sure to change MIN_SUBMIT_FILE_VERSION in dagman_main.cpp
	// if the arguments passed to condor_dagman change in an
	// incompatible way!!
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	ArgList args;

	if ( shallowOpts.runValgrind ) {
		args.AppendArg("--tool=memcheck");
		args.AppendArg("--leak-check=yes");
		args.AppendArg("--show-reachable=yes");
		args.AppendArg(deepOpts.strDagmanPath.Value());
	}

		// -p 0 causes DAGMan to run w/o a command socket (see gittrac #4987).
	args.AppendArg("-p");
	args.AppendArg("0");
	args.AppendArg("-f");
	args.AppendArg("-l");
	args.AppendArg(".");
	if ( shallowOpts.iDebugLevel != DEBUG_UNSET ) {
		args.AppendArg("-Debug");
		args.AppendArg(shallowOpts.iDebugLevel);
	}
	args.AppendArg("-Lockfile");
	args.AppendArg(shallowOpts.strLockFile.Value());
	args.AppendArg("-AutoRescue");
	args.AppendArg(deepOpts.autoRescue);
	args.AppendArg("-DoRescueFrom");
	args.AppendArg(deepOpts.doRescueFrom);

	for (auto it = shallowOpts.dagFiles.begin(); it != shallowOpts.dagFiles.end(); ++it) {
		args.AppendArg("-Dag");
		args.AppendArg(it->c_str());
	}

	if(shallowOpts.iMaxIdle != 0) 
	{
		args.AppendArg("-MaxIdle");
		args.AppendArg(shallowOpts.iMaxIdle);
	}

	if(shallowOpts.iMaxJobs != 0) 
	{
		args.AppendArg("-MaxJobs");
		args.AppendArg(shallowOpts.iMaxJobs);
	}

	if(shallowOpts.iMaxPre != 0) 
	{
		args.AppendArg("-MaxPre");
		args.AppendArg(shallowOpts.iMaxPre);
	}

	if(shallowOpts.iMaxPost != 0) 
	{
		args.AppendArg("-MaxPost");
		args.AppendArg(shallowOpts.iMaxPost);
	}

	if ( shallowOpts.bPostRunSet ) {
		if (shallowOpts.bPostRun) {
			args.AppendArg("-AlwaysRunPost");
		} else {
			args.AppendArg("-DontAlwaysRunPost");
		}
	}

	if(deepOpts.useDagDir)
	{
		args.AppendArg("-UseDagDir");
	}

	if(deepOpts.suppress_notification)
	{
		args.AppendArg("-Suppress_notification");
	}
	else
	{
		args.AppendArg("-Dont_Suppress_notification");
	}

	if ( shallowOpts.doRecovery ) {
		args.AppendArg( "-DoRecov" );
	}

	args.AppendArg("-CsdVersion");
	args.AppendArg(CondorVersion());

	if(deepOpts.allowVerMismatch) {
		args.AppendArg("-AllowVersionMismatch");
	}

	if(shallowOpts.dumpRescueDag) {
		args.AppendArg("-DumpRescue");
	}

	if(deepOpts.bVerbose) {
		args.AppendArg("-Verbose");
	}

	if(deepOpts.bForce) {
		args.AppendArg("-Force");
	}

	if(deepOpts.strNotification != "") {
		args.AppendArg("-Notification");
		args.AppendArg(deepOpts.strNotification);
	}

	if(deepOpts.strDagmanPath != "") {
		args.AppendArg("-Dagman");
		args.AppendArg(deepOpts.strDagmanPath);
	}

	if(deepOpts.strOutfileDir != "") {
		args.AppendArg("-Outfile_dir");
		args.AppendArg(deepOpts.strOutfileDir);
	}

	if(deepOpts.updateSubmit) {
		args.AppendArg("-Update_submit");
	}

	if(deepOpts.importEnv) {
		args.AppendArg("-Import_env");
	}

	if( shallowOpts.priority != 0 ) {
		args.AppendArg("-Priority");
		args.AppendArg(shallowOpts.priority);
	}

	MyString arg_str,args_error;
	if(!args.GetArgsStringV1WackedOrV2Quoted(&arg_str,&args_error)) {
		fprintf(stderr,"Failed to insert arguments: %s",args_error.Value());
		exit(1);
	}
	fprintf(pSubFile, "arguments\t= %s\n", arg_str.Value());

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Be sure to change MIN_SUBMIT_FILE_VERSION in dagman_main.cpp
	// if the environment passed to condor_dagman changes in an
	// incompatible way!!
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	EnvFilter env;
	if ( deepOpts.importEnv ) {
		env.Import( );
	}
	env.SetEnv("_CONDOR_DAGMAN_LOG", shallowOpts.strDebugLog.Value());
	env.SetEnv("_CONDOR_MAX_DAGMAN_LOG=0");
	if ( shallowOpts.strScheddDaemonAdFile != "" ) {
		env.SetEnv("_CONDOR_SCHEDD_DAEMON_AD_FILE",
				   shallowOpts.strScheddDaemonAdFile.Value());
	}
	if ( shallowOpts.strScheddAddressFile != "" ) {
		env.SetEnv("_CONDOR_SCHEDD_ADDRESS_FILE",
				   shallowOpts.strScheddAddressFile.Value());
	}
	if ( shallowOpts.strConfigFile != "" ) {
		if ( access( shallowOpts.strConfigFile.Value(), F_OK ) != 0 ) {
			fprintf( stderr, "ERROR: unable to read config file %s "
						"(error %d, %s)\n",
						shallowOpts.strConfigFile.Value(), errno, strerror(errno) );
			fclose(pSubFile);
			return false;
		}
		env.SetEnv("_CONDOR_DAGMAN_CONFIG_FILE", shallowOpts.strConfigFile.Value());
	}

	MyString env_str;
	MyString env_errors;
	if ( !env.getDelimitedStringV1RawOrV2Quoted( &env_str, &env_errors ) ) {
		fprintf( stderr,"Failed to insert environment: %s",
					env_errors.Value() );
		fclose(pSubFile);
		return false;
	}
	fprintf(pSubFile, "environment\t= %s\n",env_str.Value());

	if ( deepOpts.strNotification != "" ) {    
		fprintf( pSubFile, "notification\t= %s\n",
					deepOpts.strNotification.Value() );
	}

		// Append user-specified stuff to submit file...

		// ...first, the insert file, if any...
	if ( shallowOpts.appendFile != "" ) {
		FILE *aFile = safe_fopen_wrapper_follow(
					shallowOpts.appendFile.Value(), "r");
		if ( !aFile ) {
			fprintf( stderr, "ERROR: unable to read submit append file (%s)\n",
					 shallowOpts.appendFile.Value() );
			return false;
		}

		char *line;
		int lineno = 0;
		while ( (line = getline_trim( aFile, lineno )) != NULL ) {
			fprintf(pSubFile, "%s\n", line);
		}

		fclose( aFile );
	}

		// ...now append lines specified in the DAG file...
	for (auto it = dagFileAttrLines.begin(); it != dagFileAttrLines.end(); ++it) {
			// Note:  prepending "+" here means that this only works
			// for setting ClassAd attributes.
		fprintf( pSubFile, "+%s\n", it->c_str() );
	}

		// ...now things specified directly on the command line.
	for (auto it = shallowOpts.appendLines.begin(); it != shallowOpts.appendLines.end(); ++it) {
		fprintf( pSubFile, "%s\n", it->c_str() );
	}

	fprintf(pSubFile, "queue\n");

	fclose(pSubFile);

	return true;
}

/** Run condor_submit_dag on the given DAG file.
@param opts: the condor_submit_dag options
@param dagFile: the DAG file to process
@param directory: the directory from which the DAG file should
	be processed (ignored if NULL)
@param priority: the priority of this DAG
@param isRetry: whether this is a retry
@return 0 if successful, 1 if failed
*/
int
DagmanUtils::runSubmitDag( const SubmitDagDeepOptions &deepOpts,
			const char *dagFile, const char *directory, int priority,
			bool isRetry )
{
	int result = 0;

		// Change to the appropriate directory if necessary.
	TmpDir tmpDir;
	MyString errMsg;
	if ( directory ) {
		if ( !tmpDir.Cd2TmpDir( directory, errMsg ) ) {
			fprintf( stderr, "Error (%s) changing to node directory\n",
						errMsg.Value() );
			result = 1;
			return result;
		}
	}

		// Build up the command line for the recursive run of
		// condor_submit_dag.  We need -no_submit so we don't
		// actually run the subdag now; we need -update_submit
		// so the lower-level .condor.sub file will get
		// updated, in case it came from an earlier version
		// of condor_submit_dag.
	ArgList args;
	args.AppendArg( "condor_submit_dag" );
	args.AppendArg( "-no_submit" );
	args.AppendArg( "-update_submit" );

		// Add in arguments we're passing along.
	if ( deepOpts.bVerbose ) {
		args.AppendArg( "-verbose" );
	}

	if ( deepOpts.bForce && !isRetry ) {
		args.AppendArg( "-force" );
	}

	if (deepOpts.strNotification != "" ) {
		args.AppendArg( "-notification" );
		if(deepOpts.suppress_notification) {
			args.AppendArg( "never" );
		} else { 
			args.AppendArg( deepOpts.strNotification.Value() );
		}
	}

	if ( deepOpts.strDagmanPath != "" ) {
		args.AppendArg( "-dagman" );
		args.AppendArg( deepOpts.strDagmanPath.Value() );
	}

	if ( deepOpts.useDagDir ) {
		args.AppendArg( "-usedagdir" );
	}

	if ( deepOpts.strOutfileDir != "" ) {
		args.AppendArg( "-outfile_dir" );
		args.AppendArg( deepOpts.strOutfileDir.Value() );
	}

	args.AppendArg( "-autorescue" );
	args.AppendArg( deepOpts.autoRescue );

	if ( deepOpts.doRescueFrom != 0 ) {
		args.AppendArg( "-dorescuefrom" );
		args.AppendArg( deepOpts.doRescueFrom );
	}

	if ( deepOpts.allowVerMismatch ) {
		args.AppendArg( "-allowver" );
	}

	if ( deepOpts.importEnv ) {
		args.AppendArg( "-import_env" );
	}

	if ( deepOpts.recurse ) {
		args.AppendArg( "-do_recurse" );
	}

	if ( deepOpts.updateSubmit ) {
		args.AppendArg( "-update_submit" );
	}

	if( priority != 0) {
		args.AppendArg( "-Priority" );
		args.AppendArg( priority );
	}

	if( deepOpts.suppress_notification ) {
		args.AppendArg( "-suppress_notification" );
	} else {
		args.AppendArg( "-dont_suppress_notification" );
	}

	args.AppendArg( dagFile );

	MyString cmdLine;
	args.GetArgsStringForDisplay( &cmdLine );
	dprintf( D_ALWAYS, "Recursive submit command: <%s>\n",
				cmdLine.Value() );

		// Now actually run the command.
	int retval = my_system( args );
	if ( retval != 0 ) {
		dprintf( D_ALWAYS, "ERROR: condor_submit_dag -no_submit "
					"failed on DAG file %s.\n", dagFile );
		result = 1;
	}

		// Change back to the directory we started from.
	if ( !tmpDir.Cd2MainDir( errMsg ) ) {
		dprintf( D_ALWAYS, "Error (%s) changing back to original directory\n",
					errMsg.Value() );
	}

	return result;
}

//---------------------------------------------------------------------------
/** Set up things in deep and shallow options that aren't directly specified
	on the command line.
	@param deepOpts: the condor_submit_dag deep options
	@param shallowOpts: the condor_submit_dag shallow options
	@return 0 if successful, 1 if failed
*/
int
DagmanUtils::setUpOptions( SubmitDagDeepOptions &deepOpts,
			SubmitDagShallowOptions &shallowOpts,
			std::list<std::string> &dagFileAttrLines )
{
	shallowOpts.strLibOut = shallowOpts.primaryDagFile + ".lib.out";
	shallowOpts.strLibErr = shallowOpts.primaryDagFile + ".lib.err";

	if ( deepOpts.strOutfileDir != "" ) {
		shallowOpts.strDebugLog = deepOpts.strOutfileDir + DIR_DELIM_STRING +
					condor_basename( shallowOpts.primaryDagFile.Value() );
	} else {
		shallowOpts.strDebugLog = shallowOpts.primaryDagFile;
	}
	shallowOpts.strDebugLog += ".dagman.out";
	shallowOpts.strSchedLog = shallowOpts.primaryDagFile + ".dagman.log";
	shallowOpts.strSubFile = shallowOpts.primaryDagFile + DAG_SUBMIT_FILE_SUFFIX;

	MyString	rescueDagBase;

		// If we're running each DAG in its own directory, write any rescue
		// DAG to the current directory, to avoid confusion (since the
		// rescue DAG must be run from the current directory).
	if ( deepOpts.useDagDir ) {
		if ( !condor_getcwd( rescueDagBase ) ) {
			fprintf( stderr, "ERROR: unable to get cwd: %d, %s\n",
					errno, strerror(errno) );
			return 1;
		}
		rescueDagBase += DIR_DELIM_STRING;
		rescueDagBase += condor_basename(shallowOpts.primaryDagFile.Value());
	} else {
		rescueDagBase = shallowOpts.primaryDagFile;
	}

		// If we're running multiple DAGs, put "_multi" in the rescue
		// DAG name to indicate that the rescue DAG is for *all* of
		// the DAGs we're running.
	if ( shallowOpts.dagFiles.size() > 1 ) {
		rescueDagBase += "_multi";
	}
	shallowOpts.strRescueFile = rescueDagBase + ".rescue";

	shallowOpts.strLockFile = shallowOpts.primaryDagFile + ".lock";

	if (deepOpts.strDagmanPath == "" ) {
		deepOpts.strDagmanPath = which( dagman_exe );
	}

	if (deepOpts.strDagmanPath == "")
	{
		fprintf( stderr, "ERROR: can't find %s in PATH, aborting.\n",
				 dagman_exe );
		return 1;
	}
	MyString	msg;
	if ( !GetConfigAndAttrs( shallowOpts.dagFiles, deepOpts.useDagDir,
				shallowOpts.strConfigFile,
				dagFileAttrLines, msg) ) {
		fprintf( stderr, "ERROR: %s\n", msg.Value() );
		return 1;
	}

	return 0;
}

/** Get the configuration file (if any) and the submit append commands
	(if any), specified by the given list of DAG files.  If more than one
	DAG file specifies a configuration file, they must specify the same file.
	@param dagFiles: the list of DAG files
	@param useDagDir: run DAGs in directories from DAG file paths 
			if true
	@param configFile: holds the path to the config file; if a config
			file is specified on the command line, configFile should
			be set to that value before this method is called; the
			value of configFile will be changed if it's not already
			set and the DAG file(s) specify a configuration file
	@param attrLines: a std::list<std::string> to receive the submit attributes
	@param errMsg: a MyString to receive any error message
	@return true if the operation succeeded; otherwise false
*/
bool
DagmanUtils::GetConfigAndAttrs( /* const */ std::list<std::string> &dagFiles, bool useDagDir, 
			MyString &configFile, std::list<std::string> &attrLines, MyString &errMsg )
{
	bool		result = true;
		// Note: destructor will change back to original directory.
	TmpDir		  dagDir;

	for (auto dagfile_it = dagFiles.begin(); dagfile_it != dagFiles.end(); ++dagfile_it) {

			//
			// Change to the DAG file's directory if necessary, and
			// get the filename we need to use for it from that directory.
			//
		const char *	newDagFile;
		if ( useDagDir ) {
			MyString	tmpErrMsg;
			if ( !dagDir.Cd2TmpDirFile( dagfile_it->c_str(), tmpErrMsg ) ) {
				AppendError( errMsg,
						MyString("Unable to change to DAG directory ") +
						tmpErrMsg );
				return false;
			}
			newDagFile = condor_basename( dagfile_it->c_str() );
		} else {
			newDagFile = dagfile_it->c_str();
		}

		std::list<std::string> configFiles;
			// Note: destructor will close file.
		MultiLogFiles::FileReader reader;
		errMsg = reader.Open( newDagFile );
		if ( errMsg != "" ) {
			return false;
		}

		MyString logicalLine;
		while ( reader.NextLogicalLine( logicalLine ) ) {
			if ( logicalLine != "" ) {

				// Initialize list of tokens from logicalLine
				std::list<std::string> tokens;
				MyStringTokener tok;
				logicalLine.trim();
				tok.Tokenize(logicalLine.Value());
				while( const char* token = tok.GetNextToken(" \t", true) ) {
					tokens.push_back(std::string(token));
				}

				const char *firstToken = tokens.front().c_str();
				if ( !strcasecmp( firstToken, "config" ) ) {

						// Get the value, which is the next token in the list
					tokens.pop_front();
					const char *newValue = tokens.front().c_str();
					if ( !newValue || !strcmp( newValue, "" ) ) {
						AppendError( errMsg, "Improperly-formatted "
									"file: value missing after keyword "
									"CONFIG" );
						result = false;
					} else {

							// Add the value we just found to the config
							// files list (if it's not already in the
							// list -- we don't want duplicates).
						bool alreadyInList = false;
						for (auto configfile_it = configFiles.begin(); configfile_it != configFiles.end(); ++configfile_it) {
							if ( !strcmp( configfile_it->c_str(), newValue ) ) {
								alreadyInList = true;
							}
						}

						if ( !alreadyInList ) {
								// Note: append copies the string here.
							configFiles.push_back( newValue );
						}
					}

					//some DAG commands are needed for condor_submit_dag, too...
				} else if ( !strcasecmp( firstToken, "SET_JOB_ATTR" ) ) {
						// Strip of DAGMan-specific command name; the
						// rest we pass to the submit file.
					logicalLine.replaceString( "SET_JOB_ATTR", "" );
					logicalLine.trim();
					if ( logicalLine == "" ) {
						AppendError( errMsg, "Improperly-formatted "
									"file: value missing after keyword "
									"SET_JOB_ATTR" );
						result = false;
					} else {
						attrLines.push_back( logicalLine.Value() );
					}
				}
			}
		}
	
		reader.Close();
			//
			// Check the specified config file(s) against whatever we
			// currently have, setting the config file if it hasn't
			// been set yet, flagging an error if config files conflict.
			//
		for (auto configfile_it = configFiles.begin(); configfile_it != configFiles.end(); ++configfile_it) {
			MyString cfgFileMS = configfile_it->c_str();
			MyString tmpErrMsg;
			if ( MakePathAbsolute( cfgFileMS, tmpErrMsg ) ) {
				if ( configFile == "" ) {
					configFile = cfgFileMS;
				} else if ( configFile != cfgFileMS ) {
					AppendError( errMsg, MyString("Conflicting DAGMan ") +
								"config files specified: " + configFile +
								" and " + cfgFileMS );
					result = false;
				}
			} else {
				AppendError( errMsg, tmpErrMsg );
				result = false;
			}
		}

			//
			// Go back to our original directory.
			//
		MyString tmpErrMsg;
		if ( !dagDir.Cd2MainDir( tmpErrMsg ) ) {
			AppendError( errMsg,
					MyString("Unable to change to original directory ") +
					tmpErrMsg );
			result = false;
		}
	}

	return result;
}

/** Make the given path into an absolute path, if it is not already.
	@param filePath: the path to make absolute (filePath is changed)
	@param errMsg: a MyString to receive any error message.
	@return true if the operation succeeded; otherwise false
*/
bool
DagmanUtils::MakePathAbsolute(MyString &filePath, MyString &errMsg)
{
	bool result = true;

	if ( !fullpath( filePath.Value() ) ) {
		MyString currentDir;
		if ( !condor_getcwd( currentDir ) ) {
			formatstr( errMsg, "condor_getcwd() failed with errno %d (%s) at %s:%d",
					   errno, strerror(errno), __FILE__, __LINE__ );
			result = false;
		}

		filePath = currentDir + DIR_DELIM_STRING + filePath;
	}

	return result;
}

/** Finds the number of the last existing rescue DAG file for the
	given "primary" DAG.
	@param primaryDagFile The primary DAG file name
	@param multiDags Whether we have multiple DAGs
	@param maxRescueDagNum the maximum legal rescue DAG number
	@return The number of the last existing rescue DAG (0 if there
		is none)
*/
int
DagmanUtils::FindLastRescueDagNum( const char *primaryDagFile, bool multiDags,
			int maxRescueDagNum )
{
	int lastRescue = 0;

	for ( int test = 1; test <= maxRescueDagNum; test++ ) {
		MyString testName = RescueDagName( primaryDagFile, multiDags,
					test );
		if ( access( testName.Value(), F_OK ) == 0 ) {
			if ( test > lastRescue + 1 ) {
					// This should probably be a fatal error if
					// DAGMAN_USE_STRICT is set, but I'm avoiding
					// that for now because the fact that this code
					// is used in both condor_dagman and condor_submit_dag
					// makes that harder to implement. wenger 2011-01-28
				dprintf( D_ALWAYS, "Warning: found rescue DAG "
							"number %d, but not rescue DAG number %d\n",
							test, test - 1);
			}
			lastRescue = test;
		}
	}
	
	if ( lastRescue >= maxRescueDagNum ) {
		dprintf( D_ALWAYS,
					"Warning: FindLastRescueDagNum() hit maximum "
					"rescue DAG number: %d\n", maxRescueDagNum );
	}

	return lastRescue;
}

/** Creates a rescue DAG name, given a primary DAG name and rescue
	DAG number
	@param primaryDagFile The primary DAG file name
	@param multiDags Whether we have multiple DAGs
	@param rescueDagNum The rescue DAG number
	@return The full name of the rescue DAG
*/
MyString
DagmanUtils::RescueDagName(const char *primaryDagFile, bool multiDags,
			int rescueDagNum)
{
	ASSERT( rescueDagNum >= 1 );

	MyString fileName(primaryDagFile);
	if ( multiDags ) {
		fileName += "_multi";
	}
	fileName += ".rescue";
	fileName.formatstr_cat( "%.3d", rescueDagNum );

	return fileName;
}

/** Renames all rescue DAG files for this primary DAG after the
	given one (as long as the numbers are contiguous).	For example,
	if rescueDagNum is 3, we will rename .rescue4, .rescue5, etc.
	@param primaryDagFile The primary DAG file name
	@param multiDags Whether we have multiple DAGs
	@param rescueDagNum The rescue DAG number to rename *after*
	@param maxRescueDagNum the maximum legal rescue DAG number
*/
void
DagmanUtils::RenameRescueDagsAfter(const char *primaryDagFile, bool multiDags,
			int rescueDagNum, int maxRescueDagNum)
{
		// Need to allow 0 here so condor_submit_dag -f can rename all
		// rescue DAGs.
	ASSERT( rescueDagNum >= 0 );

	dprintf( D_ALWAYS, "Renaming rescue DAGs newer than number %d\n",
				rescueDagNum );

	int firstToDelete = rescueDagNum + 1;
	int lastToDelete = FindLastRescueDagNum( primaryDagFile, multiDags,
				maxRescueDagNum );

	for ( int rescueNum = firstToDelete; rescueNum <= lastToDelete;
				rescueNum++ ) {
		MyString rescueDagName = RescueDagName( primaryDagFile, multiDags,
					rescueNum );
		dprintf( D_ALWAYS, "Renaming %s\n", rescueDagName.Value() );
		MyString newName = rescueDagName + ".old";
			// Unlink here to be safe on Windows.
		tolerant_unlink( newName.Value() );
		if ( rename( rescueDagName.Value(), newName.Value() ) != 0 ) {
			EXCEPT( "Fatal error: unable to rename old rescue file "
						"%s: error %d (%s)\n", rescueDagName.Value(),
						errno, strerror( errno ) );
		}
	}
}

/** Generates the halt file name based on the primary DAG name.
	@return The halt file name.
*/
MyString
DagmanUtils::HaltFileName( const MyString &primaryDagFile )
{
	MyString haltFile = primaryDagFile + ".halt";

	return haltFile;
}

/** Attempts to unlink the given file, and prints an appropriate error
	message if this fails (but doesn't return an error, so only call
	this if a failure of the unlink is okay).
	@param pathname The path of the file to unlink
*/
void
DagmanUtils::tolerant_unlink( const char *pathname )
{
	if ( unlink( pathname ) != 0 ) {
		if ( errno == ENOENT ) {
			dprintf( D_SYSCALLS,
						"Warning: failure (%d (%s)) attempting to unlink file %s\n",
						errno, strerror( errno ), pathname );
		} else {
			dprintf( D_ALWAYS,
						"Error (%d (%s)) attempting to unlink file %s\n",
						errno, strerror( errno ), pathname );

		}
	}
}

//---------------------------------------------------------------------------
bool 
DagmanUtils::fileExists(const MyString &strFile)
{
	int fd = safe_open_wrapper_follow(strFile.Value(), O_RDONLY);
	if (fd == -1)
		return false;
	close(fd);
	return true;
}

//---------------------------------------------------------------------------
bool 
DagmanUtils::ensureOutputFilesExist(const SubmitDagDeepOptions &deepOpts,
			SubmitDagShallowOptions &shallowOpts)
{
	int maxRescueDagNum = param_integer("DAGMAN_MAX_RESCUE_NUM",
				MAX_RESCUE_DAG_DEFAULT, 0, ABS_MAX_RESCUE_DAG_NUM);

	if (deepOpts.doRescueFrom > 0)
	{
		MyString rescueDagName = RescueDagName(shallowOpts.primaryDagFile.Value(),
				shallowOpts.dagFiles.size() > 1, deepOpts.doRescueFrom);
		if (!fileExists(rescueDagName))
		{
			fprintf( stderr, "-dorescuefrom %d specified, but rescue "
						"DAG file %s does not exist!\n", deepOpts.doRescueFrom,
						rescueDagName.Value() );
			return false;
		}
	}

		// Get rid of the halt file (if one exists).
	tolerant_unlink( HaltFileName( shallowOpts.primaryDagFile ).Value() );

	if (deepOpts.bForce)
	{
		tolerant_unlink(shallowOpts.strSubFile.Value());
		tolerant_unlink(shallowOpts.strSchedLog.Value());
		tolerant_unlink(shallowOpts.strLibOut.Value());
		tolerant_unlink(shallowOpts.strLibErr.Value());
		RenameRescueDagsAfter(shallowOpts.primaryDagFile.Value(),
					shallowOpts.dagFiles.size() > 1, 0, maxRescueDagNum);
	}

		// Check whether we're automatically running a rescue DAG -- if
		// so, allow things to continue even if the files generated
		// by condor_submit_dag already exist.
	bool autoRunningRescue = false;
	if (deepOpts.autoRescue) {
		int rescueDagNum = FindLastRescueDagNum(shallowOpts.primaryDagFile.Value(),
					shallowOpts.dagFiles.size() > 1, maxRescueDagNum);
		if (rescueDagNum > 0) {
			printf("Running rescue DAG %d\n", rescueDagNum);
			autoRunningRescue = true;
		}
	}

	bool bHadError = false;
		// If not running a rescue DAG, check for existing files
		// generated by condor_submit_dag...
	if (!autoRunningRescue && deepOpts.doRescueFrom < 1 && !deepOpts.updateSubmit) {
		if (fileExists(shallowOpts.strSubFile))
		{
			fprintf( stderr, "ERROR: \"%s\" already exists.\n",
					 shallowOpts.strSubFile.Value() );
			bHadError = true;
		}
		if (fileExists(shallowOpts.strLibOut))
		{
			fprintf( stderr, "ERROR: \"%s\" already exists.\n",
					 shallowOpts.strLibOut.Value() );
			bHadError = true;
		}
		if (fileExists(shallowOpts.strLibErr))
		{
			fprintf( stderr, "ERROR: \"%s\" already exists.\n",
					 shallowOpts.strLibErr.Value() );
			bHadError = true;
		}
		if (fileExists(shallowOpts.strSchedLog))
		{
			fprintf( stderr, "ERROR: \"%s\" already exists.\n",
					 shallowOpts.strSchedLog.Value() );
			bHadError = true;
		}
	}

		// This is checking for the existance of an "old-style" rescue
		// DAG file.
	if (!deepOpts.autoRescue && deepOpts.doRescueFrom < 1 &&
				fileExists(shallowOpts.strRescueFile))
	{
		fprintf( stderr, "ERROR: \"%s\" already exists.\n",
				 shallowOpts.strRescueFile.Value() );
		fprintf( stderr, "	You may want to resubmit your DAG using that "
				 "file, instead of \"%s\"\n", shallowOpts.primaryDagFile.Value());
		fprintf( stderr, "	Look at the HTCondor manual for details about DAG "
				 "rescue files.\n" );
		fprintf( stderr, "	Please investigate and either remove \"%s\",\n",
				 shallowOpts.strRescueFile.Value() );
		fprintf( stderr, "	or use it as the input to condor_submit_dag.\n" );
		bHadError = true;
	}

	if (bHadError) 
	{
		fprintf( stderr, "\nSome file(s) needed by %s already exist.  ",
				 dagman_exe );
		if(!usingPythonBindings) {
			fprintf( stderr, "Either rename them,\nuse the \"-f\" option to "
				 "force them to be overwritten, or use\n"
				 "the \"-update_submit\" option to update the submit "
				 "file and continue.\n" );
		}
		else {
			fprintf( stderr, "Either rename them,\nor set the { \"force\" : True }"
				" option to force them to be overwritten.\n" );
		}
		return false;
	}

	return true;
}

