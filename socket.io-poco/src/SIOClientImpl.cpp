#include "SIOClientImpl.h"

#include <Poco/ConsoleChannel.h>
#include <Poco/Net/HTMLForm.h>
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/HTTPResponse.h"
#include "Poco/Net/HTTPMessage.h"
#include "Poco/Net/WebSocket.h"
#include "Poco/Net/NetException.h"
#include "Poco/Net/SocketAddress.h"
#include "Poco/StreamCopier.h"
#include "Poco/Format.h"
#include "Poco/Timer.h"
#include "Poco/Timespan.h"
#include <iostream>
#include <sstream>
#include <limits>
#include "Poco/StringTokenizer.h"
#include "Poco/String.h"
#include "Poco/RunnableAdapter.h"
#include "Poco/URI.h"

#include "SIONotifications.h"
#include "SIOClientRegistry.h"
#include "SIOClient.h"

using Poco::Net::HTMLForm;
using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPMessage;
using Poco::Net::NetException;
using Poco::Net::SocketAddress;
using Poco::StreamCopier;
using Poco::StringTokenizer;
using Poco::cat;
using Poco::UInt16;
using Poco::Timer;
using Poco::TimerCallback;
using Poco::Dynamic::Var;
using Poco::Net::WebSocket;
using Poco::URI;


SIOClientImpl::SIOClientImpl() {
	SIOClientImpl("localhost", 3000);
}

SIOClientImpl::SIOClientImpl(std::string host, int port, std::string query) :
	_port(port),
	_host(host),
	_refCount(0),
	_query(query)
{
	std::stringstream s;
	s << host << ":" << port;
	_uri = s.str();
	_ws = NULL;

}

SIOClientImpl::~SIOClientImpl(void) {

	_thread.join();

	disconnect("");

	_ws->shutdown();
	delete(_ws);

	delete(_heartbeatTimer);
	delete(_session);

	_timer.cancel();

	SIOClientRegistry::instance()->removeSocket(_uri);
}

void SIOClientImpl::ConnectTask::run(){
	Logger* _logger = &(Logger::get("SIOClientLog"));
	try{
		_logger->debug("SIOClientImpl::onAttemptConnect");
		if(sio->handshake() && sio->openSocket()){
			_logger->debug("Success");
			return;
		}else{
			// Exponential Backoff with max of 4 mins
			_connectWait *= 2;
			if(_connectWait > 240){
				_connectWait = 240;
			}
			_logger->debug("Waiting: "+std::to_string(_connectWait));
			Poco::Timestamp time = Poco::Timestamp() + _connectWait*1000000;
			Poco::Util::TimerTask::Ptr task = new ConnectTask(sio, _connectWait);
			sio->_timer.schedule(task, time);
		}
	}catch(std::exception e){
		_logger->error(e.what());
	}catch(...){
		_logger->error("Unknown error");
	}
}

void SIOClientImpl::init() {
	_logger = &(Logger::get("SIOClientLog"));

	_logger->debug("SIOClientImpl::init");

	Poco::Util::TimerTask::Ptr task = new ConnectTask(this);
	_timer.schedule(task, Poco::Timestamp());

}

bool SIOClientImpl::handshake() {
	_logger->debug("SIOClientImpl::handshake");
	UInt16 aport = _port;
	if(_session == NULL)
		_session = new HTTPClientSession(_host, aport);
	//_session->setTimeout(Poco::Timespan(4000000));

	std::string url("/socket.io/1");
	if(!_query.empty()){
		url += "?"+_query;
	}
	HTTPRequest req(HTTPRequest::HTTP_POST,url,HTTPMessage::HTTP_1_1);

	try {
		HTMLForm form;

		form.prepareSubmit(req);
		form.write(_session->sendRequest(req));
		HTTPResponse res;
		std::istream& rs = _session->receiveResponse(res);

		std::cout << res.getStatus() << " " << res.getReason() << std::endl;

		if (res.getStatus() == 200)
		{
			std::string temp;

			StreamCopier::copyToString(rs, temp);

			_logger->information("response: %s\n",temp);

			StringTokenizer msg(temp, ":");

			_logger->information("session: %s",msg[0]);
			_logger->information("heartbeat: %s",msg[1]);
			_logger->information("timeout: %s",msg[2]);
			_logger->information("transports: %s",msg[3]);

			_sid = msg[0];
			_heartbeat_timeout = atoi(msg[1].c_str());
			_timeout = atoi(msg[2].c_str());

			return true;
		}
	} catch (std::exception &e) {
		_logger->error(std::string("error: ") + e.what());
	}

	return false;
}

bool SIOClientImpl::openSocket() {
	_logger->debug("SIOClientImpl::openSocket");
	UInt16 aport = _port;
	HTTPRequest req(HTTPRequest::HTTP_GET,"/socket.io/1/websocket/"+_sid,HTTPMessage::HTTP_1_1);
	HTTPResponse res;

	do {
		try {

			_ws = new WebSocket(*_session, req, res);

		}
		catch(NetException ne) {
			std::cout << ne.displayText() << " : " << ne.code() << " - " << ne.what() << "\n";

			if(_ws) {
				delete _ws;
				_ws = NULL;
			}
			Poco::Thread::sleep(2000);

		}
	} while(_ws == NULL);

	_logger->information("WebSocket Created\n");

	_connected = true;
	_lastHeartbeat = Poco::Timestamp();

	int hbInterval = this->_heartbeat_timeout*.75*1000;
	_heartbeatTimer = new Timer(hbInterval, hbInterval);
	TimerCallback<SIOClientImpl> heartbeat(*this, &SIOClientImpl::heartbeat);
	_heartbeatTimer->start(heartbeat);

	_thread.start(*this);

	return _connected;

}


SIOClientImpl* SIOClientImpl::connect(std::string host, int port, const std::string& query) {

	SIOClientImpl *s = new SIOClientImpl(host, port, query);

	if(s) {
		s->init();
		return s;
	}

	return NULL;
}

void SIOClientImpl::disconnect(std::string endpoint) {
	std::string s = "0::" + endpoint;

	if(endpoint == "") {

		_heartbeatTimer->stop();
		_connected = false;

	}

	_ws->sendFrame(s.data(), s.size());
}

void SIOClientImpl::connectToEndpoint(std::string endpoint) {

	std::string s = "1::" + endpoint;

	_ws->sendFrame(s.data(), s.size());

}

void SIOClientImpl::heartbeat(Poco::Timer& timer) {
	_logger->information("heartbeat called\n");
	bool shouldDisconnect = false;
	try{

		std::string s = "2::";

		_ws->sendFrame(s.data(), s.size());

		// Check if client timed out
		if(_lastHeartbeat.isElapsed(this->_heartbeat_timeout*2*1000000)){
			shouldDisconnect = true;
		}
	}catch(...){
		shouldDisconnect = true;
	}
	if(shouldDisconnect){
		disconnect("");
		init();
	}
}

void SIOClientImpl::run() {

	monitor();

}

void SIOClientImpl::monitor() {
	do
	{
		receive();

	} while (_connected);
}

void SIOClientImpl::send(std::string endpoint, std::string s) {
	_logger->information("sending message\n");

	std::stringstream pre;

	pre << "3::" << endpoint << ":" << s;

	std::string msg = pre.str();

	_ws->sendFrame(msg.data(), msg.size());

}

void SIOClientImpl::emit(std::string endpoint, std::string eventname, std::string args) {
	_logger->information("emitting event\n");

	std::stringstream pre;

	pre << "5::" << endpoint << ":{\"name\":\"" << eventname << "\",\"args\":" << args << "}";

	_logger->information("event data: %s\n", pre.str());

	std::string msg = pre.str();

	_ws->sendFrame(msg.data(), msg.size());

}

bool SIOClientImpl::receive() {

	char buffer[2048];
	int flags;
	int n;

	n = _ws->receiveFrame(buffer, sizeof(buffer), flags);
	_logger->information("bytes received: %d ",n);

	std::stringstream s;
	for(int i = 0; i < n; i++) {
		s << buffer[i];
	}

	_logger->information("buffer received: \"%s\"\n",s.str());

	int control = 0;
	if(n > 0)
		control = atoi(&buffer[0]);
	StringTokenizer st(s.str(), ":");
	std::string endpoint;
	if(st.count() >= 3)
		endpoint = st[2];

	std::string uri = _uri;
	uri += endpoint;

	SIOClient *c = SIOClientRegistry::instance()->getClient(uri);

	std::string payload = "";

	// If we received any message reset heartbeat timer
	_lastHeartbeat = Poco::Timestamp();

	switch(control) {
		case 0:
			_logger->information("Socket Disconnected\n");
			disconnect("");
			// Try to reconnect
			init();
			break;
		case 1:
			_logger->information("Connected to endpoint: %s \n", st[2]);
			break;
		case 2:
			_logger->information("Heartbeat received\n");
			break;
		case 3:
			_logger->information("Message received\n");

			c->getNCenter()->postNotification(new SIOMessage(st[3]));
			break;
		case 4:
			_logger->information("JSON Message Received\n");

			c->getNCenter()->postNotification(new SIOJSONMessage(st[3]));
			break;
		case 5:
			_logger->information("Event Dispatched\n");

			for(int i = 3; i < st.count(); i++)
			{
				if(i != 3) payload += ":";
				payload += st[i];
			}

			c->getNCenter()->postNotification(new SIOEvent(c, payload));
			break;
		case 6:
			_logger->information("Message Ack\n");
			break;
		case 7:
			_logger->information("Error\n");
			break;
		case 8:
			_logger->information("Noop\n");
			break;
	}

	return true;

}

void SIOClientImpl::addref() {
	_refCount++;
}

void SIOClientImpl::release() {
	if(--_refCount == 0) delete this;
}
