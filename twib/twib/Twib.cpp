#include "Twib.hpp"

#include "platform.hpp"

#include<random>
#include<iomanip>

#include<string.h>

#include<msgpack11.hpp>

#include "CLI/CLI.hpp"

#include "Logger.hpp"
#include "ResultError.hpp"
#include "config.hpp"
#include "Protocol.hpp"
#include "ITwibMetaInterface.hpp"
#include "ITwibDeviceInterface.hpp"
#include "util.hpp"
#include "err.hpp"

namespace twili {
namespace twib {

Twib::Twib(int fd) : mc(fd, this) {
	// TODO: Figure out how to fix this.
#ifndef _WIN32
	if(pipe(event_thread_notification_pipe) < 0) {
		LogMessage(Fatal, "failed to create pipe for event thread notifications: %s", strerror(errno));
		exit(1);
	}
#endif
	std::thread event_thread(&Twib::event_thread_func, this);
	this->event_thread = std::move(event_thread);
}

Twib::~Twib() {
	event_thread_destroy = true;
	NotifyEventThread();
	event_thread.join();
}

void Twib::event_thread_func() {
	fd_set recvset;
	fd_set sendset;
	SOCKET maxfd = 0;
	while(!event_thread_destroy) {
		LogMessage(Debug, "event thread loop");

		FD_ZERO(&recvset);
		FD_ZERO(&sendset);
#ifndef _WIN32
		FD_SET(event_thread_notification_pipe[0], &recvset);
		maxfd = std::max(maxfd, event_thread_notification_pipe[0]);
#endif
		FD_SET(mc.fd, &recvset);
		maxfd = std::max(maxfd, mc.fd);
		if(mc.out_buffer.ReadAvailable() > 0) {
			FD_SET(mc.fd, &sendset);
		}

		if(select(maxfd + 1, &recvset, &sendset, NULL, NULL) < 0) {
			LogMessage(Fatal, "failed to select file descriptors: %s", strerror(errno));
			exit(1);
		}

#ifndef _WIN32
		// check poll flags on event notification pipe
		if(FD_ISSET(event_thread_notification_pipe[0], &recvset)) {
			char buf[64];
			ssize_t r = read(event_thread_notification_pipe[0], buf, sizeof(buf));
			if(r < 0) {
				LogMessage(Fatal, "failed to read from event thread notification pipe: %s", strerror(errno));
				exit(1);
			}
			LogMessage(Debug, "event thread notified: '%.*s'", r, buf);
		}
#endif

		// check poll flags on twibd socket
		if(FD_ISSET(mc.fd, &sendset)) {
			mc.PumpOutput();
		}
		if(FD_ISSET(mc.fd, &recvset)) {
			mc.PumpInput();
		}
		mc.Process();
	}
}

void Twib::NotifyEventThread() {
	char buf[] = ".";
	if(write(event_thread_notification_pipe[1], buf, sizeof(buf)) != sizeof(buf)) {
		LogMessage(Fatal, "failed to write to event thread notification pipe: %s", strerror(errno));
		exit(1);
	}
}

Client::Client(twibc::MessageConnection<Client> &mc, Twib *twib) : mc(mc), twib(twib) {
	
}

void Client::IncomingMessage(protocol::MessageHeader &mh, util::Buffer &payload, util::Buffer &object_ids) {
	// create RAII objects for remote objects
	std::vector<std::shared_ptr<RemoteObject>> objects(mh.object_count);
	for(uint32_t i = 0; i < mh.object_count; i++) {
		uint32_t id;
		if(!object_ids.Read(id)) {
			LogMessage(Error, "not enough object IDs");
			return;
		}
		objects[i] = std::make_shared<RemoteObject>(shared_from_this(), mh.device_id, id);
	}
	
	std::promise<Response> promise;
	{
		std::lock_guard<std::mutex> lock(response_map_mutex);
		auto it = response_map.find(mh.tag);
		if(it == response_map.end()) {
			LogMessage(Warning, "dropping response for unknown tag 0x%x", mh.tag);
			return;
		}
		promise.swap(it->second);
		response_map.erase(it);
	}
	
	promise.set_value(
		Response(
			shared_from_this(),
			mh.device_id,
			mh.object_id,
			mh.result_code,
			mh.tag,
			std::vector<uint8_t>(payload.Read(), payload.Read() + payload.ReadAvailable()),
			objects));
}

std::future<Response> Client::SendRequest(Request rq) {
	std::future<Response> future;
	{
		std::lock_guard<std::mutex> lock(response_map_mutex);
		static std::random_device rng;
		
		uint32_t tag = rng();
		rq.tag = tag;
		
		std::promise<Response> promise;
		future = promise.get_future();
		response_map[tag] = std::move(promise);
	}
	{
		std::lock_guard<std::mutex> lock(mc.out_buffer_mutex);
		protocol::MessageHeader mh;
		mh.device_id = rq.device_id;
		mh.object_id = rq.object_id;
		mh.command_id = rq.command_id;
		mh.tag = rq.tag;
		mh.payload_size = rq.payload.size();
		mh.object_count = 0;

		mc.out_buffer.Write(mh);
		mc.out_buffer.Write(rq.payload);
		twib->NotifyEventThread();
		LogMessage(Debug, "sent request");
	}
	
	return future;
}

template<size_t N>
void PrintTable(std::vector<std::array<std::string, N>> rows) {
	std::array<int, N> lengths = {0};
	for(auto r : rows) {
		for(size_t i = 0; i < N; i++) {
			if(r[i].size() > lengths[i]) {
				lengths[i] = r[i].size();
			}
		}
	}
	for(auto r : rows) {
		for(size_t i = 0; i < N; i++) {
			printf("%-*s%s", lengths[i], r[i].c_str(), (i + 1 == N) ? "\n" : " | ");
		}
	}
}

template<typename T>
std::string ToHex(T num, int width, bool prefix) {
	std::stringstream stream;
	stream << (prefix ? "0x" : "") << std::setfill('0') << std::setw(width) << std::hex << num;
	return stream.str();
}

template<typename T>
std::string ToHex(T num, bool prefix) {
	std::stringstream stream;
	stream << (prefix ? "0x" : "") << std::hex << num;
	return stream.str();
}

void ListDevices(ITwibMetaInterface &iface) {
	std::vector<std::array<std::string, 4>> rows;
	rows.push_back({"Device ID", "Nickname", "Firmware Version", "Bridge Type"});
	auto devices = iface.ListDevices();
	for(msgpack11::MsgPack device : devices) {
		uint32_t device_id = device["device_id"].uint32_value();
		std::string bridge_type = device["bridge_type"].string_value();
		msgpack11::MsgPack ident = device["identification"];
		auto nickname = ident["device_nickname"].string_value();
		auto version = ident["firmware_version"].binary_items();
		std::string fw_version((char*) version.data() + 0x68);
		
		rows.push_back({ToHex(device_id, 8, false), nickname, fw_version, bridge_type});
	}
	PrintTable(rows);
}

void ListProcesses(ITwibDeviceInterface &iface) {
	std::vector<std::array<std::string, 5>> rows;
	rows.push_back({"Process ID", "Result", "Title ID", "Process Name", "MMU Flags"});
	auto processes = iface.ListProcesses();
	for(auto p : processes) {
		rows.push_back({
			ToHex(p.process_id, true),
			ToHex(p.result, true),
			ToHex(p.title_id, true),
			std::string(p.process_name, 12),
			ToHex(p.mmu_flags, true)});
	}
	PrintTable(rows);
}

} // namespace twib
} // namespace twili

int connect_tcp(uint16_t port);
int connect_unix(std::string path);

void show(msgpack11::MsgPack const& blob);

int main(int argc, char *argv[]) {
#ifdef _WIN32
	WSADATA wsaData;
	int err;
	err = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (err != 0) {
		printf("WSAStartup failed with error: %d\n", err);
		return 1;
	}
#endif

	CLI::App app {"Twili debug monitor client"};

	std::string device_id_str;
	app.add_option("-d,--device", device_id_str, "Use a specific device")
		->type_name("DeviceId")
		->envname("TWIB_DEVICE");

	bool is_verbose;
	app.add_flag("-v,--verbose", is_verbose, "Enable debug logging");

	std::string frontend;
	std::string unix_frontend_path = TWIB_UNIX_FRONTEND_DEFAULT_PATH;
	uint16_t tcp_frontend_port = TWIB_TCP_FRONTEND_DEFAULT_PORT;
	
#if TWIB_UNIX_FRONTEND_ENABLED == 1
	frontend = "unix";
#else
	frontend = "tcp";
#endif
	
	app.add_set("-f,--frontend", frontend, {
#if TWIB_UNIX_FRONTEND_ENABLED == 1
			"unix",
#endif
#if TWIB_TCP_FRONTEND_ENABLED == 1
			"tcp",
#endif
		})->envname("TWIB_FRONTEND");

#if TWIB_UNIX_FRONTEND_ENABLED == 1
	app.add_option(
		"-P,--unix-path", unix_frontend_path,
		"Path to the twibd UNIX socket")
		->envname("TWIB_UNIX_FRONTEND_PATH");
#endif

#if TWIB_TCP_FRONTEND_ENABLED == 1
	app.add_option(
		"-p,--tcp-port", tcp_frontend_port,
		"Port for the twibd TCP socket")
		->envname("TWIB_TCP_FRONTEND_PORT");
#endif
	
	CLI::App *ld = app.add_subcommand("list-devices", "List devices");
	
	CLI::App *cmd_connect_tcp = app.add_subcommand("connect-tcp", "Connect to a device over TCP");
	std::string connect_tcp_hostname;
	std::string connect_tcp_port = "15152";
	cmd_connect_tcp->add_option("hostname", connect_tcp_hostname, "Hostname to connect to")->required();
	cmd_connect_tcp->add_option("port", connect_tcp_port, "Port to connect to");
	
	CLI::App *run = app.add_subcommand("run", "Run an executable");
	std::string run_file;
	run->add_option("file", run_file, "Executable to run")->check(CLI::ExistingFile)->required();
	
	CLI::App *reboot = app.add_subcommand("reboot", "Reboot the device");

	CLI::App *coredump = app.add_subcommand("coredump", "Make a coredump of a crashed process");
	std::string core_file;
	uint64_t core_process_id;
	coredump->add_option("file", core_file, "File to dump core to")->required();
	coredump->add_option("pid", core_process_id, "Process ID")->required();
	
	CLI::App *terminate = app.add_subcommand("terminate", "Terminate a process on the device");
	uint64_t terminate_process_id;
	terminate->add_option("pid", terminate_process_id, "Process ID")->required();
	
	CLI::App *ps = app.add_subcommand("ps", "List processes on the device");

	CLI::App *identify = app.add_subcommand("identify", "Identify the device");

	CLI::App *list_named_pipes = app.add_subcommand("list-named-pipes", "List named pipes on the device");

	CLI::App *open_named_pipe = app.add_subcommand("open-named-pipe", "Open a named pipe on the device");
	std::string open_named_pipe_name;
	open_named_pipe->add_option("name", open_named_pipe_name, "Name of pipe to open")->required();
	
	app.require_subcommand(1);
	
	try {
		app.parse(argc, argv);
	} catch(const CLI::ParseError &e) {
		return app.exit(e);
	}

	if(is_verbose) {
		add_log(std::make_shared<twili::log::PrettyFileLogger>(stdout, twili::log::Level::Debug, twili::log::Level::Error));
	}
	add_log(std::make_shared<twili::log::PrettyFileLogger>(stderr, twili::log::Level::Error));
	
	LogMessage(Message, "starting twib");

	int fd;
	if(TWIB_UNIX_FRONTEND_ENABLED && frontend == "unix") {
		fd = connect_unix(unix_frontend_path);
	} else if(TWIB_TCP_FRONTEND_ENABLED && frontend == "tcp") {
		fd = connect_tcp(tcp_frontend_port);
	} else {
		LogMessage(Fatal, "unrecognized frontend: %s", frontend.c_str());
		exit(1);
	}
	twili::twib::Twib twib(fd);
	twili::twib::ITwibMetaInterface itmi(twili::twib::RemoteObject(twib.mc.obj, 0, 0));
	
	if(ld->parsed()) {
		ListDevices(itmi);
		return 0;
	}

	if(cmd_connect_tcp->parsed()) {
		printf("%s\n", itmi.ConnectTcp(connect_tcp_hostname, connect_tcp_port).c_str());
		return 0;
	}

	uint32_t device_id;
	if(device_id_str.size() > 0) {
		device_id = std::stoull(device_id_str, NULL, 16);
	} else {
		std::vector<msgpack11::MsgPack> devices = itmi.ListDevices();
		if(devices.size() == 0) {
			LogMessage(Fatal, "No devices were detected.");
			return 1;
		}
		if(devices.size() > 1) {
			LogMessage(Fatal, "Multiple devices were detected. Please use -d to specify which one you mean.");
			return 1;
		}
		device_id = devices[0]["device_id"].uint32_value();
	}
	twili::twib::ITwibDeviceInterface itdi(std::make_shared<twili::twib::RemoteObject>(twib.mc.obj, device_id, 0));
	
	if(run->parsed()) {
		auto v_opt = twili::util::ReadFile(run_file.c_str());
		if(!v_opt) {
			LogMessage(Fatal, "could not read file");
			return 1;
		}
		twili::twib::RunResult rs = itdi.Run(*v_opt);
		printf("PID: 0x%x\n", rs.pid);
		volatile bool running = true;
		auto pump_output =
			[&running](twili::twib::ITwibPipeReader reader, FILE *stream) {
				try {
					while(running) {
						std::vector<uint8_t> str = reader.ReadSync();
						size_t r = fwrite(str.data(), sizeof(str[0]), str.size(), stream);
						if(r < str.size() && str.size() > 0) {
							throw std::system_error(errno, std::generic_category());
						}
						fflush(stream);
					}
				} catch(twili::twib::ResultError &e) {
					running = false;
					if(e.code == TWILI_ERR_EOF) {
						running = false;
						return;
					} else {
						throw e;
					}
				} catch(...) {
					running = false;
					throw;
				}
			};
		std::thread stdout_pump(pump_output, rs.tp_stdout, stdout);
		std::thread stderr_pump(pump_output, rs.tp_stderr, stderr);
		std::thread stdin_pump(
			[&running](twili::twib::ITwibPipeWriter writer) {
				try {
					std::vector<uint8_t> buffer(4096, 0);
					while(running) {
						buffer.resize(4096);
						// fread won't return until it fills the buffer or errors.
						// this is not what we want; we want to send data over the
						// pipe as soon as it comes in.
						ssize_t r = read(fileno(stdin), buffer.data(), buffer.size());
						if(r > 0) {
							buffer.resize(r);
							writer.WriteSync(buffer);
						} else if(r == 0) {
							writer.Close();
							break;
						} else {
							throw std::system_error(errno, std::generic_category());
						}
					}
				} catch(twili::twib::ResultError &e) {
					running = false;
					if(e.code == TWILI_ERR_EOF) {
						running = false;
						return;
					} else {
						throw e;
					}
				} catch(...) {
					running = false;
					throw;
				}
			}, rs.tp_stdin);
		stdout_pump.join();
		stderr_pump.join();
		exit(0); // skip calling stdin_pump thread destructor, since it will std::terminate
	}

	if(reboot->parsed()) {
		itdi.Reboot();
		return 0;
	}

	if(coredump->parsed()) {
		FILE *f = fopen(core_file.c_str(), "wb");
		if(!f) {
			LogMessage(Fatal, "could not open '%s': %s", core_file.c_str(), strerror(errno));
			return 1;
		}
		std::vector<uint8_t> core = itdi.CoreDump(core_process_id);
		size_t written = 0;
		while(written < core.size()) {
			ssize_t r = fwrite(core.data() + written, 1, core.size() - written, f);
			if(r <= 0 || ferror(f)) {
				LogMessage(Fatal, "write error on '%s'");
			} else {
				written+= r;
			}
		}
		fclose(f);
	}
	
	if(terminate->parsed()) {
		itdi.Terminate(terminate_process_id);
		return 0;
	}

	if(ps->parsed()) {
		ListProcesses(itdi);
		return 0;
	}

	if(identify->parsed()) {
		show(itdi.Identify());
		return 0;
	}

	if(list_named_pipes->parsed()) {
		for(auto n : itdi.ListNamedPipes()) {
			printf("%s\n", n.c_str());
		}
		return 0;
	}

	if(open_named_pipe->parsed()) {
		auto reader = itdi.OpenNamedPipe(open_named_pipe_name);
		try {
			while(true) {
				std::vector<uint8_t> str = reader.ReadSync();
				std::cout << std::string(str.begin(), str.end());
			}
		} catch(twili::twib::ResultError &e) {
			if(e.code == TWILI_ERR_EOF) {
				return 0;
			} else {
				throw e;
			}
		}
		return 0;
	}
	
	return 0;
}

int connect_tcp(uint16_t port) {
#if TWIB_TCP_FRONTEND_ENABLED == 0
	LogMessage(Fatal, "TCP socket not supported");
	exit(1);
	return -1;
#else
	SOCKET fd = socket(AF_INET6, SOCK_STREAM, 0);
	if(fd < 0) {
		LogMessage(Fatal, "failed to create TCP socket: %s", strerror(errno));
		exit(1);
	}

	struct sockaddr_in6 addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_loopback;
	addr.sin6_port = htons(port);

	if(connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		LogMessage(Fatal, "failed to connect to twibd: %s", strerror(errno));
		closesocket(fd);
		exit(1);
	}
	LogMessage(Info, "connected to twibd: %d", fd);
	return fd;
#endif
}

int connect_unix(std::string path) {
#if TWIB_UNIX_FRONTEND_ENABLED == 0
	LogMessage(Fatal, "UNIX domain socket not supported");
	exit(1);
	return -1;
#else
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(fd < 0) {
		LogMessage(Fatal, "failed to create UNIX domain socket: %s", strerror(errno));
		exit(1);
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path)-1);

	if(connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		LogMessage(Fatal, "failed to connect to twibd: %s", strerror(errno));
		close(fd);
		exit(1);
	}
	LogMessage(Info, "connected to twibd: %d", fd);
	return fd;
#endif
}
