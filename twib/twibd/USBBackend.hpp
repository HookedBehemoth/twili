#pragma once

#include "platform.hpp"

#include<thread>
#include<list>
#include<queue>
#include<mutex>
#include<condition_variable>

#include<libusb.h>

#include "Buffer.hpp"
#include "Device.hpp"
#include "Messages.hpp"
#include "Protocol.hpp"

namespace twili {
namespace twibd {

class Twibd;

namespace backend {

class USBBackend {
 public:
	USBBackend(Twibd *twibd);
	~USBBackend();
	
	class Device : public twibd::Device, public std::enable_shared_from_this<Device> {
	 public:
		Device(USBBackend *backend, libusb_device_handle *device, uint8_t endp_addrs[4], uint8_t interface_number);
		~Device();

		enum class State {
			AVAILABLE, BUSY
		};

		void Begin();
		
		// thread-agnostic
		virtual void SendRequest(const Request &&r) override;

		virtual int GetPriority() override;
		virtual std::string GetBridgeType() override;
		
		bool ready_flag = false;
		bool added_flag = false;
	 private:
		USBBackend *backend;
		
		libusb_device_handle *handle;
		uint8_t interface_number;
		uint8_t endp_meta_out;
		uint8_t endp_data_out;
		uint8_t endp_meta_in;
		uint8_t endp_data_in;
		libusb_transfer *tfer_meta_out = NULL;
		libusb_transfer *tfer_data_out = NULL;
		libusb_transfer *tfer_meta_in = NULL;
		libusb_transfer *tfer_data_in = NULL;
		State state = State::AVAILABLE;
		bool transferring_meta = false;
		bool transferring_data = false;
		std::mutex state_mutex;
		std::condition_variable state_cv;
		protocol::MessageHeader mhdr;
		protocol::MessageHeader mhdr_in;
		WeakRequest request_out;
		Response response_in;
		std::vector<uint32_t> object_ids_in;
		std::list<WeakRequest> pending_requests;

		std::shared_ptr<Device> *SharedPtrForTransfer();
		void MetaOutTransferCompleted();
		void DataOutTransferCompleted();
		void MetaInTransferCompleted();
		void DataInTransferCompleted();
		void ObjectInTransferCompleted();
		void DispatchResponse();
		void Identified(Response &r);
		void ResubmitMetaInTransfer();
		bool CheckTransfer(libusb_transfer *tfer);
		static size_t LimitTransferSize(size_t size);
		static void MetaOutTransferShim(libusb_transfer *tfer);
		static void DataOutTransferShim(libusb_transfer *tfer);
		static void MetaInTransferShim(libusb_transfer *tfer);
		static void DataInTransferShim(libusb_transfer *tfer);
		static void ObjectInTransferShim(libusb_transfer *tfer);
	};

	void Probe();
	void QueueAddDevice(libusb_device *device);
	void AddDevice(libusb_device *device);
	void RemoveDevice(libusb_context *ctx, libusb_device *device);

 private:
	Twibd *twibd;
	std::list<std::shared_ptr<Device>> devices;
	std::queue<libusb_device*> devices_to_add;
	
	class StdoutTransferState {
	 public:
		StdoutTransferState(libusb_device_handle *handle, uint8_t address);
		~StdoutTransferState();

		void Submit();
		void Kill(); // we have to delete this outside of the libusb event loop
		static void Callback(libusb_transfer *tfer);
		
		libusb_transfer *tfer;
		libusb_device_handle *handle;
		uint8_t address;
		uint8_t io_buffer[0x4000];
		util::Buffer string_buffer;
		bool deletion_flag = false;
	};

	std::list<std::shared_ptr<StdoutTransferState>> stdout_transfers;
	
	void ProbeStdioInterface(libusb_device *dev, const libusb_interface_descriptor *d);
	
	bool event_thread_destroy = false;
	void event_thread_func();
	std::thread event_thread;

	libusb_context *ctx;
	libusb_hotplug_callback_handle hotplug_handle;
	libusb_hotplug_callback_handle hotplug_handle_nintendo_sdk_debugger;
};

} // namespace backend
} // namespace twibd
} // namespace twili
