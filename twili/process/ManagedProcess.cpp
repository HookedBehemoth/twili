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

#include "ManagedProcess.hpp"

#include "../twili.hpp"

#include "../process_creation.hpp"

using namespace trn;

namespace twili {
namespace process {

static std::vector<uint32_t> caps = {
	0b00011111111111111111111111101111, // SVC grants
	0b00111111111111111111111111101111,
	0b01011111111111111111111111101111,
	0b01100000000000001111111111101111,
	0b10011111111111111111111111101111,
	0b10100000000000000000111111101111,
	0b00000010000000000111001110110111, // KernelFlags
	0b00000000000000000101111111111111, // ApplicationType
	0b00000000000110000011111111111111, // KernelReleaseVersion
	0b00000010000000000111111111111111, // HandleTableSize
	0b00000000000001101111111111111111, // DebugFlags (can be debugged)
};

ManagedProcess::ManagedProcess(Twili &twili) :
	MonitoredProcess(twili),
	hbabi_shim_reader(twili.resources.hbabi_shim_nro) {
	uint64_t shim_addr = ResultCode::AssertOk(builder.AppendNRO(hbabi_shim_reader));
}

ManagedProcess::~ManagedProcess() {
	twili.services.sm_m.SendSyncRequest<1>(
		trn::ipc::InRaw<uint64_t>(GetPid()));
}

void ManagedProcess::Launch(bridge::ResponseOpener response) {
	std::shared_ptr<trn::KProcess> process = ResultCode::AssertOk(builder.Build("twili_child", caps));

	ChangeState(State::Started);
	Attach(process);

	printf("registering sac...\n");
	uint8_t sac[] = {0x00, '*', 0x80, '*'};
	ResultCode::AssertOk(
		twili.services.sm_m.SendSyncRequest<0>(
			trn::ipc::InRaw<uint64_t>(GetPid()),
			trn::ipc::Buffer<uint8_t, 0x5>(sac, sizeof(sac)),
			trn::ipc::Buffer<uint8_t, 0x5>(sac, sizeof(sac))
			));
	has_registered_sac = true;
	printf("  registered sac.\n");
	
	printf("created managed process: 0x%x, pid 0x%x\n", proc->handle, GetPid());
	wait = twili.event_waiter.Add(*proc, [this]() {
			printf("managed process (0x%x) signalled\n", this->proc->handle);
			this->proc->ResetSignal();
			auto state = (trn::svc::ProcessState) ResultCode::AssertOk(trn::svc::GetProcessInfo(*this->proc, 0));
			printf("  state: 0x%lx\n", state);
			if(state == trn::svc::ProcessState::CRASHED) {
				printf("managed process (0x%x) crashed\n", this->proc->handle);
				printf("ready to generate crash report\n");
				ChangeState(State::Crashed);
			}
			if(state == trn::svc::ProcessState::EXITED) {
				printf("managed process (0x%x) exited\n", this->proc->handle);
				ChangeState(State::Exited);
				return false; // unsubscribe from further signaling
			}
			return true;
		});

	printf("launching managed process: 0x%x\n", proc->handle);
	auto r = trn::svc::StartProcess(*proc, 58, 0, 0x100000);
	if(!r) {
		SetResult(r.error());
		ChangeState(State::Exited);
		
		response.BeginError(r.error()).Finalize();
	} else {
		auto w = response.BeginOk(sizeof(uint64_t));
		w.Write<uint64_t>(GetPid());
		w.Finalize();
	}
}

void ManagedProcess::AppendCode(std::vector<uint8_t> nro) {
	process_creation::ProcessBuilder::VectorDataReader &r =
		readers.emplace_back(nro);
	uint64_t base = ResultCode::AssertOk(builder.AppendNRO(r));
	if(target_entry == 0) {
		target_entry = base;
	}
}

} // namespace process
} // namespace twili
