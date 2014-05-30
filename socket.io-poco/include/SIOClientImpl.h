#pragma once

#include <string>
#include <memory>

#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/WebSocket.h"
#include "Poco/Logger.h"
#include "Poco/Timer.h"
#include "Poco/NotificationCenter.h"
#include "Poco/Thread.h"
#include "Poco/ThreadTarget.h"
#include "Poco/RunnableAdapter.h"
#include "Poco/Util/Timer.h"
#include "Poco/Util/TimerTask.h"

#include "Poco/JSON/Parser.h"

#include "SIONotificationHandler.h"
#include "SIOEventRegistry.h"
#include "SIOEventTarget.h"


using Poco::JSON::Object;

using Poco::Net::HTTPClientSession;
using Poco::Net::WebSocket;
using Poco::Logger;
using Poco::Timer;
using Poco::TimerCallback;
using Poco::NotificationCenter;
using Poco::Thread;
using Poco::ThreadTarget;


class SIOClientImpl: public Poco::Runnable
{
private:
	SIOClientImpl();
	SIOClientImpl(std::string host, int port, std::string query="");
	~SIOClientImpl(void);

	std::string _sid;
	int _heartbeat_timeout;
	int _timeout;
	std::string _host;
	int _port;
	std::string _uri;
	bool _connected;
	std::string _query;

	HTTPClientSession *_session = NULL;
	WebSocket *_ws;
	Timer *_heartbeatTimer;
	Logger *_logger;
	Thread _thread;

	int _refCount;
	Poco::Timestamp _lastHeartbeat;

	void onAttemptConnect(Poco::Timer& timer);

	Poco::Util::Timer _timer;

	//SIOEventRegistry* _registry;
	//SIONotificationHandler *_sioHandler;

	struct ConnectTask : public Poco::Util::TimerTask {
		SIOClientImpl* sio;
		int _connectWait;

		ConnectTask(SIOClientImpl* sio, int wait = 1):sio(sio),_connectWait(wait){
		}
		void run();
	};

public:

	bool handshake();
	bool openSocket();
	void init();

	void release();
	void addref();

	static SIOClientImpl* connect(std::string host, int port, const std::string& query="");
	void disconnect(std::string endpoint);
	void connectToEndpoint(std::string endpoint);
	void monitor();
	virtual void run();
	void heartbeat(Poco::Timer& timer);
	bool receive();
	void send(std::string endpoint, std::string s);
	void emit(std::string endpoint, std::string eventname, std::string args);

	std::string getUri();
};
