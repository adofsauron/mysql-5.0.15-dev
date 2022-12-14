/* Copyright (C) 2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <ndb_global.h>
#include <my_pthread.h>

#include "MgmtSrvr.hpp"
#include "MgmtErrorReporter.hpp"
#include <ConfigRetriever.hpp>

#include <NdbOut.hpp>
#include <NdbApiSignal.hpp>
#include <kernel_types.h>
#include <RefConvert.hpp>
#include <BlockNumbers.h>
#include <GlobalSignalNumbers.h>
#include <signaldata/TestOrd.hpp>
#include <signaldata/TamperOrd.hpp>
#include <signaldata/StartOrd.hpp>
#include <signaldata/ApiVersion.hpp>
#include <signaldata/ResumeReq.hpp>
#include <signaldata/SetLogLevelOrd.hpp>
#include <signaldata/EventSubscribeReq.hpp>
#include <signaldata/EventReport.hpp>
#include <signaldata/DumpStateOrd.hpp>
#include <signaldata/BackupSignalData.hpp>
#include <signaldata/GrepImpl.hpp>
#include <signaldata/ManagementServer.hpp>
#include <signaldata/NFCompleteRep.hpp>
#include <signaldata/NodeFailRep.hpp>
#include <NdbSleep.h>
#include <EventLogger.hpp>
#include <DebuggerNames.hpp>
#include <ndb_version.h>

#include <SocketServer.hpp>
#include <NdbConfig.h>

#include <NdbAutoPtr.hpp>

#include <ndberror.h>

#include <mgmapi.h>
#include <mgmapi_configuration.hpp>
#include <mgmapi_config_parameters.h>
#include <m_string.h>

#include <SignalSender.hpp>

//#define MGM_SRV_DEBUG
#ifdef MGM_SRV_DEBUG
#define DEBUG(x) do ndbout << x << endl; while(0)
#else
#define DEBUG(x)
#endif

#define INIT_SIGNAL_SENDER(ss,nodeId) \
  SignalSender ss(theFacade); \
  ss.lock(); /* lock will be released on exit */ \
  {\
    int result = okToSendTo(nodeId, true);\
    if (result != 0) {\
      return result;\
    }\
  }

extern int global_flag_send_heartbeat_now;
extern int g_no_nodeid_checks;
extern my_bool opt_core;

static void require(bool v)
{
  if(!v)
  {
    if (opt_core)
      abort();
    else
      exit(-1);
  }
}

void *
MgmtSrvr::logLevelThread_C(void* m)
{
  MgmtSrvr *mgm = (MgmtSrvr*)m;
  mgm->logLevelThreadRun();
  return 0;
}

extern EventLogger g_eventLogger;

static NdbOut&
operator<<(NdbOut& out, const LogLevel & ll)
{
  out << "[LogLevel: ";
  for(size_t i = 0; i<LogLevel::LOGLEVEL_CATEGORIES; i++)
    out << ll.getLogLevel((LogLevel::EventCategory)i) << " ";
  out << "]";
  return out;
}

void
MgmtSrvr::logLevelThreadRun() 
{
  while (!_isStopThread) {
    /**
     * Handle started nodes
     */
    EventSubscribeReq req;
    req = m_event_listner[0].m_logLevel;
    req.blockRef = _ownReference;

    SetLogLevelOrd ord;
    
    m_started_nodes.lock();
    while(m_started_nodes.size() > 0){
      Uint32 node = m_started_nodes[0];
      m_started_nodes.erase(0, false);
      m_started_nodes.unlock();

      setEventReportingLevelImpl(node, req);
      
      ord = m_nodeLogLevel[node];
      setNodeLogLevelImpl(node, ord);
      
      m_started_nodes.lock();
    }				 
    m_started_nodes.unlock();
    
    m_log_level_requests.lock();
    while(m_log_level_requests.size() > 0){
      req = m_log_level_requests[0];
      m_log_level_requests.erase(0, false);
      m_log_level_requests.unlock();
      
      LogLevel tmp;
      tmp = req;
      
      if(req.blockRef == 0){
	req.blockRef = _ownReference;
	setEventReportingLevelImpl(0, req);
      } else {
	ord = req;
	setNodeLogLevelImpl(req.blockRef, ord);
      }
      m_log_level_requests.lock();
    }      
    m_log_level_requests.unlock();
    NdbSleep_MilliSleep(_logLevelThreadSleep);  
  }
}

void
MgmtSrvr::startEventLog() 
{
  NdbMutex_Lock(m_configMutex);

  g_eventLogger.setCategory("MgmSrvr");

  ndb_mgm_configuration_iterator 
    iter(* _config->m_configValues, CFG_SECTION_NODE);

  if(iter.find(CFG_NODE_ID, _ownNodeId) != 0){
    NdbMutex_Unlock(m_configMutex);
    return;
  }
  
  const char * tmp;
  BaseString logdest;
  char *clusterLog= NdbConfig_ClusterLogFileName(_ownNodeId);
  NdbAutoPtr<char> tmp_aptr(clusterLog);

  if(iter.get(CFG_LOG_DESTINATION, &tmp) == 0){
    logdest.assign(tmp);
  }
  NdbMutex_Unlock(m_configMutex);
  
  if(logdest.length() == 0 || logdest == "") {
    logdest.assfmt("FILE:filename=%s,maxsize=1000000,maxfiles=6", 
		   clusterLog);
  }
  if(!g_eventLogger.addHandler(logdest)) {
    ndbout << "Warning: could not add log destination \""
	   << logdest.c_str() << "\"" << endl;
  }
}

void 
MgmtSrvr::stopEventLog() 
{
  // Nothing yet
}

class ErrorItem 
{
public:
  int _errorCode;
  const char * _errorText;
};

bool
MgmtSrvr::setEventLogFilter(int severity, int enable)
{
  Logger::LoggerLevel level = (Logger::LoggerLevel)severity;
  if (enable > 0) {
    g_eventLogger.enable(level);
  } else if (enable == 0) {
    g_eventLogger.disable(level);
  } else if (g_eventLogger.isEnable(level)) {
    g_eventLogger.disable(level);
  } else {
    g_eventLogger.enable(level);
  }
  return g_eventLogger.isEnable(level);
}

bool 
MgmtSrvr::isEventLogFilterEnabled(int severity) 
{
  return g_eventLogger.isEnable((Logger::LoggerLevel)severity);
}

static ErrorItem errorTable[] = 
{
  {MgmtSrvr::NO_CONTACT_WITH_PROCESS, "No contact with the process (dead ?)."},
  {MgmtSrvr::PROCESS_NOT_CONFIGURED, "The process is not configured."},
  {MgmtSrvr::WRONG_PROCESS_TYPE, 
   "The process has wrong type. Expected a DB process."},
  {MgmtSrvr::COULD_NOT_ALLOCATE_MEMORY, "Could not allocate memory."},
  {MgmtSrvr::SEND_OR_RECEIVE_FAILED, "Send to process or receive failed."},
  {MgmtSrvr::INVALID_LEVEL, "Invalid level. Should be between 1 and 30."},
  {MgmtSrvr::INVALID_ERROR_NUMBER, "Invalid error number. Should be >= 0."},
  {MgmtSrvr::INVALID_TRACE_NUMBER, "Invalid trace number."},
  {MgmtSrvr::NOT_IMPLEMENTED, "Not implemented."},
  {MgmtSrvr::INVALID_BLOCK_NAME, "Invalid block name"},

  {MgmtSrvr::CONFIG_PARAM_NOT_EXIST, 
   "The configuration parameter does not exist for the process type."},
  {MgmtSrvr::CONFIG_PARAM_NOT_UPDATEABLE, 
   "The configuration parameter is not possible to update."},
  {MgmtSrvr::VALUE_WRONG_FORMAT_INT_EXPECTED, 
   "Incorrect value. Expected integer."},
  {MgmtSrvr::VALUE_TOO_LOW, "Value is too low."},
  {MgmtSrvr::VALUE_TOO_HIGH, "Value is too high."},
  {MgmtSrvr::VALUE_WRONG_FORMAT_BOOL_EXPECTED, 
   "Incorrect value. Expected TRUE or FALSE."},

  {MgmtSrvr::CONFIG_FILE_OPEN_WRITE_ERROR, 
   "Could not open configuration file for writing."},
  {MgmtSrvr::CONFIG_FILE_OPEN_READ_ERROR, 
   "Could not open configuration file for reading."},
  {MgmtSrvr::CONFIG_FILE_WRITE_ERROR, 
   "Write error when writing configuration file."},
  {MgmtSrvr::CONFIG_FILE_READ_ERROR, 
   "Read error when reading configuration file."},
  {MgmtSrvr::CONFIG_FILE_CLOSE_ERROR, "Could not close configuration file."},

  {MgmtSrvr::CONFIG_CHANGE_REFUSED_BY_RECEIVER, 
   "The change was refused by the receiving process."},
  {MgmtSrvr::COULD_NOT_SYNC_CONFIG_CHANGE_AGAINST_PHYSICAL_MEDIUM, 
   "The change could not be synced against physical medium."},
  {MgmtSrvr::CONFIG_FILE_CHECKSUM_ERROR, 
   "The config file is corrupt. Checksum error."},
  {MgmtSrvr::NOT_POSSIBLE_TO_SEND_CONFIG_UPDATE_TO_PROCESS_TYPE, 
   "It is not possible to send an update of a configuration variable "
   "to this kind of process."},
  {MgmtSrvr::NODE_SHUTDOWN_IN_PROGESS, "Node shutdown in progress" },
  {MgmtSrvr::SYSTEM_SHUTDOWN_IN_PROGRESS, "System shutdown in progress" },
  {MgmtSrvr::NODE_SHUTDOWN_WOULD_CAUSE_SYSTEM_CRASH,
   "Node shutdown would cause system crash" },
  {MgmtSrvr::NODE_NOT_API_NODE, "The specified node is not an API node." },
  {MgmtSrvr::OPERATION_NOT_ALLOWED_START_STOP, 
   "Operation not allowed while nodes are starting or stopping."},
  {MgmtSrvr::NO_CONTACT_WITH_DB_NODES, "No contact with database nodes" }
};

int MgmtSrvr::translateStopRef(Uint32 errCode)
{
  switch(errCode){
  case StopRef::NodeShutdownInProgress:
    return NODE_SHUTDOWN_IN_PROGESS;
    break;
  case StopRef::SystemShutdownInProgress:
    return SYSTEM_SHUTDOWN_IN_PROGRESS;
    break;
  case StopRef::NodeShutdownWouldCauseSystemCrash:
    return NODE_SHUTDOWN_WOULD_CAUSE_SYSTEM_CRASH;
    break;
  }
  return 4999;
}

static int noOfErrorCodes = sizeof(errorTable) / sizeof(ErrorItem);

int 
MgmtSrvr::getNodeCount(enum ndb_mgm_node_type type) const 
{
  int count = 0;
  NodeId nodeId = 0;

  while (getNextNodeId(&nodeId, type)) {
    count++;
  }
  return count;
}

int 
MgmtSrvr::getPort() const
{
  if(NdbMutex_Lock(m_configMutex))
    return 0;

  ndb_mgm_configuration_iterator 
    iter(* _config->m_configValues, CFG_SECTION_NODE);

  if(iter.find(CFG_NODE_ID, getOwnNodeId()) != 0){
    ndbout << "Could not retrieve configuration for Node " 
	   << getOwnNodeId() << " in config file." << endl 
	   << "Have you set correct NodeId for this node?" << endl;
    NdbMutex_Unlock(m_configMutex);
    return 0;
  }

  unsigned type;
  if(iter.get(CFG_TYPE_OF_SECTION, &type) != 0 ||
     type != NODE_TYPE_MGM){
    ndbout << "Local node id " << getOwnNodeId()
	   << " is not defined as management server" << endl
	   << "Have you set correct NodeId for this node?" << endl;
    NdbMutex_Unlock(m_configMutex);
    return 0;
  }
  
  Uint32 port = 0;
  if(iter.get(CFG_MGM_PORT, &port) != 0){
    ndbout << "Could not find PortNumber in the configuration file." << endl;
    NdbMutex_Unlock(m_configMutex);
    return 0;
  }

  NdbMutex_Unlock(m_configMutex);

  return port;
}

/* Constructor */
int MgmtSrvr::init()
{
  if ( _ownNodeId > 0)
    return 0;
  return -1;
}

MgmtSrvr::MgmtSrvr(SocketServer *socket_server,
		   const char *config_filename,
		   const char *connect_string) :
  _blockNumber(1), // Hard coded block number since it makes it easy to send
                   // signals to other management servers.
  m_socket_server(socket_server),
  _ownReference(0),
  theSignalIdleList(NULL),
  theWaitState(WAIT_SUBSCRIBE_CONF),
  m_event_listner(this)
{
    
  DBUG_ENTER("MgmtSrvr::MgmtSrvr");

  _ownNodeId= 0;

  _config     = NULL;

  _isStopThread        = false;
  _logLevelThread      = NULL;
  _logLevelThreadSleep = 500;

  theFacade = 0;

  m_newConfig = NULL;
  if (config_filename)
    m_configFilename.assign(config_filename);

  m_nextConfigGenerationNumber = 0;

  m_config_retriever= new ConfigRetriever(connect_string,
					  NDB_VERSION, NDB_MGM_NODE_TYPE_MGM);
  // if connect_string explicitly given or
  // no config filename is given then
  // first try to allocate nodeid from another management server
  if ((connect_string || config_filename == NULL) &&
      (m_config_retriever->do_connect(0,0,0) == 0))
  {
    int tmp_nodeid= 0;
    tmp_nodeid= m_config_retriever->allocNodeId(0 /*retry*/,0 /*delay*/);
    if (tmp_nodeid == 0)
    {
      ndbout_c(m_config_retriever->getErrorString());
      require(false);
    }
    // read config from other managent server
    _config= fetchConfig();
    if (_config == 0)
    {
      ndbout << m_config_retriever->getErrorString() << endl;
      require(false);
    }
    _ownNodeId= tmp_nodeid;
  }

  if (_ownNodeId == 0)
  {
    // read config locally
    _config= readConfig();
    if (_config == 0) {
      ndbout << "Unable to read config file" << endl;
      exit(-1);
    }
  }

  theMgmtWaitForResponseCondPtr = NdbCondition_Create();

  m_configMutex = NdbMutex_Create();

  /**
   * Fill the nodeTypes array
   */
  for(Uint32 i = 0; i<MAX_NODES; i++) {
    nodeTypes[i] = (enum ndb_mgm_node_type)-1;
    m_connect_address[i].s_addr= 0;
  }

  {
    ndb_mgm_configuration_iterator
      iter(* _config->m_configValues, CFG_SECTION_NODE);

    for(iter.first(); iter.valid(); iter.next()){
      unsigned type, id;
      if(iter.get(CFG_TYPE_OF_SECTION, &type) != 0)
	continue;
      
      if(iter.get(CFG_NODE_ID, &id) != 0)
	continue;
      
      MGM_REQUIRE(id < MAX_NODES);
      
      switch(type){
      case NODE_TYPE_DB:
	nodeTypes[id] = NDB_MGM_NODE_TYPE_NDB;
	break;
      case NODE_TYPE_API:
	nodeTypes[id] = NDB_MGM_NODE_TYPE_API;
	break;
      case NODE_TYPE_MGM:
	nodeTypes[id] = NDB_MGM_NODE_TYPE_MGM;
	break;
      case NODE_TYPE_REP:
	nodeTypes[id] = NDB_MGM_NODE_TYPE_REP;
	break;
      case NODE_TYPE_EXT_REP:
      default:
	break;
      }
    }
  }

  _props = NULL;
  BaseString error_string;

  if ((m_node_id_mutex = NdbMutex_Create()) == 0)
  {
    ndbout << "mutex creation failed line = " << __LINE__ << endl;
    require(false);
  }

  if (_ownNodeId == 0) // we did not get node id from other server
  {
    NodeId tmp= m_config_retriever->get_configuration_nodeid();

    if (!alloc_node_id(&tmp, NDB_MGM_NODE_TYPE_MGM,
		       0, 0, error_string)){
      ndbout << "Unable to obtain requested nodeid: "
	     << error_string.c_str() << endl;
      require(false);
    }
    _ownNodeId = tmp;
  }

  {
    DBUG_PRINT("info", ("verifyConfig"));
    if (!m_config_retriever->verifyConfig(_config->m_configValues,
					  _ownNodeId))
    {
      ndbout << m_config_retriever->getErrorString() << endl;
      require(false);
    }
  }

  // Setup clusterlog as client[0] in m_event_listner
  {
    Ndb_mgmd_event_service::Event_listener se;
    se.m_socket = NDB_INVALID_SOCKET;
    for(size_t t = 0; t<LogLevel::LOGLEVEL_CATEGORIES; t++){
      se.m_logLevel.setLogLevel((LogLevel::EventCategory)t, 7);
    }
    se.m_logLevel.setLogLevel(LogLevel::llError, 15);
    se.m_logLevel.setLogLevel(LogLevel::llConnection, 8);
    se.m_logLevel.setLogLevel(LogLevel::llBackup, 15);
    m_event_listner.m_clients.push_back(se);
    m_event_listner.m_logLevel = se.m_logLevel;
  }
  
  DBUG_VOID_RETURN;
}


//****************************************************************************
//****************************************************************************
bool 
MgmtSrvr::check_start() 
{
  if (_config == 0) {
    DEBUG("MgmtSrvr.cpp: _config is NULL.");
    return false;
  }

  return true;
}

bool 
MgmtSrvr::start(BaseString &error_string)
{
  DBUG_ENTER("MgmtSrvr::start");
  if (_props == NULL) {
    if (!check_start()) {
      error_string.append("MgmtSrvr.cpp: check_start() failed.");
      DBUG_RETURN(false);
    }
  }
  theFacade= TransporterFacade::theFacadeInstance
    = new TransporterFacade();
  
  if(theFacade == 0) {
    DEBUG("MgmtSrvr.cpp: theFacade is NULL.");
    error_string.append("MgmtSrvr.cpp: theFacade is NULL.");
    DBUG_RETURN(false);
  }  
  if ( theFacade->start_instance
       (_ownNodeId, (ndb_mgm_configuration*)_config->m_configValues) < 0) {
    DEBUG("MgmtSrvr.cpp: TransporterFacade::start_instance < 0.");
    DBUG_RETURN(false);
  }

  MGM_REQUIRE(_blockNumber == 1);

  // Register ourself at TransporterFacade to be able to receive signals
  // and to be notified when a database process has died.
  _blockNumber = theFacade->open(this,
				 signalReceivedNotification,
				 nodeStatusNotification);
  
  if(_blockNumber == -1){
    DEBUG("MgmtSrvr.cpp: _blockNumber is -1.");
    error_string.append("MgmtSrvr.cpp: _blockNumber is -1.");
    theFacade->stop_instance();
    theFacade = 0;
    DBUG_RETURN(false);
  }

  TransporterRegistry *reg = theFacade->get_registry();
  for(unsigned int i=0;i<reg->m_transporter_interface.size();i++) {
    BaseString msg;
    DBUG_PRINT("info",("Setting dynamic port %d->%d : %d",
		       reg->get_localNodeId(),
		       reg->m_transporter_interface[i].m_remote_nodeId,
		       reg->m_transporter_interface[i].m_s_service_port
		       )
	       );
    int res = setConnectionDbParameter((int)reg->get_localNodeId(),
				       (int)reg->m_transporter_interface[i]
				            .m_remote_nodeId,
				       (int)CFG_CONNECTION_SERVER_PORT,
				       reg->m_transporter_interface[i]
				            .m_s_service_port,
					 msg);
    DBUG_PRINT("info",("Set result: %d: %s",res,msg.c_str()));
  }

  _ownReference = numberToRef(_blockNumber, _ownNodeId);
  
  startEventLog();
  // Set the initial confirmation count for subscribe requests confirm
  // from NDB nodes in the cluster.
  //
  // Loglevel thread
  _logLevelThread = NdbThread_Create(logLevelThread_C,
				     (void**)this,
				     32768,
				     "MgmtSrvr_Loglevel",
				     NDB_THREAD_PRIO_LOW);

  DBUG_RETURN(true);
}


//****************************************************************************
//****************************************************************************
MgmtSrvr::~MgmtSrvr() 
{
  if(theFacade != 0){
    theFacade->stop_instance();
    delete theFacade;
    theFacade = 0;
  }

  stopEventLog();

  NdbMutex_Destroy(m_node_id_mutex);
  NdbCondition_Destroy(theMgmtWaitForResponseCondPtr);
  NdbMutex_Destroy(m_configMutex);

  if(m_newConfig != NULL)
    free(m_newConfig);

  if(_config != NULL)
    delete _config;

  // End set log level thread
  void* res = 0;
  _isStopThread = true;

  if (_logLevelThread != NULL) {
    NdbThread_WaitFor(_logLevelThread, &res);
    NdbThread_Destroy(&_logLevelThread);
  }

  if (m_config_retriever)
    delete m_config_retriever;
}

//****************************************************************************
//****************************************************************************

int MgmtSrvr::okToSendTo(NodeId nodeId, bool unCond) 
{
  if(nodeId == 0)
    return 0;

  if (getNodeType(nodeId) != NDB_MGM_NODE_TYPE_NDB)
    return WRONG_PROCESS_TYPE;
  
  // Check if we have contact with it
  if(unCond){
    if(theFacade->theClusterMgr->getNodeInfo(nodeId).connected)
      return 0;
    return NO_CONTACT_WITH_PROCESS;
  }
  if (theFacade->get_node_alive(nodeId) == 0) {
    return NO_CONTACT_WITH_PROCESS;
  } else {
    return 0;
  }
}

void report_unknown_signal(SimpleSignal *signal)
{
  g_eventLogger.error("Unknown signal received. SignalNumber: "
		      "%i from (%d, %x)",
		      signal->readSignalNumber(),
		      refToNode(signal->header.theSendersBlockRef),
		      refToBlock(signal->header.theSendersBlockRef));
}

/*****************************************************************************
 * Starting and stopping database nodes
 ****************************************************************************/

int 
MgmtSrvr::start(int nodeId)
{
  INIT_SIGNAL_SENDER(ss,nodeId);
  
  SimpleSignal ssig;
  StartOrd* const startOrd = CAST_PTR(StartOrd, ssig.getDataPtrSend());
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_START_ORD, StartOrd::SignalLength);
  startOrd->restartInfo = 0;
  
  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}

/*****************************************************************************
 * Version handling
 *****************************************************************************/

int 
MgmtSrvr::versionNode(int nodeId, Uint32 &version, const char **address)
{
  version= 0;
  if (getOwnNodeId() == nodeId)
  {
    /**
     * If we're inquiring about our own node id,
     * We know what version we are (version implies connected for mgm)
     * but would like to find out from elsewhere what address they're using
     * to connect to us. This means that secondary mgm servers
     * can list ip addresses for mgm servers.
     *
     * If we don't get an address (i.e. no db nodes),
     * we get the address from the configuration.
     */
    sendVersionReq(nodeId, version, address);
    version= NDB_VERSION;
    if(!*address)
    {
      ndb_mgm_configuration_iterator
	iter(*_config->m_configValues, CFG_SECTION_NODE);
      unsigned tmp= 0;
      for(iter.first();iter.valid();iter.next())
      {
	if(iter.get(CFG_NODE_ID, &tmp)) require(false);
	if((unsigned)nodeId!=tmp)
	  continue;
	if(iter.get(CFG_NODE_HOST, address)) require(false);
	break;
      }
    }
  }
  else if (getNodeType(nodeId) == NDB_MGM_NODE_TYPE_NDB)
  {
    ClusterMgr::Node node= theFacade->theClusterMgr->getNodeInfo(nodeId);
    if(node.connected)
      version= node.m_info.m_version;
    *address= get_connect_address(nodeId);
  }
  else if (getNodeType(nodeId) == NDB_MGM_NODE_TYPE_API ||
	   getNodeType(nodeId) == NDB_MGM_NODE_TYPE_MGM)
  {
    return sendVersionReq(nodeId, version, address);
  }

  return 0;
}

int 
MgmtSrvr::sendVersionReq(int v_nodeId, Uint32 &version, const char **address)
{
  SignalSender ss(theFacade);
  ss.lock();

  SimpleSignal ssig;
  ApiVersionReq* req = CAST_PTR(ApiVersionReq, ssig.getDataPtrSend());
  req->senderRef = ss.getOwnRef();
  req->nodeId = v_nodeId;
  ssig.set(ss, TestOrd::TraceAPI, QMGR, GSN_API_VERSION_REQ, 
	   ApiVersionReq::SignalLength);

  int do_send = 1;
  NodeId nodeId;

  while (1)
  {
    if (do_send)
    {
      bool next;
      nodeId = 0;

      while((next = getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB)) == true &&
	    okToSendTo(nodeId, true) != 0);

      const ClusterMgr::Node &node=
	theFacade->theClusterMgr->getNodeInfo(nodeId);
      if(next && node.m_state.startLevel != NodeState::SL_STARTED)
      {
	NodeId tmp=nodeId;
	while((next = getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB)) == true &&
	      okToSendTo(nodeId, true) != 0);
	if(!next)
	  nodeId= tmp;
      }

      if(!next) return NO_CONTACT_WITH_DB_NODES;

      if (ss.sendSignal(nodeId, &ssig) != SEND_OK) {
	return SEND_OR_RECEIVE_FAILED;
      }
      do_send = 0;
    }

    SimpleSignal *signal = ss.waitFor();

    int gsn = signal->readSignalNumber();
    switch (gsn) {
    case GSN_API_VERSION_CONF: {
      const ApiVersionConf * const conf = 
	CAST_CONSTPTR(ApiVersionConf, signal->getDataPtr());
      assert(conf->nodeId == v_nodeId);
      version = conf->version;
      struct in_addr in;
      in.s_addr= conf->inet_addr;
      *address= inet_ntoa(in);
      return 0;
    }
    case GSN_NF_COMPLETEREP:{
      const NFCompleteRep * const rep =
	CAST_CONSTPTR(NFCompleteRep, signal->getDataPtr());
      if (rep->failedNodeId == nodeId)
	do_send = 1; // retry with other node
      continue;
    }
    case GSN_NODE_FAILREP:{
      const NodeFailRep * const rep =
	CAST_CONSTPTR(NodeFailRep, signal->getDataPtr());
      if (NodeBitmask::get(rep->theNodes,nodeId))
	do_send = 1; // retry with other node
      continue;
    }
    default:
      report_unknown_signal(signal);
      return SEND_OR_RECEIVE_FAILED;
    }
    break;
  } // while(1)

  return 0;
}

/*
 * Common method for handeling all STOP_REQ signalling that
 * is used by Stopping, Restarting and Single user commands
 */

int MgmtSrvr::sendSTOP_REQ(NodeId nodeId,
			   NodeBitmask &stoppedNodes,
			   Uint32 singleUserNodeId,
			   bool abort,
			   bool stop,
			   bool restart,
			   bool nostart,
			   bool initialStart)
{
  stoppedNodes.clear();

  SignalSender ss(theFacade);
  ss.lock(); // lock will be released on exit

  SimpleSignal ssig;
  StopReq* const stopReq = CAST_PTR(StopReq, ssig.getDataPtrSend());
  ssig.set(ss, TestOrd::TraceAPI, NDBCNTR, GSN_STOP_REQ, StopReq::SignalLength);

  stopReq->requestInfo = 0;
  stopReq->apiTimeout = 5000;
  stopReq->transactionTimeout = 1000;
  stopReq->readOperationTimeout = 1000;
  stopReq->operationTimeout = 1000;
  stopReq->senderData = 12;
  stopReq->senderRef = ss.getOwnRef();
  if (singleUserNodeId)
  {
    stopReq->singleuser = 1;
    stopReq->singleUserApi = singleUserNodeId;
    StopReq::setSystemStop(stopReq->requestInfo, false);
    StopReq::setPerformRestart(stopReq->requestInfo, false);
    StopReq::setStopAbort(stopReq->requestInfo, false);
  }
  else
  {
    stopReq->singleuser = 0;
    StopReq::setSystemStop(stopReq->requestInfo, stop);
    StopReq::setPerformRestart(stopReq->requestInfo, restart);
    StopReq::setStopAbort(stopReq->requestInfo, abort);
    StopReq::setNoStart(stopReq->requestInfo, nostart);
    StopReq::setInitialStart(stopReq->requestInfo, initialStart);
  }

  // send the signals
  NodeBitmask nodes;
  if (nodeId)
  {
    {
      int r;
      if((r = okToSendTo(nodeId, true)) != 0)
	return r;
    }
    {
      if (ss.sendSignal(nodeId, &ssig) != SEND_OK)
	return SEND_OR_RECEIVE_FAILED;
    }
    nodes.set(nodeId);
  }
  else
    while(getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB))
    {
      if(okToSendTo(nodeId, true) == 0)
      {
	SendStatus result = ss.sendSignal(nodeId, &ssig);
	if (result == SEND_OK)
	  nodes.set(nodeId);
      }
    }

  // now wait for the replies
  int error = 0;
  while (!nodes.isclear())
  {
    SimpleSignal *signal = ss.waitFor();
    int gsn = signal->readSignalNumber();
    switch (gsn) {
    case GSN_STOP_REF:{
      const StopRef * const ref = CAST_CONSTPTR(StopRef, signal->getDataPtr());
      const NodeId nodeId = refToNode(signal->header.theSendersBlockRef);
#ifdef VM_TRACE
      ndbout_c("Node %d refused stop", nodeId);
#endif
      assert(nodes.get(nodeId));
      nodes.clear(nodeId);
      error = translateStopRef(ref->errorCode);
      break;
    }
    case GSN_STOP_CONF:{
      const StopConf * const ref = CAST_CONSTPTR(StopConf, signal->getDataPtr());
      const NodeId nodeId = refToNode(signal->header.theSendersBlockRef);
#ifdef VM_TRACE
      ndbout_c("Node %d single user mode", nodeId);
#endif
      assert(nodes.get(nodeId));
      assert(singleUserNodeId != 0);
      nodes.clear(nodeId);
      stoppedNodes.set(nodeId);
      break;
    }
    case GSN_NF_COMPLETEREP:{
      const NFCompleteRep * const rep =
	CAST_CONSTPTR(NFCompleteRep, signal->getDataPtr());
#ifdef VM_TRACE
      ndbout_c("Node %d fail completed", rep->failedNodeId);
#endif
      break;
    }
    case GSN_NODE_FAILREP:{
      const NodeFailRep * const rep =
	CAST_CONSTPTR(NodeFailRep, signal->getDataPtr());
      NodeBitmask failedNodes;
      failedNodes.assign(NodeBitmask::Size, rep->theNodes);
#ifdef VM_TRACE
      {
	ndbout << "Failed nodes:";
	for (unsigned i = 0; i < 32*NodeBitmask::Size; i++)
	  if(failedNodes.get(i))
	    ndbout << " " << i;
	ndbout << endl;
      }
#endif
      failedNodes.bitAND(nodes);
      if (!failedNodes.isclear())
      {
	nodes.bitANDC(failedNodes); // clear the failed nodes
	if (singleUserNodeId == 0)
	  stoppedNodes.bitOR(failedNodes);
      }
      break;
    }
    default:
      report_unknown_signal(signal);
#ifdef VM_TRACE
      ndbout_c("Unknown signal %d", gsn);
#endif
      return SEND_OR_RECEIVE_FAILED;
    }
  }
  return error;
}

/*
 * Stop one node
 */

int MgmtSrvr::stopNode(int nodeId, bool abort)
{
  if (!abort)
  {
    NodeId nodeId = 0;
    ClusterMgr::Node node;
    while(getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB))
    {
      node = theFacade->theClusterMgr->getNodeInfo(nodeId);
      if((node.m_state.startLevel != NodeState::SL_STARTED) && 
	 (node.m_state.startLevel != NodeState::SL_NOTHING))
	return OPERATION_NOT_ALLOWED_START_STOP;
    }
  }
  NodeBitmask nodes;
  return sendSTOP_REQ(nodeId,
		      nodes,
		      0,
		      abort,
		      false,
		      false,
		      false,
		      false);
}

/*
 * Perform system shutdown
 */

int MgmtSrvr::stop(int * stopCount, bool abort)
{
  NodeBitmask nodes;
  int ret = sendSTOP_REQ(0,
			 nodes,
			 0,
			 abort,
			 true,
			 false,
			 false,
			 false);
  if (stopCount)
    *stopCount = nodes.count();
  return ret;
}

/*
 * Enter single user mode on all live nodes
 */

int MgmtSrvr::enterSingleUser(int * stopCount, Uint32 singleUserNodeId)
{
  if (getNodeType(singleUserNodeId) != NDB_MGM_NODE_TYPE_API)
    return NODE_NOT_API_NODE;
  NodeId nodeId = 0;
  ClusterMgr::Node node;
  while(getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB))
  {
    node = theFacade->theClusterMgr->getNodeInfo(nodeId);
    if((node.m_state.startLevel != NodeState::SL_STARTED) && 
       (node.m_state.startLevel != NodeState::SL_NOTHING))
      return OPERATION_NOT_ALLOWED_START_STOP;
  }
  NodeBitmask nodes;
  int ret = sendSTOP_REQ(0,
			 nodes,
			 singleUserNodeId,
			 false,
			 false,
			 false,
			 false,
			 false);
  if (stopCount)
    *stopCount = nodes.count();
  return ret;
}

/*
 * Perform node restart
 */

int MgmtSrvr::restartNode(int nodeId, bool nostart, bool initialStart, 
			  bool abort)
{
  NodeBitmask nodes;
  return sendSTOP_REQ(nodeId,
		      nodes,
		      0,
		      abort,
		      false,
		      true,
		      nostart,
		      initialStart);
}

/*
 * Perform system restart
 */

int MgmtSrvr::restart(bool nostart, bool initialStart, 
		      bool abort, int * stopCount )
{
  NodeBitmask nodes;
  int ret = sendSTOP_REQ(0,
			 nodes,
			 0,
			 abort,
			 true,
			 true,
			 true,
			 initialStart);

  if (ret)
    return ret;

  if (stopCount)
    *stopCount = nodes.count();

#ifdef VM_TRACE
    ndbout_c("Stopped %d nodes", nodes.count());
#endif
  /**
   * Here all nodes were correctly stopped,
   * so we wait for all nodes to be contactable
   */
  int waitTime = 12000;
  NodeId nodeId = 0;
  NDB_TICKS maxTime = NdbTick_CurrentMillisecond() + waitTime;

  ndbout_c(" %d", nodes.get(1));
  ndbout_c(" %d", nodes.get(2));

  while(getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB)) {
    if (!nodes.get(nodeId))
      continue;
    enum ndb_mgm_node_status s;
    s = NDB_MGM_NODE_STATUS_NO_CONTACT;
#ifdef VM_TRACE
    ndbout_c("Waiting for %d not started", nodeId);
#endif
    while (s != NDB_MGM_NODE_STATUS_NOT_STARTED && waitTime > 0) {
      Uint32 startPhase = 0, version = 0, dynamicId = 0, nodeGroup = 0;
      Uint32 connectCount = 0;
      bool system;
      const char *address;
      status(nodeId, &s, &version, &startPhase, 
	     &system, &dynamicId, &nodeGroup, &connectCount, &address);
      NdbSleep_MilliSleep(100);  
      waitTime = (maxTime - NdbTick_CurrentMillisecond());
    }
  }
  
  if(nostart)
    return 0;
  
  /**
   * Now we start all database nodes (i.e. we make them non-idle)
   * We ignore the result we get from the start command.
   */
  nodeId = 0;
  while(getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB)) {
    if (!nodes.get(nodeId))
      continue;
    int result;
    result = start(nodeId);
    DEBUG("Starting node " << nodeId << " with result " << result);
    /**
     * Errors from this call are deliberately ignored.
     * Maybe the user only wanted to restart a subset of the nodes.
     * It is also easy for the user to check which nodes have 
     * started and which nodes have not.
     */
  }
  
  return 0;
}

int
MgmtSrvr::exitSingleUser(int * stopCount, bool abort)
{
  NodeId nodeId = 0;
  int count = 0;

  SignalSender ss(theFacade);
  ss.lock(); // lock will be released on exit

  SimpleSignal ssig;
  ResumeReq* const resumeReq = 
    CAST_PTR(ResumeReq, ssig.getDataPtrSend());
  ssig.set(ss,TestOrd::TraceAPI, NDBCNTR, GSN_RESUME_REQ, 
	   ResumeReq::SignalLength);
  resumeReq->senderData = 12;
  resumeReq->senderRef = ss.getOwnRef();

  while(getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB)){
    if(okToSendTo(nodeId, true) == 0){
      SendStatus result = ss.sendSignal(nodeId, &ssig);
      if (result == SEND_OK)
	count++;
    }
  }

  if(stopCount != 0)
    * stopCount = count;

  return 0;
}

/*****************************************************************************
 * Status
 ****************************************************************************/

#include <ClusterMgr.hpp>

int 
MgmtSrvr::status(int nodeId, 
                 ndb_mgm_node_status * _status, 
		 Uint32 * version,
		 Uint32 * _phase, 
		 bool * _system,
		 Uint32 * dynamic,
		 Uint32 * nodegroup,
		 Uint32 * connectCount,
		 const char **address)
{
  if (getNodeType(nodeId) == NDB_MGM_NODE_TYPE_API ||
      getNodeType(nodeId) == NDB_MGM_NODE_TYPE_MGM) {
    versionNode(nodeId, *version, address);
  } else {
    *address= get_connect_address(nodeId);
  }

  const ClusterMgr::Node node = 
    theFacade->theClusterMgr->getNodeInfo(nodeId);

  if(!node.connected){
    * _status = NDB_MGM_NODE_STATUS_NO_CONTACT;
    return 0;
  }
  
  if (getNodeType(nodeId) == NDB_MGM_NODE_TYPE_NDB) {
    * version = node.m_info.m_version;
  }

  * dynamic = node.m_state.dynamicId;
  * nodegroup = node.m_state.nodeGroup;
  * connectCount = node.m_info.m_connectCount;
  
  switch(node.m_state.startLevel){
  case NodeState::SL_CMVMI:
    * _status = NDB_MGM_NODE_STATUS_NOT_STARTED;
    * _phase = 0;
    return 0;
    break;
  case NodeState::SL_STARTING:
    * _status     = NDB_MGM_NODE_STATUS_STARTING;
    * _phase = node.m_state.starting.startPhase;
    return 0;
    break;
  case NodeState::SL_STARTED:
    * _status = NDB_MGM_NODE_STATUS_STARTED;
    * _phase = 0;
    return 0;
    break;
  case NodeState::SL_STOPPING_1:
    * _status = NDB_MGM_NODE_STATUS_SHUTTING_DOWN;
    * _phase = 1;
    * _system = node.m_state.stopping.systemShutdown != 0;
    return 0;
    break;
  case NodeState::SL_STOPPING_2:
    * _status = NDB_MGM_NODE_STATUS_SHUTTING_DOWN;
    * _phase = 2;
    * _system = node.m_state.stopping.systemShutdown != 0;
    return 0;
    break;
  case NodeState::SL_STOPPING_3:
    * _status = NDB_MGM_NODE_STATUS_SHUTTING_DOWN;
    * _phase = 3;
    * _system = node.m_state.stopping.systemShutdown != 0;
    return 0;
    break;
  case NodeState::SL_STOPPING_4:
    * _status = NDB_MGM_NODE_STATUS_SHUTTING_DOWN;
    * _phase = 4;
    * _system = node.m_state.stopping.systemShutdown != 0;
    return 0;
    break;
  case NodeState::SL_SINGLEUSER:
    * _status = NDB_MGM_NODE_STATUS_SINGLEUSER;
    * _phase  = 0;
    return 0;
    break;
  default:
    * _status = NDB_MGM_NODE_STATUS_UNKNOWN;
    * _phase = 0;
    return 0;
  }
  
  return -1;
}

int 
MgmtSrvr::setEventReportingLevelImpl(int nodeId, 
				     const EventSubscribeReq& ll)
{
  INIT_SIGNAL_SENDER(ss,nodeId);

  SimpleSignal ssig;
  EventSubscribeReq * dst = 
    CAST_PTR(EventSubscribeReq, ssig.getDataPtrSend());
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_EVENT_SUBSCRIBE_REQ,
	   EventSubscribeReq::SignalLength);
  *dst = ll;

  send(ss,ssig,nodeId,NODE_TYPE_DB);

#if 0
  while (1)
  {
    SimpleSignal *signal = ss.waitFor();
    int gsn = signal->readSignalNumber();
    switch (gsn) { 
    case GSN_EVENT_SUBSCRIBE_CONF:{
      break;
    }
    case GSN_EVENT_SUBSCRIBE_REF:{
      return SEND_OR_RECEIVE_FAILED;
    }
    case GSN_NF_COMPLETEREP:{
      const NFCompleteRep * const rep =
	CAST_CONSTPTR(NFCompleteRep, signal->getDataPtr());
      if (rep->failedNodeId == nodeId)
	return SEND_OR_RECEIVE_FAILED;
      break;
    }
    case GSN_NODE_FAILREP:{
      const NodeFailRep * const rep =
	CAST_CONSTPTR(NodeFailRep, signal->getDataPtr());
      if (NodeBitmask::get(rep->theNodes,nodeId))
	return SEND_OR_RECEIVE_FAILED;
      break;
    }
    default:
      report_unknown_signal(signal);
      return SEND_OR_RECEIVE_FAILED;
    }

  }
#endif
  return 0;
}

//****************************************************************************
//****************************************************************************
int 
MgmtSrvr::setNodeLogLevelImpl(int nodeId, const SetLogLevelOrd & ll)
{
  INIT_SIGNAL_SENDER(ss,nodeId);

  SimpleSignal ssig;
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_SET_LOGLEVELORD,
	   SetLogLevelOrd::SignalLength);
  SetLogLevelOrd* const dst = CAST_PTR(SetLogLevelOrd, ssig.getDataPtrSend());
  *dst = ll;
  
  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}

int
MgmtSrvr::send(SignalSender &ss, SimpleSignal &ssig, Uint32 node, Uint32 node_type){
  Uint32 max = (node == 0) ? MAX_NODES : node + 1;
  
  for(; node < max; node++){
    while(nodeTypes[node] != (int)node_type && node < max) node++;
    if(nodeTypes[node] != (int)node_type)
      break;
    ss.sendSignal(node, &ssig);
  }
  return 0;
}

//****************************************************************************
//****************************************************************************

int 
MgmtSrvr::insertError(int nodeId, int errorNo) 
{
  if (errorNo < 0) {
    return INVALID_ERROR_NUMBER;
  }

  INIT_SIGNAL_SENDER(ss,nodeId);
  
  SimpleSignal ssig;
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_TAMPER_ORD, 
	   TamperOrd::SignalLength);
  TamperOrd* const tamperOrd = CAST_PTR(TamperOrd, ssig.getDataPtrSend());
  tamperOrd->errorNo = errorNo;

  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}



//****************************************************************************
//****************************************************************************

int 
MgmtSrvr::setTraceNo(int nodeId, int traceNo)
{
  if (traceNo < 0) {
    return INVALID_TRACE_NUMBER;
  }

  INIT_SIGNAL_SENDER(ss,nodeId);

  SimpleSignal ssig;
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_TEST_ORD, TestOrd::SignalLength);
  TestOrd* const testOrd = CAST_PTR(TestOrd, ssig.getDataPtrSend());
  testOrd->clear();
  // Assume TRACE command causes toggling. Not really defined... ? TODO
  testOrd->setTraceCommand(TestOrd::Toggle, 
			   (TestOrd::TraceSpecification)traceNo);

  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}

//****************************************************************************
//****************************************************************************

int 
MgmtSrvr::getBlockNumber(const BaseString &blockName) 
{
  short bno = getBlockNo(blockName.c_str());
  if(bno != 0)
    return bno;
  return -1;
}

//****************************************************************************
//****************************************************************************

int 
MgmtSrvr::setSignalLoggingMode(int nodeId, LogMode mode, 
			       const Vector<BaseString>& blocks)
{
  INIT_SIGNAL_SENDER(ss,nodeId);

  // Convert from MgmtSrvr format...

  TestOrd::Command command;
  if (mode == Off) {
    command = TestOrd::Off;
  }
  else {
    command = TestOrd::On;
  }

  TestOrd::SignalLoggerSpecification logSpec;
  switch (mode) {
  case In:
    logSpec = TestOrd::InputSignals;
    break;
  case Out:
    logSpec = TestOrd::OutputSignals;
    break;
  case InOut:
    logSpec = TestOrd::InputOutputSignals;
    break;
  case Off:
    // In MgmtSrvr interface it's just possible to switch off all logging, both
    // "in" and "out" (this should probably be changed).
    logSpec = TestOrd::InputOutputSignals;
    break;
  default:
    ndbout_c("Unexpected value %d, MgmtSrvr::setSignalLoggingMode, line %d",
	     (unsigned)mode, __LINE__);
    assert(false);
    return -1;
  }

  SimpleSignal ssig;
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_TEST_ORD, TestOrd::SignalLength);

  TestOrd* const testOrd = CAST_PTR(TestOrd, ssig.getDataPtrSend());
  testOrd->clear();
  
  if (blocks.size() == 0 || blocks[0] == "ALL") {
    // Logg command for all blocks
    testOrd->addSignalLoggerCommand(command, logSpec);
  } else {
    for(unsigned i = 0; i < blocks.size(); i++){
      int blockNumber = getBlockNumber(blocks[i]);
      if (blockNumber == -1) {
        return INVALID_BLOCK_NAME;
      }
      testOrd->addSignalLoggerCommand(blockNumber, command, logSpec);
    } // for
  } // else

  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}

/*****************************************************************************
 * Signal tracing
 *****************************************************************************/
int MgmtSrvr::startSignalTracing(int nodeId)
{
  INIT_SIGNAL_SENDER(ss,nodeId);
  
  SimpleSignal ssig;
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_TEST_ORD, TestOrd::SignalLength);

  TestOrd* const testOrd = CAST_PTR(TestOrd, ssig.getDataPtrSend());
  testOrd->clear();
  testOrd->setTestCommand(TestOrd::On);

  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}

int 
MgmtSrvr::stopSignalTracing(int nodeId) 
{
  INIT_SIGNAL_SENDER(ss,nodeId);

  SimpleSignal ssig;
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_TEST_ORD, TestOrd::SignalLength);
  TestOrd* const testOrd = CAST_PTR(TestOrd, ssig.getDataPtrSend());
  testOrd->clear();
  testOrd->setTestCommand(TestOrd::Off);

  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}


/*****************************************************************************
 * Dump state
 *****************************************************************************/

int
MgmtSrvr::dumpState(int nodeId, const char* args)
{
  // Convert the space separeted args 
  // string to an int array
  Uint32 args_array[25];
  Uint32 numArgs = 0;

  char buf[10];  
  int b  = 0;
  memset(buf, 0, 10);
  for (size_t i = 0; i <= strlen(args); i++){
    if (args[i] == ' ' || args[i] == 0){
      args_array[numArgs] = atoi(buf);
      numArgs++;
      memset(buf, 0, 10);
      b = 0;
    } else {
      buf[b] = args[i];
      b++;
    }    
  }
  
  return dumpState(nodeId, args_array, numArgs);
}

int
MgmtSrvr::dumpState(int nodeId, const Uint32 args[], Uint32 no)
{
  INIT_SIGNAL_SENDER(ss,nodeId);

  const Uint32 len = no > 25 ? 25 : no;
  
  SimpleSignal ssig;
  DumpStateOrd * const dumpOrd = 
    CAST_PTR(DumpStateOrd, ssig.getDataPtrSend());
  ssig.set(ss,TestOrd::TraceAPI, CMVMI, GSN_DUMP_STATE_ORD, len);
  for(Uint32 i = 0; i<25; i++){
    if (i < len)
      dumpOrd->args[i] = args[i];
    else
      dumpOrd->args[i] = 0;
  }
  
  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}


//****************************************************************************
//****************************************************************************

const char* MgmtSrvr::getErrorText(int errorCode, char *buf, int buf_sz)
{

  for (int i = 0; i < noOfErrorCodes; ++i) {
    if (errorCode == errorTable[i]._errorCode) {
      BaseString::snprintf(buf, buf_sz, errorTable[i]._errorText);
      buf[buf_sz-1]= 0;
      return buf;
    }
  }

  ndb_error_string(errorCode, buf, buf_sz);
  buf[buf_sz-1]= 0;

  return buf;
}

void 
MgmtSrvr::handleReceivedSignal(NdbApiSignal* signal)
{
  // The way of handling a received signal is taken from the Ndb class.
  int gsn = signal->readSignalNumber();

  switch (gsn) {
  case GSN_EVENT_SUBSCRIBE_CONF:
    break;
  case GSN_EVENT_SUBSCRIBE_REF:
    break;
  case GSN_EVENT_REP:
  {
    EventReport *rep = CAST_PTR(EventReport, signal->getDataPtrSend());
    if (rep->getNodeId() == 0)
      rep->setNodeId(refToNode(signal->theSendersBlockRef));
    eventReport(signal->getDataPtr());
    break;
  }

  case GSN_NF_COMPLETEREP:
    break;
  case GSN_NODE_FAILREP:
    break;

  default:
    g_eventLogger.error("Unknown signal received. SignalNumber: "
			"%i from (%d, %x)",
			gsn,
			refToNode(signal->theSendersBlockRef),
			refToBlock(signal->theSendersBlockRef));
  }
  
  if (theWaitState == NO_WAIT) {
    NdbCondition_Signal(theMgmtWaitForResponseCondPtr);
  }
}

void
MgmtSrvr::handleStatus(NodeId nodeId, bool alive, bool nfComplete)
{
  DBUG_ENTER("MgmtSrvr::handleStatus");
  Uint32 theData[25];
  EventReport *rep = (EventReport *)theData;

  theData[1] = nodeId;
  if (alive) {
    m_started_nodes.push_back(nodeId);
    rep->setEventType(NDB_LE_Connected);
  } else {
    rep->setEventType(NDB_LE_Connected);
    if(nfComplete)
    {
      DBUG_VOID_RETURN;
    }
  }
  
  rep->setNodeId(_ownNodeId);
  eventReport(theData);
  DBUG_VOID_RETURN;
}

//****************************************************************************
//****************************************************************************

void 
MgmtSrvr::signalReceivedNotification(void* mgmtSrvr, 
                                     NdbApiSignal* signal,
				     LinearSectionPtr ptr[3]) 
{
  ((MgmtSrvr*)mgmtSrvr)->handleReceivedSignal(signal);
}


//****************************************************************************
//****************************************************************************
void 
MgmtSrvr::nodeStatusNotification(void* mgmSrv, Uint32 nodeId, 
				 bool alive, bool nfComplete)
{
  DBUG_ENTER("MgmtSrvr::nodeStatusNotification");
  DBUG_PRINT("enter",("nodeid= %d, alive= %d, nfComplete= %d", nodeId, alive, nfComplete));
  ((MgmtSrvr*)mgmSrv)->handleStatus(nodeId, alive, nfComplete);
  DBUG_VOID_RETURN;
}

enum ndb_mgm_node_type 
MgmtSrvr::getNodeType(NodeId nodeId) const 
{
  if(nodeId >= MAX_NODES)
    return (enum ndb_mgm_node_type)-1;
  
  return nodeTypes[nodeId];
}

const char *MgmtSrvr::get_connect_address(Uint32 node_id)
{
  if (m_connect_address[node_id].s_addr == 0 &&
      theFacade && theFacade->theTransporterRegistry &&
      theFacade->theClusterMgr &&
      getNodeType(node_id) == NDB_MGM_NODE_TYPE_NDB) 
  {
    const ClusterMgr::Node &node=
      theFacade->theClusterMgr->getNodeInfo(node_id);
    if (node.connected)
    {
      m_connect_address[node_id]=
	theFacade->theTransporterRegistry->get_connect_address(node_id);
    }
  }
  return inet_ntoa(m_connect_address[node_id]);  
}

void
MgmtSrvr::get_connected_nodes(NodeBitmask &connected_nodes) const
{
  if (theFacade && theFacade->theClusterMgr) 
  {
    for(Uint32 i = 0; i < MAX_NODES; i++)
    {
      if (getNodeType(i) == NDB_MGM_NODE_TYPE_NDB)
      {
	const ClusterMgr::Node &node= theFacade->theClusterMgr->getNodeInfo(i);
	connected_nodes.bitOR(node.m_state.m_connected_nodes);
      }
    }
  }
}

bool
MgmtSrvr::alloc_node_id(NodeId * nodeId, 
			enum ndb_mgm_node_type type,
			struct sockaddr *client_addr, 
			SOCKET_SIZE_TYPE *client_addr_len,
			BaseString &error_string)
{
  DBUG_ENTER("MgmtSrvr::alloc_node_id");
  DBUG_PRINT("enter", ("nodeid=%d, type=%d, client_addr=%d",
		       *nodeId, type, client_addr));
  if (g_no_nodeid_checks) {
    if (*nodeId == 0) {
      error_string.appfmt("no-nodeid-checks set in management server.\n"
			  "node id must be set explicitly in connectstring");
      DBUG_RETURN(false);
    }
    DBUG_RETURN(true);
  }
  Guard g(m_node_id_mutex);
  int no_mgm= 0;
  NodeBitmask connected_nodes(m_reserved_nodes);
  get_connected_nodes(connected_nodes);
  {
    for(Uint32 i = 0; i < MAX_NODES; i++)
      if (getNodeType(i) == NDB_MGM_NODE_TYPE_MGM)
	no_mgm++;
  }
  bool found_matching_id= false;
  bool found_matching_type= false;
  bool found_free_node= false;
  unsigned id_found= 0;
  const char *config_hostname= 0;
  struct in_addr config_addr= {0};
  int r_config_addr= -1;
  unsigned type_c= 0;

  if(NdbMutex_Lock(m_configMutex))
  {
    error_string.appfmt("unable to lock configuration mutex");
    return false;
  }
  ndb_mgm_configuration_iterator
    iter(* _config->m_configValues, CFG_SECTION_NODE);
  for(iter.first(); iter.valid(); iter.next()) {
    unsigned tmp= 0;
    if(iter.get(CFG_NODE_ID, &tmp)) require(false);
    if (*nodeId && *nodeId != tmp)
      continue;
    found_matching_id= true;
    if(iter.get(CFG_TYPE_OF_SECTION, &type_c)) require(false);
    if(type_c != (unsigned)type)
      continue;
    found_matching_type= true;
    if (connected_nodes.get(tmp))
      continue;
    found_free_node= true;
    if(iter.get(CFG_NODE_HOST, &config_hostname)) require(false);
    if (config_hostname && config_hostname[0] == 0)
      config_hostname= 0;
    else if (client_addr) {
      // check hostname compatability
      const void *tmp_in= &(((sockaddr_in*)client_addr)->sin_addr);
      if((r_config_addr= Ndb_getInAddr(&config_addr, config_hostname)) != 0
	 || memcmp(&config_addr, tmp_in, sizeof(config_addr)) != 0) {
	struct in_addr tmp_addr;
	if(Ndb_getInAddr(&tmp_addr, "localhost") != 0
	   || memcmp(&tmp_addr, tmp_in, sizeof(config_addr)) != 0) {
	  // not localhost
#if 0
	  ndbout << "MgmtSrvr::getFreeNodeId compare failed for \""
		 << config_hostname
		 << "\" id=" << tmp << endl;
#endif
	  continue;
	}
	// connecting through localhost
	// check if config_hostname is local
	if (!SocketServer::tryBind(0,config_hostname)) {
	  continue;
	}
      }
    } else { // client_addr == 0
      if (!SocketServer::tryBind(0,config_hostname)) {
	continue;
      }
    }
    if (*nodeId != 0 ||
	type != NDB_MGM_NODE_TYPE_MGM ||
	no_mgm == 1) { // any match is ok

      if (config_hostname == 0 &&
	  *nodeId == 0 &&
	  type != NDB_MGM_NODE_TYPE_MGM)
      {
	if (!id_found) // only set if not set earlier
	  id_found= tmp;
	continue; /* continue looking for a nodeid with specified
		   * hostname
		   */
      }
      assert(id_found == 0);
      id_found= tmp;
      break;
    }
    if (id_found) { // mgmt server may only have one match
      error_string.appfmt("Ambiguous node id's %d and %d.\n"
			  "Suggest specifying node id in connectstring,\n"
			  "or specifying unique host names in config file.",
			  id_found, tmp);
      NdbMutex_Unlock(m_configMutex);
      DBUG_RETURN(false);
    }
    if (config_hostname == 0) {
      error_string.appfmt("Ambiguity for node id %d.\n"
			  "Suggest specifying node id in connectstring,\n"
			  "or specifying unique host names in config file,\n"
			  "or specifying just one mgmt server in config file.",
			  tmp);
      DBUG_RETURN(false);
    }
    id_found= tmp; // mgmt server matched, check for more matches
  }
  NdbMutex_Unlock(m_configMutex);

  if (id_found)
  {
    *nodeId= id_found;
    DBUG_PRINT("info", ("allocating node id %d",*nodeId));
    {
      int r= 0;
      if (client_addr)
	m_connect_address[id_found]=
	  ((struct sockaddr_in *)client_addr)->sin_addr;
      else if (config_hostname)
	r= Ndb_getInAddr(&(m_connect_address[id_found]), config_hostname);
      else {
	char name[256];
	r= gethostname(name, sizeof(name));
	if (r == 0) {
	  name[sizeof(name)-1]= 0;
	  r= Ndb_getInAddr(&(m_connect_address[id_found]), name);
	}
      }
      if (r)
	m_connect_address[id_found].s_addr= 0;
    }
    m_reserved_nodes.set(id_found);
    char tmp_str[128];
    m_reserved_nodes.getText(tmp_str);
    g_eventLogger.info("Mgmt server state: nodeid %d reserved for ip %s, m_reserved_nodes %s.",
		       id_found, get_connect_address(id_found), tmp_str);
    DBUG_RETURN(true);
  }

  if (found_matching_type && !found_free_node) {
    // we have a temporary error which might be due to that 
    // we have got the latest connect status from db-nodes.  Force update.
    global_flag_send_heartbeat_now= 1;
  }

  BaseString type_string, type_c_string;
  {
    const char *alias, *str;
    alias= ndb_mgm_get_node_type_alias_string(type, &str);
    type_string.assfmt("%s(%s)", alias, str);
    alias= ndb_mgm_get_node_type_alias_string((enum ndb_mgm_node_type)type_c,
					      &str);
    type_c_string.assfmt("%s(%s)", alias, str);
  }

  if (*nodeId == 0) {
    if (found_matching_id)
      if (found_matching_type)
	if (found_free_node)
	  error_string.appfmt("Connection done from wrong host ip %s.",
			      (client_addr)?
			        inet_ntoa(((struct sockaddr_in *)
					 (client_addr))->sin_addr):"");
	else
	  error_string.appfmt("No free node id found for %s.",
			      type_string.c_str());
      else
	error_string.appfmt("No %s node defined in config file.",
			    type_string.c_str());
    else
      error_string.append("No nodes defined in config file.");
  } else {
    if (found_matching_id)
      if (found_matching_type)
	if (found_free_node) {
	  // have to split these into two since inet_ntoa overwrites itself
	  error_string.appfmt("Connection with id %d done from wrong host ip %s,",
			      *nodeId, inet_ntoa(((struct sockaddr_in *)
						  (client_addr))->sin_addr));
	  error_string.appfmt(" expected %s(%s).", config_hostname,
			      r_config_addr ?
			      "lookup failed" : inet_ntoa(config_addr));
	} else
	  error_string.appfmt("Id %d already allocated by another node.",
			      *nodeId);
      else
	error_string.appfmt("Id %d configured as %s, connect attempted as %s.",
			    *nodeId, type_c_string.c_str(),
			    type_string.c_str());
    else
      error_string.appfmt("No node defined with id=%d in config file.",
			  *nodeId);
  }

  g_eventLogger.warning("Allocate nodeid (%d) failed. Connection from ip %s. "
			"Returned error string \"%s\"",
			*nodeId,
			client_addr != 0 ? inet_ntoa(((struct sockaddr_in *)(client_addr))->sin_addr) : "<none>",
			error_string.c_str());

  NodeBitmask connected_nodes2;
  get_connected_nodes(connected_nodes2);
  {
    BaseString tmp_connected, tmp_not_connected;
    for(Uint32 i = 0; i < MAX_NODES; i++)
    {
      if (connected_nodes2.get(i))
      {
	if (!m_reserved_nodes.get(i))
	  tmp_connected.appfmt(" %d", i);
      }
      else if (m_reserved_nodes.get(i))
      {
	tmp_not_connected.appfmt(" %d", i);
      }
    }
    if (tmp_connected.length() > 0)
      g_eventLogger.info("Mgmt server state: node id's %s connected but not reserved", 
			 tmp_connected.c_str());
    if (tmp_not_connected.length() > 0)
      g_eventLogger.info("Mgmt server state: node id's %s not connected but reserved",
			 tmp_not_connected.c_str());
  }
  DBUG_RETURN(false);
}

bool
MgmtSrvr::getNextNodeId(NodeId * nodeId, enum ndb_mgm_node_type type) const 
{
  NodeId tmp = * nodeId;

  tmp++;
  while(nodeTypes[tmp] != type && tmp < MAX_NODES)
    tmp++;
  
  if(tmp == MAX_NODES){
    return false;
  }

  * nodeId = tmp;
  return true;
}

#include "Services.hpp"

void
MgmtSrvr::eventReport(const Uint32 * theData)
{
  const EventReport * const eventReport = (EventReport *)&theData[0];
  
  NodeId nodeId = eventReport->getNodeId();
  Ndb_logevent_type type = eventReport->getEventType();
  // Log event
  g_eventLogger.log(type, theData, nodeId, 
		    &m_event_listner[0].m_logLevel);  
  m_event_listner.log(type, theData, nodeId);
}

/***************************************************************************
 * Backup
 ***************************************************************************/

int
MgmtSrvr::startBackup(Uint32& backupId, int waitCompleted)
{
  SignalSender ss(theFacade);
  ss.lock(); // lock will be released on exit

  bool next;
  NodeId nodeId = 0;
  while((next = getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB)) == true &&
	theFacade->get_node_alive(nodeId) == false);
  
  if(!next) return NO_CONTACT_WITH_DB_NODES;

  SimpleSignal ssig;
  BackupReq* req = CAST_PTR(BackupReq, ssig.getDataPtrSend());
  ssig.set(ss, TestOrd::TraceAPI, BACKUP, GSN_BACKUP_REQ, 
	   BackupReq::SignalLength);
  
  req->senderData = 19;
  req->backupDataLen = 0;
  assert(waitCompleted < 3);
  req->flags = waitCompleted & 0x3;

  BackupEvent event;
  int do_send = 1;
  while (1) {
    if (do_send)
    {
      if (ss.sendSignal(nodeId, &ssig) != SEND_OK) {
	return SEND_OR_RECEIVE_FAILED;
      }
      if (waitCompleted == 0)
	return 0;
      do_send = 0;
    }
    SimpleSignal *signal = ss.waitFor();

    int gsn = signal->readSignalNumber();
    switch (gsn) {
    case GSN_BACKUP_CONF:{
      const BackupConf * const conf = 
	CAST_CONSTPTR(BackupConf, signal->getDataPtr());
      event.Event = BackupEvent::BackupStarted;
      event.Started.BackupId = conf->backupId;
      event.Nodes = conf->nodes;
#ifdef VM_TRACE
      ndbout_c("Backup(%d) master is %d", conf->backupId,
	       refToNode(signal->header.theSendersBlockRef));
#endif
      backupId = conf->backupId;
      if (waitCompleted == 1)
	return 0;
      // wait for next signal
      break;
    }
    case GSN_BACKUP_COMPLETE_REP:{
      const BackupCompleteRep * const rep = 
	CAST_CONSTPTR(BackupCompleteRep, signal->getDataPtr());
#ifdef VM_TRACE
      ndbout_c("Backup(%d) completed %d", rep->backupId);
#endif
      event.Event = BackupEvent::BackupCompleted;
      event.Completed.BackupId = rep->backupId;
    
      event.Completed.NoOfBytes = rep->noOfBytes;
      event.Completed.NoOfLogBytes = rep->noOfLogBytes;
      event.Completed.NoOfRecords = rep->noOfRecords;
      event.Completed.NoOfLogRecords = rep->noOfLogRecords;
      event.Completed.stopGCP = rep->stopGCP;
      event.Completed.startGCP = rep->startGCP;
      event.Nodes = rep->nodes;

      backupId = rep->backupId;
      return 0;
    }
    case GSN_BACKUP_REF:{
      const BackupRef * const ref = 
	CAST_CONSTPTR(BackupRef, signal->getDataPtr());
      if(ref->errorCode == BackupRef::IAmNotMaster){
	nodeId = refToNode(ref->masterRef);
#ifdef VM_TRACE
	ndbout_c("I'm not master resending to %d", nodeId);
#endif
	do_send = 1; // try again
	continue;
      }
      event.Event = BackupEvent::BackupFailedToStart;
      event.FailedToStart.ErrorCode = ref->errorCode;
      return ref->errorCode;
    }
    case GSN_BACKUP_ABORT_REP:{
      const BackupAbortRep * const rep = 
	CAST_CONSTPTR(BackupAbortRep, signal->getDataPtr());
      event.Event = BackupEvent::BackupAborted;
      event.Aborted.Reason = rep->reason;
      event.Aborted.BackupId = rep->backupId;
      event.Aborted.ErrorCode = rep->reason;
#ifdef VM_TRACE
      ndbout_c("Backup %d aborted", rep->backupId);
#endif
      return rep->reason;
    }
    case GSN_NF_COMPLETEREP:{
      const NFCompleteRep * const rep =
	CAST_CONSTPTR(NFCompleteRep, signal->getDataPtr());
#ifdef VM_TRACE
      ndbout_c("Node %d fail completed", rep->failedNodeId);
#endif
      if (rep->failedNodeId == nodeId ||
	  waitCompleted == 1)
	return 1326;
      // wait for next signal
      // master node will report aborted backup
      break;
    }
    case GSN_NODE_FAILREP:{
      const NodeFailRep * const rep =
	CAST_CONSTPTR(NodeFailRep, signal->getDataPtr());
      if (NodeBitmask::get(rep->theNodes,nodeId) ||
	  waitCompleted == 1)
	return 1326;
      // wait for next signal
      // master node will report aborted backup
      break;
    }
    default:
      report_unknown_signal(signal);
      return SEND_OR_RECEIVE_FAILED;
    }
  }
}

int 
MgmtSrvr::abortBackup(Uint32 backupId)
{
  SignalSender ss(theFacade);

  bool next;
  NodeId nodeId = 0;
  while((next = getNextNodeId(&nodeId, NDB_MGM_NODE_TYPE_NDB)) == true &&
	theFacade->get_node_alive(nodeId) == false);
  
  if(!next){
    return NO_CONTACT_WITH_DB_NODES;
  }
  
  SimpleSignal ssig;

  AbortBackupOrd* ord = CAST_PTR(AbortBackupOrd, ssig.getDataPtrSend());
  ssig.set(ss, TestOrd::TraceAPI, BACKUP, GSN_ABORT_BACKUP_ORD, 
	   AbortBackupOrd::SignalLength);
  
  ord->requestType = AbortBackupOrd::ClientAbort;
  ord->senderData = 19;
  ord->backupId = backupId;
  
  return ss.sendSignal(nodeId, &ssig) == SEND_OK ? 0 : SEND_OR_RECEIVE_FAILED;
}


/*****************************************************************************
 * Global Replication
 *****************************************************************************/

int
MgmtSrvr::repCommand(Uint32* repReqId, Uint32 request, bool waitCompleted)
{
  require(false);
  return 0;
}

MgmtSrvr::Allocated_resources::Allocated_resources(MgmtSrvr &m)
  : m_mgmsrv(m)
{
}

MgmtSrvr::Allocated_resources::~Allocated_resources()
{
  Guard g(m_mgmsrv.m_node_id_mutex);
  if (!m_reserved_nodes.isclear()) {
    m_mgmsrv.m_reserved_nodes.bitANDC(m_reserved_nodes); 
    // node has been reserved, force update signal to ndb nodes
    global_flag_send_heartbeat_now= 1;

    char tmp_str[128];
    m_mgmsrv.m_reserved_nodes.getText(tmp_str);
    g_eventLogger.info("Mgmt server state: nodeid %d freed, m_reserved_nodes %s.",
		       get_nodeid(), tmp_str);
  }
}

void
MgmtSrvr::Allocated_resources::reserve_node(NodeId id)
{
  m_reserved_nodes.set(id);
}

NodeId
MgmtSrvr::Allocated_resources::get_nodeid() const
{
  for(Uint32 i = 0; i < MAX_NODES; i++)
  {
    if (m_reserved_nodes.get(i))
      return i;
  }
  return 0;
}

int
MgmtSrvr::setDbParameter(int node, int param, const char * value,
			 BaseString& msg){

  if(NdbMutex_Lock(m_configMutex))
    return -1;

  /**
   * Check parameter
   */
  ndb_mgm_configuration_iterator
    iter(* _config->m_configValues, CFG_SECTION_NODE);
  if(iter.first() != 0){
    msg.assign("Unable to find node section (iter.first())");
    NdbMutex_Unlock(m_configMutex);
    return -1;
  }
  
  Uint32 type = NODE_TYPE_DB + 1;
  if(node != 0){
    if(iter.find(CFG_NODE_ID, node) != 0){
      msg.assign("Unable to find node (iter.find())");
      NdbMutex_Unlock(m_configMutex);
      return -1;
    }
    if(iter.get(CFG_TYPE_OF_SECTION, &type) != 0){
      msg.assign("Unable to get node type(iter.get(CFG_TYPE_OF_SECTION))");
      NdbMutex_Unlock(m_configMutex);
      return -1;
    }
  } else {
    do {
      if(iter.get(CFG_TYPE_OF_SECTION, &type) != 0){
	msg.assign("Unable to get node type(iter.get(CFG_TYPE_OF_SECTION))");
	NdbMutex_Unlock(m_configMutex);
	return -1;
      }
      if(type == NODE_TYPE_DB)
	break;
    } while(iter.next() == 0);
  }
  
  if(type != NODE_TYPE_DB){
    msg.assfmt("Invalid node type or no such node (%d %d)", 
	       type, NODE_TYPE_DB);
    NdbMutex_Unlock(m_configMutex);
    return -1;
  }

  int p_type;
  unsigned val_32;
  Uint64 val_64;
  const char * val_char;
  do {
    p_type = 0;
    if(iter.get(param, &val_32) == 0){
      val_32 = atoi(value);
      break;
    }
    
    p_type++;
    if(iter.get(param, &val_64) == 0){
      val_64 = strtoll(value, 0, 10);
      break;
    }
    p_type++;
    if(iter.get(param, &val_char) == 0){
      val_char = value;
      break;
    }
    msg.assign("Could not get parameter");
    NdbMutex_Unlock(m_configMutex);
    return -1;
  } while(0);
  
  bool res = false;
  do {
    int ret = iter.get(CFG_TYPE_OF_SECTION, &type);
    assert(ret == 0);
    
    if(type != NODE_TYPE_DB)
      continue;
    
    Uint32 node;
    ret = iter.get(CFG_NODE_ID, &node);
    assert(ret == 0);
    
    ConfigValues::Iterator i2(_config->m_configValues->m_config, 
			      iter.m_config);
    switch(p_type){
    case 0:
      res = i2.set(param, val_32);
      ndbout_c("Updating node %d param: %d to %d",  node, param, val_32);
      break;
    case 1:
      res = i2.set(param, val_64);
      ndbout_c("Updating node %d param: %d to %Ld",  node, param, val_32);
      break;
    case 2:
      res = i2.set(param, val_char);
      ndbout_c("Updating node %d param: %d to %s",  node, param, val_char);
      break;
    default:
      require(false);
    }
    assert(res);
  } while(node == 0 && iter.next() == 0);

  msg.assign("Success");
  NdbMutex_Unlock(m_configMutex);
  return 0;
}
int
MgmtSrvr::setConnectionDbParameter(int node1, 
				   int node2,
				   int param,
				   int value,
				   BaseString& msg){
  Uint32 current_value,new_value;

  DBUG_ENTER("MgmtSrvr::setConnectionDbParameter");

  if(NdbMutex_Lock(m_configMutex))
  {
    DBUG_RETURN(-1);
  }

  ndb_mgm_configuration_iterator 
    iter(* _config->m_configValues, CFG_SECTION_CONNECTION);

  if(iter.first() != 0){
    msg.assign("Unable to find connection section (iter.first())");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-1);
  }

  for(;iter.valid();iter.next()) {
    Uint32 n1,n2;
    iter.get(CFG_CONNECTION_NODE_1, &n1);
    iter.get(CFG_CONNECTION_NODE_2, &n2);
    if((n1 == (unsigned)node1 && n2 == (unsigned)node2)
       || (n1 == (unsigned)node2 && n2 == (unsigned)node1))
      break;
  }
  if(!iter.valid()) {
    msg.assign("Unable to find connection between nodes");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-2);
  }
  
  if(iter.get(param, &current_value) != 0) {
    msg.assign("Unable to get current value of parameter");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-3);
  }

  ConfigValues::Iterator i2(_config->m_configValues->m_config, 
			    iter.m_config);

  if(i2.set(param, (unsigned)value) == false) {
    msg.assign("Unable to set new value of parameter");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-4);
  }
  
  if(iter.get(param, &new_value) != 0) {
    msg.assign("Unable to get parameter after setting it.");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-5);
  }

  msg.assfmt("%u -> %u",current_value,new_value);
  NdbMutex_Unlock(m_configMutex);
  DBUG_RETURN(1);
}


int
MgmtSrvr::getConnectionDbParameter(int node1, 
				   int node2,
				   int param,
				   int *value,
				   BaseString& msg){
  DBUG_ENTER("MgmtSrvr::getConnectionDbParameter");

  if(NdbMutex_Lock(m_configMutex))
  {
    DBUG_RETURN(-1);
  }

  ndb_mgm_configuration_iterator
    iter(* _config->m_configValues, CFG_SECTION_CONNECTION);

  if(iter.first() != 0){
    msg.assign("Unable to find connection section (iter.first())");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-1);
  }

  for(;iter.valid();iter.next()) {
    Uint32 n1=0,n2=0;
    iter.get(CFG_CONNECTION_NODE_1, &n1);
    iter.get(CFG_CONNECTION_NODE_2, &n2);
    if((n1 == (unsigned)node1 && n2 == (unsigned)node2)
       || (n1 == (unsigned)node2 && n2 == (unsigned)node1))
      break;
  }
  if(!iter.valid()) {
    msg.assign("Unable to find connection between nodes");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-1);
  }
  
  if(iter.get(param, (Uint32*)value) != 0) {
    msg.assign("Unable to get current value of parameter");
    NdbMutex_Unlock(m_configMutex);
    DBUG_RETURN(-1);
  }

  msg.assfmt("%d",*value);
  NdbMutex_Unlock(m_configMutex);
  DBUG_RETURN(1);
}

void MgmtSrvr::transporter_connect(NDB_SOCKET_TYPE sockfd)
{
  if (theFacade->get_registry()->connect_server(sockfd))
  {
    /**
     * Force an update_connections() so that the
     * ClusterMgr and TransporterFacade is up to date
     * with the new connection.
     * Important for correct node id reservation handling
     */
    NdbMutex_Lock(theFacade->theMutexPtr);
    theFacade->get_registry()->update_connections();
    NdbMutex_Unlock(theFacade->theMutexPtr);
  }
}

int MgmtSrvr::set_connect_string(const char *str)
{
  return ndb_mgm_set_connectstring(m_config_retriever->get_mgmHandle(),str);
}



template class MutexVector<unsigned short>;
template class MutexVector<Ndb_mgmd_event_service::Event_listener>;
template class MutexVector<EventSubscribeReq>;
