#pragma once

#include "Twib.hpp"

namespace twili {
namespace twib {

class RemoteObject {
 public:
	RemoteObject(std::shared_ptr<Client> client, uint32_t device_id, uint32_t object_id);
	~RemoteObject();

	std::future<Response> SendRequest(uint32_t command_id, std::vector<uint8_t> payload);
	Response SendSyncRequest(uint32_t command_id, std::vector<uint8_t> payload = std::vector<uint8_t>());

	template<typename T>
	Response SendSyncRequest(T command_id, std::vector<uint8_t> payload = std::vector<uint8_t>()) {
		return SendSyncRequest((uint32_t) command_id, payload);
	}
 private:
	const std::shared_ptr<Client> client;
	const uint32_t device_id;
	const uint32_t object_id;
};

} // namespace twib
} // namespace twili
