//
// Twili - Homebrew debug monitor for the Nintendo Switch
// Copyright (C) 2018 misson20000 <xenotoad@xenotoad.net>
//
// This file is part of Twili.
//
// Twili is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Twili is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Twili.  If not, see <http://www.gnu.org/licenses/>.
//

typedef bool _Bool;
#include<iostream>

#include<libtransistor/cpp/types.hpp>
#include<libtransistor/cpp/ipcserver.hpp>
#include<libtransistor/cpp/ipcclient.hpp>
#include<libtransistor/cpp/ipc/sm.hpp>
#include<libtransistor/ipc/fatal.h>
#include<libtransistor/ipc/sm.h>
#include<libtransistor/ipc.h>
#include<libtransistor/ipc_helpers.h>
#include<libtransistor/loader_config.h>
#include<libtransistor/runtime_config.h>
#include<libtransistor/usb_serial.h>
#include<libtransistor/svc.h>

#include<unistd.h>
#include<stdio.h>

#include "util.hpp"
#include "twili.hpp"
#include "process_creation.hpp"
#include "process/Process.hpp"
#include "process/UnmonitoredProcess.hpp"
#include "service/ITwiliService.hpp"
#include "bridge/interfaces/ITwibDeviceInterface.hpp"
#include "err.hpp"

using namespace trn;

int main() {
	uint64_t syscall_hints[2] = {0xffffffffffffffff, 0xffffffffffffffff};
	memcpy(loader_config.syscall_hints, syscall_hints, sizeof(syscall_hints));
	
	try {
		if(usb_serial_init() == RESULT_OK) {
			// set up serial console
			int usb_fd = usb_serial_open_fd();
			if(usb_fd < 0) {
				throw trn::ResultError(-usb_fd);
			}
			dup2(usb_fd, STDOUT_FILENO);
			dup2(usb_fd, STDERR_FILENO);
			dup2(usb_fd, STDIN_FILENO);
			dbg_set_file(fd_file_get(usb_fd));
			printf("brought up USB serial\n");
		} else {
			// ignore
		}
		
		// initialize twili
		static twili::Twili twili;
		
		while(!twili.destroy_flag) {
			ResultCode::AssertOk(twili.event_waiter.Wait(3000000000));
			twili.monitored_processes.remove_if(
				[](const auto &proc) {
					return proc->GetState() == twili::process::MonitoredProcess::State::Exited;
				});
		}
		
		printf("twili terminating...\n");
		printf("terminating monitored processes...\n");
		for(std::shared_ptr<twili::process::MonitoredProcess> proc : twili.monitored_processes) {
			proc->Terminate();
		}
		printf("done\n");
	} catch(trn::ResultError &e) {
		std::cout << "caught ResultError: " << e.what() << std::endl;
		fatal_init();
		fatal_transition_to_fatal_error(e.code.code, 0);
	}
	
	return 0;
}

namespace twili {

Twili::Twili() :
	event_waiter(),
	server(ResultCode::AssertOk(trn::ipc::server::IPCServer::Create(&event_waiter))),
	twili_registration(
		server, "twili",
		[this](auto s) {
			return new twili::service::ITwiliService(this);
		}),
	usb_bridge(this, std::make_shared<bridge::ITwibDeviceInterface>(0, *this)),
	tcp_bridge(*this, std::make_shared<bridge::ITwibDeviceInterface>(0, *this)),
	applet_tracker(*this) {
	printf("initialized Twili\n");
}

std::shared_ptr<process::MonitoredProcess> Twili::FindMonitoredProcess(uint64_t pid) {
	auto i = std::find_if(
		monitored_processes.begin(),
		monitored_processes.end(),
		[pid](auto const &proc) {
			return proc->GetPid() == pid;
		});
	if(i == monitored_processes.end()) {
		return {};
	} else {
		return *i;
	}
}

std::shared_ptr<process::Process> Twili::FindProcess(uint64_t pid) {
	std::shared_ptr<process::Process> proc = FindMonitoredProcess(pid);
	if(!proc) {
		proc = std::make_shared<process::UnmonitoredProcess>(*this, pid);
	}
	return proc;
}

Twili::ServiceRegistration::ServiceRegistration(trn::ipc::server::IPCServer &server, std::string name, std::function<trn::Result<trn::ipc::server::Object*>(trn::ipc::server::IPCServer *server)> factory) {
	ResultCode::AssertOk(server.CreateService(name.c_str(), factory));
}

Twili::Services::Services() {
	trn::service::SM sm = ResultCode::AssertOk(trn::service::SM::Initialize());

	pm_dmnt = ResultCode::AssertOk(sm.GetService("pm:dmnt"));
	
	pm_shell = twili::service::pm::IShellService(
		ResultCode::AssertOk(
			sm.GetService("pm:shell")));
	
	ldr_dmnt = twili::service::ldr::IDebugMonitorInterface(
		ResultCode::AssertOk(
			sm.GetService("ldr:dmnt")));

	printf("acquiring ldr:shel\n");
	ldr_shel = ResultCode::AssertOk(sm.GetService("ldr:shel"));
	
	ipc::client::Object nifm_static = ResultCode::AssertOk(
		sm.GetService("nifm:s"));
	
	ResultCode::AssertOk(
		nifm_static.SendSyncRequest<4>( // CreateGeneralService
			ipc::OutObject(nifm)));

	sm_m = ResultCode::AssertOk(sm.GetService("sm:m"));

	printf("acquired services\n");
}

Twili::Resources::Resources() {
	auto hbabi_shim_nro = util::ReadFile("/squash/hbabi_shim.nro");
	if(!hbabi_shim_nro) {
		throw trn::ResultError(TWILI_ERR_IO_ERROR);
	}
	this->hbabi_shim_nro = *hbabi_shim_nro;
	printf("prepared resources\n");
}

} // namespace twili
