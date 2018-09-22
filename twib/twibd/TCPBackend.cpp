#include "TCPBackend.hpp"

#include "platform.hpp"

#include "Twibd.hpp"

#include<netdb.h>

namespace twili {
namespace twibd {
namespace backend {

TCPBackend::TCPBackend(Twibd *twibd) :
	twibd(twibd) {
	listen_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(listen_fd == -1) {
		LogMessage(Error, "Failed to create listening socket: %s", NetErrStr());
		exit(1);
	}

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(15153);
	if(bind(listen_fd, (sockaddr*) &addr, sizeof(addr)) != 0) {
		LogMessage(Error, "Failed to bind listening socket: %s", NetErrStr());
		exit(1);
	}

	ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = inet_addr("224.0.53.55");
	mreq.imr_interface.s_addr = INADDR_ANY;
	if(setsockopt(listen_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
		LogMessage(Error, "Failed to join multicast group");
		exit(1);
	}

	event_thread = std::thread(&TCPBackend::event_thread_func, this);
}

TCPBackend::~TCPBackend() {
	event_thread_destroy = true;
	closesocket(listen_fd);
	event_thread.join();
}

std::string TCPBackend::Connect(std::string hostname, std::string port) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	struct addrinfo *res = 0;
	int err = getaddrinfo(hostname.c_str(), port.c_str(), &hints, &res);
	if(err != 0) {
		return gai_strerror(err);
	}
	
	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(fd == -1) {
		return NetErrStr();
	}
	if(connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
		closesocket(fd);
		return NetErrStr();
	}
	freeaddrinfo(res);

	connections.emplace_back(std::make_shared<twibc::MessageConnection<Device>>(fd, this))->obj->Begin();
	NotifyEventThread();
	return "Ok";
}

void TCPBackend::Connect(sockaddr *addr, socklen_t addr_len) {
	if(addr->sa_family == AF_INET) {
		sockaddr_in *addr_in = (sockaddr_in*) addr;
		LogMessage(Info, "  from %s", inet_ntoa(addr_in->sin_addr));
		addr_in->sin_port = htons(15152); // force port number
		
		int fd = socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
		if(fd == -1) {
			LogMessage(Error, "could not create socket: %s", NetErrStr());
			return;
		}

		if(connect(fd, addr, addr_len) == -1) {
			LogMessage(Error, "could not connect: %s", NetErrStr());
			closesocket(fd);
			return;
		}

		connections.emplace_back(std::make_shared<twibc::MessageConnection<Device>>(fd, this))->obj->Begin();
		LogMessage(Info, "connected to %s", inet_ntoa(addr_in->sin_addr));
		NotifyEventThread();
	} else {
		LogMessage(Info, "not an IPv4 address");
	}
}

TCPBackend::Device::Device(twibc::MessageConnection<Device> &mc, TCPBackend *backend) :
	backend(backend),
	connection(mc) {
}

TCPBackend::Device::~Device() {
}

void TCPBackend::Device::Begin() {
	SendRequest(Request(std::shared_ptr<Client>(), 0x0, 0x0, (uint32_t) protocol::ITwibDeviceInterface::Command::IDENTIFY, 0xFFFFFFFF, std::vector<uint8_t>()));
}

void TCPBackend::Device::IncomingMessage(protocol::MessageHeader &mh, util::Buffer &payload, util::Buffer &object_ids) {
	response_in.device_id = device_id;
	response_in.client_id = mh.client_id;
	response_in.object_id = mh.object_id;
	response_in.result_code = mh.result_code;
	response_in.tag = mh.tag;
	response_in.payload = std::vector<uint8_t>(payload.Read(), payload.Read() + payload.ReadAvailable());
	
	// create BridgeObjects
	response_in.objects.resize(mh.object_count);
	for(uint32_t i = 0; i < mh.object_count; i++) {
		uint32_t id;
		if(!object_ids.Read(id)) {
			LogMessage(Error, "not enough object IDs");
			return;
		}
		response_in.objects[i] = std::make_shared<BridgeObject>(*backend->twibd, mh.device_id, id);
	}

	// remove from pending requests
	pending_requests.remove_if([this](WeakRequest &r) {
			return r.tag == response_in.tag;
		});
	
	if(response_in.client_id == 0xFFFFFFFF) { // identification meta-client
		Identified(response_in);
	} else {
		backend->twibd->PostResponse(std::move(response_in));
	}
}

void TCPBackend::Device::Identified(Response &r) {
	LogMessage(Debug, "got identification response back");
	LogMessage(Debug, "payload size: 0x%x", r.payload.size());
	if(r.result_code != 0) {
		LogMessage(Warning, "device identification error: 0x%x", r.result_code);
		deletion_flag = true;
		return;
	}
	std::string err;
	msgpack11::MsgPack obj = msgpack11::MsgPack::parse(std::string(r.payload.begin(), r.payload.end()), err);
	identification = obj;
	device_nickname = obj["device_nickname"].string_value();
	std::vector<uint8_t> sn = obj["serial_number"].binary_items();
	serial_number = std::string(sn.begin(), sn.end());

	LogMessage(Info, "nickname: %s", device_nickname.c_str());
	LogMessage(Info, "serial number: %s", serial_number.c_str());
	
	device_id = std::hash<std::string>()(serial_number);
	LogMessage(Info, "assigned device id: %08x", device_id);
	ready_flag = true;
}

void TCPBackend::Device::SendRequest(const Request &&r) {
	protocol::MessageHeader mhdr;
	mhdr.client_id = r.client ? r.client->client_id : 0xffffffff;
	mhdr.object_id = r.object_id;
	mhdr.command_id = r.command_id;
	mhdr.tag = r.tag;
	mhdr.payload_size = r.payload.size();
	mhdr.object_count = 0;

	pending_requests.push_back(r.Weak());

	connection.out_buffer.Write(mhdr);
	connection.out_buffer.Write(r.payload);
	/* TODO: request objects
	std::vector<uint32_t> object_ids(r.objects.size(), 0);
	std::transform(
		r.objects.begin(), r.objects.end(), object_ids.begin(),
		[](auto const &object) {
			return object->object_id;
		});
	connection.out_buffer.Write(object_ids); */
	connection.PumpOutput();
}

int TCPBackend::Device::GetPriority() {
	return 1; // lower priority than USB devices
}

std::string TCPBackend::Device::GetBridgeType() {
	return "tcp";
}

void TCPBackend::event_thread_func() {
	fd_set readfds;
	fd_set writefds;
	fd_set errorfds;
	SOCKET max_fd = 0;
	while(!event_thread_destroy) {
		LogMessage(Debug, "tcp backend event thread loop");
		
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);

		// add listen socket
		max_fd = std::max(max_fd, listen_fd);
		FD_SET(listen_fd, &readfds);
		
		// add device connections
		for(auto &c : connections) {
			if(c->obj->ready_flag && !c->obj->added_flag) {
				twibd->AddDevice(c->obj);
				c->obj->added_flag = true;
			}
			
			max_fd = std::max(max_fd, c->fd);
			FD_SET(c->fd, &errorfds);
			FD_SET(c->fd, &readfds);
			
			if(c->out_buffer.ReadAvailable() > 0) { // only add to writefds if we need to write
				FD_SET(c->fd, &writefds);
			}
		}

		if(select(max_fd + 1, &readfds, &writefds, &errorfds, NULL) < 0) {
			LogMessage(Fatal, "failed to select file descriptors: %s", NetErrStr());
			exit(1);
		}

		// check for announcements or thread notifications
		if(FD_ISSET(listen_fd, &readfds)) {
			char buffer[256];
			sockaddr_storage addr_storage;
			sockaddr *addr = (sockaddr*) &addr_storage;
			socklen_t addr_len = sizeof(addr);
			ssize_t r = recvfrom(listen_fd, buffer, sizeof(buffer)-1, 0, addr, &addr_len);
			LogMessage(Debug, "got 0x%x bytes from listen socket", r);
			if(r < 0) {
				LogMessage(Fatal, "listen socket error: %s", NetErrStr());
				exit(1);
			} else {
				buffer[r] = 0;
				if(!strcmp(buffer, "twili-announce")) {
					LogMessage(Info, "received twili device announcement");
					Connect(addr, addr_len);
				}
			}
		}
		
		// pump i/o
		for(auto mci = connections.begin(); mci != connections.end(); mci++) {
			std::shared_ptr<twibc::MessageConnection<Device>> &mc = *mci;
			if(FD_ISSET(mc->fd, &errorfds)) {
				LogMessage(Info, "detected connection error");
				mc->obj->deletion_flag = true;
				continue;
			}
			if(FD_ISSET(mc->fd, &writefds)) {
				mc->PumpOutput();
			}
			if(FD_ISSET(mc->fd, &readfds)) {
				LogMessage(Debug, "incoming data for device %x", mc->obj->device_id);
				mc->PumpInput();
			}
		}

		for(auto i = connections.begin(); i != connections.end(); ) {
			(*i)->Process();
			
			if((*i)->obj->deletion_flag) {
				if((*i)->obj->added_flag) {
					twibd->RemoveDevice((*i)->obj);
				}
				i = connections.erase(i);
				continue;
			}

			i++;
		}
	}
}

void TCPBackend::NotifyEventThread() {
	sockaddr_storage addr;
	socklen_t len = sizeof(addr);
	if(getsockname(listen_fd, (sockaddr*) &addr, &len) != 0) {
		LogMessage(Error, "failed to get listen socket address");
		exit(1);
	}
	char msg[] = "notify";
	if(sendto(listen_fd, msg, sizeof(msg)-1, 0, (sockaddr*) &addr, len) != sizeof(msg)-1) {
		LogMessage(Error, "failed to notify event thread");
		exit(1);
	}
}

} // namespace backend
} // namespace twibd
} // namespace twili
