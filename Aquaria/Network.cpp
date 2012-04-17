#include "minihttp.h"
#include "DSQ.h"
#include "Network.h"
#include "ByteBuffer.h"
//#include "VFSTools.h"
#include "MT.h"
#include <map>
#include <set>
#include <cassert>
#include "SDL.h"

using namespace minihttp;

namespace Network {


static LockedQueue<RequestData*> doneRequests;


class HttpDumpSocket : public HttpSocket
{
public:

	virtual ~HttpDumpSocket() {}

protected:
	virtual void _OnClose()
	{
		puts("_OnClose()");
		minihttp::HttpSocket::_OnClose();
	}
	virtual void _OnOpen()
	{
		puts("_OnOpen()");
		minihttp::HttpSocket::_OnOpen();

		const Request& r = GetCurrentRequest();
		// TODO ??
	}

	virtual void _OnRequestDone()
	{
		const Request& r = GetCurrentRequest();
		RequestData *data = (RequestData*)(r.user);
		printf("_OnRequestDone(): %s\n", r.resource.c_str());
		if(data->fp)
		{
			fclose(data->fp);
			data->fp = NULL;
		}
		if(rename(data->tempFilename.c_str(), data->finalFilename.c_str()))
		{
			perror("SOCKET: _OnRequestDone() failed to rename file");
			data->fail = true;
		}
		
		doneRequests.push(data);
		// TODO: handle failed requests?
	}

	virtual void _OnRecv(char *buf, unsigned int size)
	{
		if(!size)
			return;
		/*if(GetStatusCode() != 200)
		{
			printf("NETWORK: Got %u bytes with status code %u", size, GetStatusCode());
			return;
		}*/
		const Request& r = GetCurrentRequest();
		RequestData *data = (RequestData*)(r.user);
		if(!data->fp && !data->fail)
		{
			data->fp = fopen(data->tempFilename.c_str(), "wb");
			if(!data->fp)
			{
				fprintf(stderr, "SOCKET: Failed to save %u bytes, file not open");
				data->fail = true;
				// TODO: and now?
				return;
			}
		}
		fwrite(buf, 1, size, data->fp);
	}
};

// for first-time init, and signal to shut down worker thread
static volatile bool netUp = false;

// Used when sending a HTTP request
static std::string userAgent;

// socket updater thread
static SDL_Thread *worker = NULL;

// Request Queue (filled by download(), emptied by the worker thread)
static LockedQueue<RequestData*> RQ;

static int _NetworkWorkerThread(void *); // pre-decl

bool init()
{
	if(netUp)
		return true;

	puts("NETWORK: Init");

	std::ostringstream os;
	os << "Aquaria";
#ifdef AQUARIA_DEMO
	os << " Demo";
#endif
	os << " v" << VERSION_MAJOR << "." << VERSION_MINOR << "." << VERSION_REVISION;
#ifdef AQUARIA_CUSTOM_BUILD_ID
	os << AQUARIA_CUSTOM_BUILD_ID;
#endif

	userAgent = os.str();

	if(!worker)
		worker = SDL_CreateThread(_NetworkWorkerThread, NULL);

	return true;
}

void shutdown()
{
	if(netUp)
	{
		netUp = false;
		puts("NETWORK: Waiting for thread to exit...");
		SDL_WaitThread(worker, NULL);
		worker = NULL;
	}
}

// stores all sockets currently in use
// Accessed by worker thread ONLY!
static minihttp::SocketSet sockets;

static HttpDumpSocket *th_GetSocketForHost(const std::string& id)
{
	// TODO: write me
	return NULL;
}

static HttpDumpSocket *th_GetIdleSocket()
{
	// TODO: write me
	return NULL;
}

static HttpDumpSocket *th_CreateSocket()
{
	HttpDumpSocket *sock = new HttpDumpSocket;
	sock->SetAlwaysHandle(false); // only handle incoming data on success
	sock->SetBufsizeIn(1024 * 64); // FIXME
	sock->SetKeepAlive(0); // FIXME
	sock->SetNonBlocking(true);
	sock->SetUserAgent(userAgent);
	sock->SetFollowRedirect(true);
	sockets.add(sock, true); // FIXME: keep sockets alive even if closed
	return sock;
}


// must only be run by _NetworkWorkerThread
static void th_DoSendRequest(RequestData *rq)
{
	std::string host, file;
	int port;
	SplitURI(rq->url, host, file, port);
	if(port < 0)
		port = 80;

	std::ostringstream hostdesc;
	hostdesc << host << ':' << port;

	HttpDumpSocket *sock = th_GetSocketForHost(hostdesc.str());
	if(!sock)
		sock = th_GetIdleSocket();
	if(!sock)
		sock = th_CreateSocket();
	// TODO: keep a sane max. limit of sockets

	sock->SendGet(file, rq);
}

static int _NetworkWorkerThread(void *)
{
	// Init & shutdown networking from the same thread.
	// I vaguely remember this could cause trouble on win32 otherwise. -- fg
	if(!(netUp = InitNetwork()))
	{
		fprintf(stderr, "NETWORK: Failed to init network\n");
		return -1;
	}

	RequestData *rq;
	while(netUp)
	{
		while(RQ.popIfPossible(rq))
		{
			th_DoSendRequest(rq);
		}
		while(sockets.update()) {}
		SDL_Delay(10);
	}
	puts("Network worker thread exiting");
	StopNetwork();
	return 0;
}

void download(RequestData *rq)
{
	RQ.push(rq);
}

void update()
{
	RequestData *rq;
	while(doneRequests.popIfPossible(rq))
		rq->notify();
}


} // namespace Network