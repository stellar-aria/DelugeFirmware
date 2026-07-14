#include "io/file.hpp"

namespace deluge::io {

Status to_status(DelugeStatus status) {
	switch (status) {
	case DELUGE_OK:
		return Status::OK;
	case DELUGE_ERR:
		return Status::ERR;
	case DELUGE_ERR_PARAM:
		return Status::PARAM;
	case DELUGE_ERR_BUSY:
		return Status::BUSY;
	case DELUGE_ERR_TIMEOUT:
		return Status::TIMEOUT;
	case DELUGE_ERR_IO:
		return Status::IO;
	case DELUGE_ERR_NODEV:
		return Status::NODEV;
	case DELUGE_ERR_UNSUPPORTED:
		return Status::UNSUPPORTED;
	case DELUGE_ERR_NOT_FOUND:
		return Status::NOT_FOUND;
	case DELUGE_ERR_EXISTS:
		return Status::EXISTS;
	case DELUGE_ERR_NO_SPACE:
		return Status::NO_SPACE;
	case DELUGE_ERR_NO_FILESYSTEM:
		return Status::NO_FILESYSTEM;
	case DELUGE_ERR_WRITE_PROTECTED:
		return Status::WRITE_PROTECTED;
	case DELUGE_ERR_NO_MEMORY:
		return Status::NO_MEMORY;
	}
	return Status::ERR; // unreachable while the switch above stays exhaustive
}

} // namespace deluge::io
