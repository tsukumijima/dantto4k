#include "mhBit.h"

namespace MmtTlv {

bool MhBit::unpack(Common::Stream& stream)
{
    try {
        if (!MmtTableBase::unpack(stream)) {
            return false;
        }

        uint16_t uint16 = stream.getBe16U();
        sectionSyntaxIndicator = (uint16 & 0b1000000000000000) >> 15;
        sectionLength = uint16 & 0b0000111111111111;

        originalNetworkId = stream.getBe16U();

        uint8_t uint8 = stream.get8U();
        versionNumber = (uint8 & 0b00111110) >> 1;
        currentNextIndicator = uint8 & 1;
        sectionNumber = stream.get8U();
        lastSectionNumber = stream.get8U();

        uint16 = stream.getBe16U();
        broadcastViewPropriety = (uint16 & 0b0001000000000000) >> 12;
        firstDescriptorsLength = uint16 & 0b0000111111111111;
        
        Common::Stream nstream(stream, firstDescriptorsLength);
        descriptors.unpack(nstream);
        stream.skip(firstDescriptorsLength);

        while (stream.leftBytes()) {
            Broadcaster entry;
            if (!entry.unpack(stream)) {
                return false;
            }

            broadcasters.push_back(entry);
        }

        crc32 = stream.getBe32U();
    }
    catch (const std::out_of_range&) {
        return false;
    }

    return true;
}

bool MhBit::Broadcaster::unpack(Common::Stream& stream)
{
    try {
        broadcasterId = stream.get8U();

        uint16_t uint16 = stream.getBe16U();
        broadcasterDescriptorsLength = uint16 & 0b0000111111111111;
        
        Common::Stream nstream(stream, broadcasterDescriptorsLength);
        descriptors.unpack(nstream);
        stream.skip(broadcasterDescriptorsLength);
    }
    catch (const std::out_of_range&) {
        return false;
    }

    return true;
}
}