#include "Twibd.hpp"

#include "config.hpp"
#include "platform.hpp"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>

#if WITH_SYSTEMD == 1
#include<systemd/sd-daemon.h>
#endif

#include<libusb.h>
#include<msgpack11.hpp>
#include<CLI/CLI.hpp>

#include "SocketFrontend.hpp"
#include "Protocol.hpp"
#include "err.hpp"

#include <iostream>
#include <ostream>
#include <string>

namespace twili {
namespace twibd {

Twibd::Twibd() :
	local_client(std::make_shared<LocalClient>(this)),
	usb(this),
	tcp(this) {
	AddClient(local_client);
	usb.Probe();
}

Twibd::~Twibd() {
	LogMessage(Debug, "destroying twibd");
}

void Twibd::AddDevice(std::shared_ptr<Device> device) {
	std::lock_guard<std::mutex> lock(device_map_mutex);
	LogMessage(Info, "adding device with id %08x", device->device_id);
	std::weak_ptr<Device> &entry = devices[device->device_id];
	std::shared_ptr<Device> entry_lock = entry.lock();
	if(!entry_lock || entry_lock->GetPriority() <= device->GetPriority()) { // don't let tcp devices clobber usb devices
		entry = device;
	}
	
	LogMessage(Debug, "resetting objects on new device");
	local_client->SendRequest(
		Request(nullptr, device->device_id, 0, 0xffffffff, 0)); // we don't care about the response
}

void Twibd::AddClient(std::shared_ptr<Client> client) {
	std::lock_guard<std::mutex> lock(client_map_mutex);

	uint32_t client_id;
	do {
		client_id = rng();
	} while(clients.find(client_id) != clients.end());
	client->client_id = client_id;
	LogMessage(Info, "adding client with newly assigned id %08x", client_id);
	
	clients[client_id] = client;
}

void Twibd::PostRequest(Request &&request) {
	dispatch_queue.enqueue(request);
}

void Twibd::PostResponse(Response &&response) {
	dispatch_queue.enqueue(response);
}

void Twibd::RemoveClient(std::shared_ptr<Client> client) {
	std::lock_guard<std::mutex> lock(client_map_mutex);
	clients.erase(clients.find(client->client_id));
	LogMessage(Info, "removing client %08x", client->client_id);
}

void Twibd::RemoveDevice(std::shared_ptr<Device> device) {
	std::lock_guard<std::mutex> lock(device_map_mutex);
	devices.erase(devices.find(device->device_id));
	LogMessage(Info, "removing device %08x", device->device_id);
}

void Twibd::Process() {
	std::variant<Request, Response> v;
	LogMessage(Debug, "Process: dequeueing job...");
	dispatch_queue.wait_dequeue(v);
	LogMessage(Debug, "Process: dequeued job: %d", v.index());

	if(v.index() == 0) {
		Request rq = std::get<Request>(v);
		LogMessage(Debug, "dispatching request");
		LogMessage(Debug, "  client id: %08x", rq.client->client_id);
		LogMessage(Debug, "  device id: %08x", rq.device_id);
		LogMessage(Debug, "  object id: %08x", rq.object_id);
		LogMessage(Debug, "  command id: %08x", rq.command_id);
		LogMessage(Debug, "  tag: %08x", rq.tag);
		
		if(rq.device_id == 0) {
			PostResponse(HandleRequest(rq));
		} else {
			std::shared_ptr<Device> device;
			{
				std::lock_guard<std::mutex> lock(device_map_mutex);
				auto i = devices.find(rq.device_id);
				if(i == devices.end()) {
					PostResponse(rq.RespondError(TWILI_ERR_PROTOCOL_UNRECOGNIZED_DEVICE));
					return;
				}
				device = i->second.lock();
				if(!device || device->deletion_flag) {
					PostResponse(rq.RespondError(TWILI_ERR_PROTOCOL_UNRECOGNIZED_DEVICE));
					return;
				}
			}
			if(rq.command_id == 0xffffffff) {
				LogMessage(Debug, "detected close request for 0x%x", rq.object_id);
				std::shared_ptr<Client> client = rq.client;
				if(client) {
					// disown the object that's being closed
					for(auto i = client->owned_objects.begin(); i != client->owned_objects.end(); ){
						if((*i)->object_id == rq.object_id) {
							// need to mark this so that it doesn't send another close request
							(*i)->valid = false;
							i = client->owned_objects.erase(i);
							LogMessage(Debug, "  disowned from client");
						} else {
							i++;
						}
					}
				} else {
					LogMessage(Warning, "failed to locate client for disownership");
				}
			}
			LogMessage(Debug, "sending request via device");
			device->SendRequest(std::move(rq));
			LogMessage(Debug, "sent request via device");
		}
	} else if(v.index() == 1) {
		Response rs = std::get<Response>(v);
		LogMessage(Debug, "dispatching response");
		LogMessage(Debug, "  client id: %08x", rs.client_id);
		LogMessage(Debug, "  object id: %08x", rs.object_id);
		LogMessage(Debug, "  result code: %08x", rs.result_code);
		LogMessage(Debug, "  tag: %08x", rs.tag);
		LogMessage(Debug, "  objects:");
		for(auto o : rs.objects) {
			LogMessage(Debug, "    0x%x", o->object_id);
		}
		
		std::shared_ptr<Client> client = GetClient(rs.client_id);
		if(!client) {
			LogMessage(Info, "dropping response for bad client: 0x%x", rs.client_id);
			return;
		}
		// add any objects this response included to the client's
		// owned object list, to keep the BridgeObject object alive
		client->owned_objects.insert(
			client->owned_objects.end(),
			rs.objects.begin(),
			rs.objects.end());
		client->PostResponse(rs);
	}
	LogMessage(Debug, "finished process loop");
}

Response Twibd::HandleRequest(Request &rq) {
	switch(rq.object_id) {
	case 0:
		switch((protocol::ITwibMetaInterface::Command) rq.command_id) {
		case protocol::ITwibMetaInterface::Command::LIST_DEVICES: {
			LogMessage(Debug, "command 0 issued to twibd meta object: LIST_DEVICES");
			
			Response r = rq.RespondOk();
			std::vector<msgpack11::MsgPack> device_packs;
			for(auto i = devices.begin(); i != devices.end(); i++) {
				auto device = i->second.lock();
				device_packs.push_back(
					msgpack11::MsgPack::object {
						{"device_id", device->device_id},
						{"bridge_type", device->GetBridgeType()},
						{"identification", device->identification}
					});
			}
			msgpack11::MsgPack array_pack(device_packs);
			std::string ser = array_pack.dump();
			r.payload = std::vector<uint8_t>(ser.begin(), ser.end());
			
			return r; }
		case protocol::ITwibMetaInterface::Command::CONNECT_TCP: {
			LogMessage(Debug, "command 1 issued to twibd meta object: CONNECT_TCP");

			util::Buffer buffer(rq.payload);
			size_t hostname_len, port_len;
			std::string hostname, port;
			if(!buffer.Read(hostname_len) ||
				 !buffer.Read(port_len) ||
				 !buffer.Read(hostname, hostname_len) ||
				 !buffer.Read(port, port_len)) {
				return rq.RespondError(TWILI_ERR_PROTOCOL_BAD_REQUEST);
			}
			LogMessage(Info, "requested to connect to %s:%s", hostname.c_str(), port.c_str());

			Response r = rq.RespondOk();
			std::string msg = tcp.Connect(hostname, port);
			r.payload = std::vector<uint8_t>(msg.begin(), msg.end());
			return r; }
		default:
			return rq.RespondError(TWILI_ERR_PROTOCOL_UNRECOGNIZED_FUNCTION);
		}
	default:
		return rq.RespondError(TWILI_ERR_PROTOCOL_UNRECOGNIZED_OBJECT);
	}
}

std::shared_ptr<Client> Twibd::GetClient(uint32_t client_id) {
	std::shared_ptr<Client> client;
	{
		std::lock_guard<std::mutex> lock(client_map_mutex);
		auto i = clients.find(client_id);
		if(i == clients.end()) {
			LogMessage(Debug, "client id 0x%x is not in map", client_id);
			return std::shared_ptr<Client>();
		}
		client = i->second.lock();
		if(!client) {
			LogMessage(Debug, "client id 0x%x weak pointer expired", client_id);
			return std::shared_ptr<Client>();
		}
		if(client->deletion_flag) {
			LogMessage(Debug, "client id 0x%x deletion flag set", client_id);
			return std::shared_ptr<Client>();
		}
	}
	return client;
}

#if TWIB_TCP_FRONTEND_ENABLED == 1
static std::shared_ptr<frontend::SocketFrontend> CreateTCPFrontend(Twibd &twibd, uint16_t port) {
	struct sockaddr_in6 addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_any;
	return std::make_shared<frontend::SocketFrontend>(&twibd, AF_INET6, SOCK_STREAM, (struct sockaddr*) &addr, sizeof(addr));
}
#endif

#if TWIB_UNIX_FRONTEND_ENABLED == 1
static std::shared_ptr<frontend::SocketFrontend> CreateUNIXFrontend(Twibd &twibd, std::string path) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path)-1);
	return std::make_shared<frontend::SocketFrontend>(&twibd, AF_UNIX, SOCK_STREAM, (struct sockaddr*) &addr, sizeof(addr));
}
#endif

} // namespace twibd
} // namespace twili

int main(int argc, char *argv[]) {
#ifdef _WIN32
	WSADATA wsaData;
	int err;
	err = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (err != 0) {
		printf("WSASStartup failed with error: %d\n", err);
		return 1;
	}
#endif

	CLI::App app {"Twili debug monitor daemon"};

	int verbosity = false;
	app.add_flag("-v,--verbose", verbosity, "Enable verbose messages. Use twice to enable debug messages");
	
	bool systemd_mode = false;
#if WITH_SYSTEMD == 1
	app.add_flag("--systemd", systemd_mode, "Log in systemd format and obtain sockets from systemd (disables unix and tcp frontends)");
#endif

#if TWIB_UNIX_FRONTEND_ENABLED == 1
	bool unix_frontend_enabled = true;
	app.add_flag_function(
		"--unix",
		[&unix_frontend_enabled](int count) {
			unix_frontend_enabled = true;
		}, "Enable UNIX socket frontend");
	app.add_flag_function(
		"--no-unix",
		[&unix_frontend_enabled](int count) {
			unix_frontend_enabled = false;
		}, "Disable UNIX socket frontend");
	std::string unix_frontend_path = TWIB_UNIX_FRONTEND_DEFAULT_PATH;
	app.add_option(
		"-P,--unix-path", unix_frontend_path,
		"Path for the twibd UNIX socket frontend")
		->envname("TWIB_UNIX_FRONTEND_PATH");
#endif

#if TWIB_TCP_FRONTEND_ENABLED == 1
	bool tcp_frontend_enabled = true;
	app.add_flag_function(
		"--tcp",
		[&tcp_frontend_enabled](int count) {
			tcp_frontend_enabled = true;
		}, "Enable TCP socket frontend");
	app.add_flag_function(
		"--no-tcp",
		[&tcp_frontend_enabled](int count) {
			tcp_frontend_enabled = false;
		}, "Disable TCP socket frontend");
	uint16_t tcp_frontend_port;
	app.add_option(
		"-p,--tcp-port", tcp_frontend_port,
		"Port for the twibd TCP socket frontend")
		->envname("TWIB_TCP_FRONTEND_PORT");
#endif

	try {
		app.parse(argc, argv);
	} catch(const CLI::ParseError &e) {
		return app.exit(e);
	}

	twili::log::Level min_log_level = twili::log::Level::Message;
	if(verbosity >= 1) {
		min_log_level = twili::log::Level::Info;
	}
	if(verbosity >= 2) {
		min_log_level = twili::log::Level::Debug;
	}
#if WITH_SYSTEMD == 1
	if(systemd_mode) {
		add_log(std::make_shared<twili::log::SystemdLogger>(stderr, min_log_level));
	}
#endif
	if(!systemd_mode) {
		add_log(std::make_shared<twili::log::PrettyFileLogger>(stdout, min_log_level, twili::log::Level::Error));
		add_log(std::make_shared<twili::log::PrettyFileLogger>(stderr, twili::log::Level::Error));
	}

	LogMessage(Message, "starting twibd");
	twili::twibd::Twibd twibd;
	std::vector<std::shared_ptr<twili::twibd::frontend::SocketFrontend>> frontends;
	if(!systemd_mode) {
#if TWIB_TCP_FRONTEND_ENABLED == 1
		if(tcp_frontend_enabled) {
			frontends.push_back(twili::twibd::CreateTCPFrontend(twibd, tcp_frontend_port));
		}
#endif
#if TWIB_UNIX_FRONTEND_ENABLED == 1
		if(unix_frontend_enabled) {
			frontends.push_back(twili::twibd::CreateUNIXFrontend(twibd, unix_frontend_path));
		}
#endif
	}

#if WITH_SYSTEMD == 1
	if(systemd_mode) {
		int num_fds = sd_listen_fds(false);
		if(num_fds < 0) {
			LogMessage(Warning, "failed to get FDs from systemd");
		} else {
			LogMessage(Info, "got %d sockets from systemd", num_fds);
			for(int fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + num_fds; fd++) {
				if(sd_is_socket(fd, 0, SOCK_STREAM, 1) == 1) {
					frontends.push_back(std::make_shared<twili::twibd::frontend::SocketFrontend>(&twibd, fd));
				} else {
					LogMessage(Warning, "got an FD from systemd that wasn't a SOCK_STREAM: %d", fd);
				}
			}
		}
		sd_notify(false, "READY=1");
	}
#endif
	
	while(1) {
		twibd.Process();
	}
	return 0;
}
