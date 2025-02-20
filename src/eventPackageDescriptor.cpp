#include "eventPackageDescriptor.h"

namespace MmtTlv {

bool EventPackageDescriptor::unpack(Common::ReadStream& stream)
{
    try {
        if (!MmtDescriptorTemplate::unpack(stream)) {
            return false;
        }

        Common::ReadStream nstream(stream, descriptorLength);

        mmtPackageIdLength = nstream.get8U();

        mmtPackageIdByte.resize(mmtPackageIdLength);
        nstream.read(mmtPackageIdByte.data(), mmtPackageIdLength);

        stream.skip(descriptorLength);
    }
    catch (const std::out_of_range&) {
        return false;
    }

    return true;
}

}