#include<unistd.h>

#include "IPipe.hpp"

#include "err.hpp"

namespace twili {

IPipe::IPipe(trn::ipc::server::IPCServer *server) : trn::ipc::server::Object(server) {
}

IPipeStandard::IPipeStandard(trn::ipc::server::IPCServer *server, int fd) :
	IPipe(server),
	fd(fd) {
}

trn::ResultCode IPipeStandard::Dispatch(trn::ipc::Message msg, uint32_t request_id) {
	switch(request_id) {
	case 0:
		return trn::ipc::server::RequestHandler<&IPipeStandard::Read>::Handle(this, msg);
	case 1:
		return trn::ipc::server::RequestHandler<&IPipeStandard::Write>::Handle(this, msg);
	}
	return 1;
}

trn::ResultCode IPipeStandard::Read(trn::ipc::OutRaw<uint64_t> size, trn::ipc::Buffer<uint8_t, 0x6, 0> buffer) {
	size = read(fd, buffer.data, buffer.size);
	return RESULT_OK;
}

trn::ResultCode IPipeStandard::Write(trn::ipc::Buffer<uint8_t, 0x5, 0> buffer) {
	size_t written = 0;
	while(written < buffer.size) {
		ssize_t ret = write(fd, buffer.data + written, buffer.size - written);
		if(ret <= 0) {
			return 1;
		}
		written+= ret;
	}
	return RESULT_OK;
}

IPipeTwib::IPipeTwib(trn::ipc::server::IPCServer *server, std::shared_ptr<TwibPipe> pipe) :
	IPipe(server), pipe(pipe) {

}

trn::ResultCode IPipeTwib::Dispatch(trn::ipc::Message msg, uint32_t request_id) {
	try {
		switch(request_id) {
		case 0:
			return trn::ipc::server::RequestHandler<&IPipeTwib::Read>::Handle(this, msg);
		case 1:
			return trn::ipc::server::RequestHandler<&IPipeTwib::Write>::Handle(this, msg);
		}
		return 1;
	} catch(trn::ResultError &e) {
		printf("caught result error 0x%x while dispatching for IPipeTwib\n", e.code.code);
		throw e;
	}
}

trn::ResultCode IPipeTwib::Read(std::function<void(trn::ResultCode)> cb, trn::ipc::OutRaw<uint64_t> size, trn::ipc::Buffer<uint8_t, 0x6, 0> buffer) {
	pipe->Read(
		[cb, size, buffer](void *data, size_t data_size) mutable {
			if(data_size > buffer.size) {
				data_size = buffer.size;
			}
			size = data_size;
			memcpy(buffer.data, data, data_size);
			cb(RESULT_OK);
			return data_size;
		});
	return RESULT_OK;
}

trn::ResultCode IPipeTwib::Write(std::function<void(trn::ResultCode)> cb, trn::ipc::Buffer<uint8_t, 0x5, 0> buffer) {
	pipe->Write(
		buffer.data, buffer.size,
		[cb](bool is_closed) { cb(is_closed ? TWILI_ERR_EOF : RESULT_OK); });
	return RESULT_OK;
}

} // namespace twili
