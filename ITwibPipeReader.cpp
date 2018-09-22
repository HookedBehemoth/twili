#include "ITwibPipeReader.hpp"

#include "err.hpp"

using trn::ResultCode;
using trn::ResultError;

namespace twili {
namespace bridge {

ITwibPipeReader::ITwibPipeReader(uint32_t device_id, std::weak_ptr<TwibPipe> pipe) : Object(device_id), pipe(pipe) {
}

void ITwibPipeReader::HandleRequest(uint32_t command_id, std::vector<uint8_t> payload, bridge::ResponseOpener opener) {
	switch((protocol::ITwibPipeReader::Command) command_id) {
	case protocol::ITwibPipeReader::Command::READ:
		Read(payload, opener);
		break;
	default:
		opener.BeginError(ResultCode(TWILI_ERR_PROTOCOL_UNRECOGNIZED_FUNCTION)).Finalize();
		break;
	}
}

void ITwibPipeReader::Read(std::vector<uint8_t> payload, bridge::ResponseOpener opener) {
	if(payload.size() != 0) {
		throw ResultError(TWILI_ERR_BAD_REQUEST);
	}

	if(std::shared_ptr<TwibPipe> observe = pipe.lock()) {
		observe->Read(
			[opener](uint8_t *data, size_t actual_size) mutable {
				if(actual_size == 0) {
					opener.BeginError(TWILI_ERR_EOF).Finalize();
				} else {
					auto r = opener.BeginOk(actual_size);
					r.Write(data, actual_size);
					r.Finalize();
				}
				return actual_size;
			});
	} else {
		opener.BeginError(TWILI_ERR_EOF).Finalize();
	}
}

} // namespace bridge
} // namespace twili
