/*
obs-ssp
 Copyright (C) 2019-2020 Yibai Zhang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; If not, see <https://www.gnu.org/licenses/>
*/

#include <obs.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <memory>  // For std::unique_ptr

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

#include <QUuid>
#include <QFileInfo>
#include <QDir>
#include <pthread.h>

#include "obs-ssp.h"
#include "ssp-client-iso.h"

static size_t zc_process_pipe_read(zc_process_pipe_t *pipe, uint8_t *dst,
				   size_t size)
{
#if defined(__APPLE__)
	return zc_mac_process_pipe_read(pipe, dst, size);
#else
	return os_process_pipe_read(pipe, dst, size);
#endif
}

static size_t zc_process_pipe_read_err(zc_process_pipe_t *pipe, uint8_t *dst,
				       size_t size)
{
#if defined(__APPLE__)
	return zc_mac_process_pipe_read_err(pipe, dst, size);
#else
	return os_process_pipe_read_err(pipe, dst, size);
#endif
}

static zc_process_pipe_t *zc_process_pipe_create(const char *command,
						 const char *mode,
						 char *error,
						 size_t error_size)
{
#if defined(__APPLE__)
	return zc_mac_process_pipe_create(command, mode, error, error_size);
#else
	(void)error;
	(void)error_size;
	return os_process_pipe_create(command, mode);
#endif
}

static int zc_process_pipe_destroy(zc_process_pipe_t *pipe)
{
#if defined(__APPLE__)
	return zc_mac_process_pipe_destroy(pipe);
#else
	return os_process_pipe_destroy(pipe);
#endif
}

static size_t zc_process_pipe_read_retry(zc_process_pipe_t *pipe, uint8_t *dst,
					 size_t size)
{
	size_t pos = 0, cur = 0;
	while (pos < size) {
		cur = zc_process_pipe_read(pipe, dst + pos, size - pos);
		if (!cur) {
			break;
		}
		pos += cur;
	}
	return pos;
}

/* Upper bound on one framed message body. Even a large H.264/HEVC access unit
   is well under a few MB, so a larger `length` means the pipe stream is corrupt;
   trusting it would either allocate/copy an arbitrary amount or (if bmalloc
   fails) memcpy into a NULL pointer. */
#define ZC_MAX_MSG_BODY (64u * 1024u * 1024u)

static Message *msg_recv(zc_process_pipe_t *pipe)
{
	size_t sz = 0;
	Message *msg = (Message *)bmalloc(sizeof(Message));
	if (!msg) {
		return nullptr;
	}
	sz = zc_process_pipe_read_retry(pipe, (uint8_t *)msg,
					sizeof(Message));
	if (sz != sizeof(Message)) {
		ssp_blog(LOG_WARNING, "pipe protocol header error, recv: %zu!",
			 sz);
		bfree(msg);
		return nullptr;
	}
	if (msg->length == 0) {
		return msg;
	}
	if (msg->length > ZC_MAX_MSG_BODY) {
		ssp_blog(LOG_WARNING,
			 "pipe protocol length %u exceeds limit, dropping!",
			 msg->length);
		bfree(msg);
		return nullptr;
	}
	Message *msg_all = nullptr;
	msg_all = (Message *)bmalloc(sizeof(Message) + msg->length);
	if (!msg_all) {
		bfree(msg);
		return nullptr;
	}
	memcpy(msg_all, msg, sizeof(Message));
	bfree(msg);
	//ssp_blog(LOG_INFO, "receive msg type: %d, size: %d", msg_all->type, msg_all->length);
	sz = zc_process_pipe_read_retry(pipe, msg_all->value, msg_all->length);
	if (sz != msg_all->length) {
		ssp_blog(LOG_WARNING, "pipe protocol body error, recv: %zu!",
			 sz);
		bfree(msg_all);
		return nullptr;
	}
	return msg_all;
}

static void msg_free(Message *msg)
{
	if (msg) {
		bfree(msg);
	}
}

static void *dump_stderr(zc_process_pipe_t *pipe)
{
	size_t sz;
	char buf[1024];
	while (true) {
		sz = zc_process_pipe_read_err(pipe, (uint8_t *)buf,
					      sizeof(buf) - 1);
		if (sz == 0) {
			break;
		}
		buf[sz] = '\0';
		ssp_blog(LOG_INFO, "%s", buf);
	}
	ssp_blog(LOG_INFO, "read thread exited");
	return nullptr;
}

#ifdef _WIN32
/* libobs keeps `struct os_process_pipe` opaque — it is only defined in
   libobs/util/pipe-windows.c — so there is no public way to reach the child
   process handle. Stop() needs it to terminate a connector that is alive but
   silent (the very stall the watchdog exists for): such a child never closes
   its pipe ends, so this side's blocking ReadFile never returns and both
   worker.join() and os_process_pipe_destroy()'s INFINITE wait would hang OBS
   forever. The layout below mirrors pipe-windows.c for the current OBS; only
   `process` is read. A layout drift is harmless — TerminateProcess() simply
   rejects a handle that lacks PROCESS_TERMINATE access instead of crashing. */
struct zc_process_pipe_win {
	bool read_pipe;
	HANDLE handle;
	HANDLE handle_err;
	HANDLE process;
};

static HANDLE pipe_child_process(os_process_pipe *pp)
{
	return reinterpret_cast<zc_process_pipe_win *>(pp)->process;
}
#endif

SSPClientIso::SSPClientIso(const std::string &ip)
{
	this->ip = ip;
	this->running = false;
	this->pipe = nullptr;

#if defined(__APPLE__)
	Dl_info info;
	dladdr((const void *)msg_free, &info);
	QFileInfo plugin_path(info.dli_fname);
	ssp_connector_path =
		plugin_path.dir().filePath(QStringLiteral(SSP_CONNECTOR));
#elif defined(_WIN32)
	/* Resolve the connector relative to THIS module (obs-zcamera.dll), not
	   relative to OBS's working directory. The latter would resolve
	   `../../obs-plugins/64bit` from OBS's CWD and silently launch the legacy
	   Program Files connector / libssp instead of the copies bundled with this
	   plugin. Use the address of a symbol in this DLL to find its own path. */
	HMODULE self = nullptr;
	GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&msg_free), &self);
	char dllPath[MAX_PATH] = {};
	if (self && GetModuleFileNameA(self, dllPath, MAX_PATH) > 0) {
		QFileInfo dll(dllPath);
		ssp_connector_path =
			dll.dir().filePath(QStringLiteral("ssp-connector.exe"));
	} else {
		/* Fallback to the legacy OBS-relative layout. */
		ssp_connector_path = QStringLiteral("../../obs-plugins/64bit/ssp-connector.exe");
	}
#endif
	connect(this, SIGNAL(Start()), this, SLOT(doStart()));
}

SSPClientIso::~SSPClientIso()
{
	// Ensure we stop the client and clean up resources
	Stop();
	
	// Disconnect all signals to prevent callbacks after destruction
	disconnect(this, SIGNAL(Start()), this, SLOT(doStart()));
	
	// Clear all callbacks to prevent dangling references
	bufferFullCallback = nullptr;
	audioDataCallback = nullptr;
	metaCallback = nullptr;
	disconnectedCallback = nullptr;
	connectedCallback = nullptr;
	h264DataCallback = nullptr;
	exceptionCallback = nullptr;
}

using namespace std::placeholders;

void SSPClientIso::doStart()
{
	struct dstr cmd;

	dstr_init_copy(&cmd, ssp_connector_path.toStdString().c_str());
	dstr_insert_ch(&cmd, 0, '\"');
	dstr_cat(&cmd, "\" ");
	dstr_cat(&cmd, "--host ");
	/* Quote the host: the command is passed to a shell (libobs' pipe API on
	   Windows, /bin/sh in the macOS helper), so an unquoted, attacker-
	   controlled zc_source_ip from a shared scene collection would inject
	   shell. */
	dstr_cat_ch(&cmd, '"');
	dstr_cat(&cmd, this->ip.c_str());
	dstr_cat_ch(&cmd, '"');
	dstr_cat(&cmd, " --port ");
	dstr_cat(&cmd, "9999");

	char spawn_error[256] = {};
	auto tpipe = zc_process_pipe_create(cmd.array, "r", spawn_error,
					    sizeof(spawn_error));
	blog(LOG_INFO, "Start ssp-connector at: %s", cmd.array);
	dstr_free(&cmd);

	if (!tpipe) {
		blog(LOG_WARNING, "Start ssp-connector failed%s%s",
		     spawn_error[0] ? ": " : "",
		     spawn_error[0] ? spawn_error : "");
		return;
	}
	this->statusLock.lock();
	this->running = true;
	this->pipe = tpipe;

	this->worker = std::thread(SSPClientIso::ReceiveThread, this);
#if defined(_WIN32) || defined(__APPLE__)
	/* Keep the connector's stderr drained. Both Windows and macOS stop the
	   child before joining this reader, so it cannot block forever on a
	   connector that is alive but silent. */
	this->stderrWorker = std::thread(dump_stderr, tpipe);
#endif
	this->statusLock.unlock();
}

void *SSPClientIso::ReceiveThread(void *arg)
{
	auto th = (SSPClientIso *)arg;
	Message *msg = nullptr;
	th->statusLock.lock();
	auto pipe = th->pipe;
	th->statusLock.unlock();

	// Use RAII to ensure message is freed
	std::unique_ptr<Message, decltype(&msg_free)> initial_msg(msg_recv(pipe), msg_free);
	if (!initial_msg) {
		blog(LOG_WARNING, "%s Receive error !", th->getIp().c_str());
		return nullptr;
	}
	if (initial_msg->type != MessageType::ConnectorOkMsg) {
		blog(LOG_WARNING, "%s Protocol error !", th->getIp().c_str());
		return nullptr;
	}

	while (th->running) {
		// Use RAII to ensure message is freed
		std::unique_ptr<Message, decltype(&msg_free)> msg(msg_recv(pipe), msg_free);
		if (!msg) {
			blog(LOG_WARNING, "%s Receive error !", th->getIp().c_str());
			break;
		}

		switch (msg->type) {
		case MessageType::MetaDataMsg:
			th->OnMetadata((Metadata *)msg->value);
			break;
		case MessageType::VideoDataMsg:
			th->OnH264Data((VideoData *)msg->value);
			break;
		case MessageType::AudioDataMsg:
			th->OnAudioData((AudioData *)msg->value);
			break;
		case MessageType::RecvBufferFullMsg:
			th->OnRecvBufferFull();
			break;
		case MessageType::DisconnectMsg:
			th->OnDisconnected();
			break;
		case MessageType::ConnectionConnectedMsg:
			th->OnConnectionConnected();
			break;
		case MessageType::ExceptionMsg:
			th->OnException((Message *)msg->value);
			break;
		default:
			blog(LOG_WARNING, "Protocol error !");
			break;
		}
	}

	blog(LOG_WARNING, "%s Receive thread exit !",th->getIp().c_str());
	return nullptr;
}

void SSPClientIso::Restart()
{
	this->Stop();
	emit this->Start();
}

void SSPClientIso::Stop()
{
	blog(LOG_INFO, "ssp client %s stopping...",ip.c_str());
	this->statusLock.lock();
	const bool wasRunning = this->running;
	this->running = false;
	zc_process_pipe_t *pp = this->pipe;
	this->statusLock.unlock();

	if (!wasRunning && !pp) {
		blog(LOG_INFO, "ssp client %s already stopped...", ip.c_str());
		return;
	}

#ifdef _WIN32
	/* Terminate the connector FIRST. It may be alive but silent (a stalled
	   camera): its pipe stays open, so the receive thread's ReadFile never
	   returns and joining it — or os_process_pipe_destroy()'s own INFINITE
	   wait on the child — would freeze OBS for good. Killing the child closes
	   its pipe ends, which unblocks both the stdout and stderr readers. */
	if (pp) {
		HANDLE child = pipe_child_process(pp);
		if (child) {
			if (TerminateProcess(child, 0))
				blog(LOG_INFO, "ssp client %s connector terminated",
				     ip.c_str());
			/* Bounded: the handle is signalled once the child is gone, so
			   os_process_pipe_destroy()'s WaitForSingleObject below returns
			   immediately instead of blocking. */
			WaitForSingleObject(child, 5000);
		}
	}
#elif defined(__APPLE__)
	/* The connector may be alive but silent; without a child handle the
	   reader would stay blocked in fread() and join forever. Terminate the
	   process group first so both readers see EOF. */
	if (pp)
		zc_mac_process_pipe_terminate(pp);
#endif

	/* Join both readers before the pipe is destroyed: they read from it and
	   would otherwise dereference the freed struct. */
	if (this->worker.joinable())
		this->worker.join();
	if (this->stderrWorker.joinable())
		this->stderrWorker.join();

	if (pp) {
		zc_process_pipe_destroy(pp);
		this->pipe = nullptr;
	}
}

void SSPClientIso::OnRecvBufferFull()
{
	this->bufferFullCallback();
}

void SSPClientIso::OnH264Data(VideoData *videoData)
{
	imf::SspH264Data video;
	video.frm_no = videoData->frm_no;
	video.ntp_timestamp = videoData->ntp_timestamp;
	video.pts = videoData->pts;
	video.type = videoData->type;
	video.len = videoData->len;
	video.data = videoData->data;
	this->h264DataCallback(&video);
}
void SSPClientIso::OnAudioData(AudioData *audioData)
{
	struct imf::SspAudioData audio;
	audio.ntp_timestamp = audioData->ntp_timestamp;
	audio.pts = audioData->pts;
	audio.len = audioData->len;
	audio.data = audioData->data;
	this->audioDataCallback(&audio);
}
void SSPClientIso::OnMetadata(Metadata *metadata)
{

	struct imf::SspVideoMeta vmeta;
	struct imf::SspAudioMeta ameta;
	struct imf::SspMeta meta;

	ameta.bitrate = metadata->ameta.bitrate;
	ameta.channel = metadata->ameta.channel;
	ameta.encoder = metadata->ameta.encoder;
	ameta.sample_rate = metadata->ameta.sample_rate;
	ameta.sample_size = metadata->ameta.sample_size;
	ameta.timescale = metadata->ameta.timescale;
	ameta.unit = metadata->ameta.unit;

	vmeta.encoder = metadata->vmeta.encoder;
	vmeta.gop = metadata->vmeta.gop;
	vmeta.height = metadata->vmeta.height;
	vmeta.timescale = metadata->vmeta.timescale;
	vmeta.unit = metadata->vmeta.unit;
	vmeta.width = metadata->vmeta.width;

	meta.pts_is_wall_clock = metadata->meta.pts_is_wall_clock;
	meta.tc_drop_frame = metadata->meta.tc_drop_frame;
	meta.timecode = metadata->meta.timecode;

	this->metaCallback(&vmeta, &ameta, &meta);
}
void SSPClientIso::OnDisconnected()
{
	this->disconnectedCallback();
}
void SSPClientIso::OnConnectionConnected()
{
	this->connectedCallback();
}
void SSPClientIso::OnException(Message *exception)
{
	this->exceptionCallback(exception->type, (char *)exception->value);
}

void SSPClientIso::setOnRecvBufferFullCallback(
	const imf::OnRecvBufferFullCallback &cb)
{
	this->bufferFullCallback = cb;
}

void SSPClientIso::setOnAudioDataCallback(const imf::OnAudioDataCallback &cb)
{
	this->audioDataCallback = cb;
}

void SSPClientIso::setOnMetaCallback(const imf::OnMetaCallback &cb)
{
	this->metaCallback = cb;
}

void SSPClientIso::setOnDisconnectedCallback(
	const imf::OnDisconnectedCallback &cb)
{
	this->disconnectedCallback = cb;
}

void SSPClientIso::setOnConnectionConnectedCallback(
	const imf::OnConnectionConnectedCallback &cb)
{
	this->connectedCallback = cb;
}

void SSPClientIso::setOnH264DataCallback(const imf::OnH264DataCallback &cb)
{
	this->h264DataCallback = cb;
}

void SSPClientIso::setOnExceptionCallback(const imf::OnExceptionCallback &cb)
{
	this->exceptionCallback = cb;
}
