#include "acascard.h"
#include "dataUnit.h"
#include "ecm.h"
#include "m2SectionMessage.h"
#include "m2ShortSectionMessage.h"
#include "mhCdt.h"
#include "mhEit.h"
#include "mhSdt.h"
#include "mhTot.h"
#include "mhBit.h"
#include "mmtStream.h"
#include "MmtTlvDemuxer.h"
#include "mpt.h"
#include "fragmentAssembler.h"
#include "mfuDataProcessorFactory.h"
#include "nit.h"
#include "paMessage.h"
#include "plt.h"
#include "signalingMessage.h"
#include "smartcard.h"
#include "stream.h"
#include "mmtTableFactory.h"
#include "tlvTableFactory.h"
#include "demuxerHandler.h"
#include "mhStreamIdentificationDescriptor.h"
#include "videoMfuDataProcessor.h"
#include "videoComponentDescriptor.h"
#include "mhAudioComponentDescriptor.h"
#include "ipv6.h"
#include "ntp.h"

namespace MmtTlv {

MmtTlvDemuxer::MmtTlvDemuxer()
{
    smartCard = std::make_shared<Acas::SmartCard>();
    acasCard = std::make_unique<Acas::AcasCard>(smartCard);
}

bool MmtTlvDemuxer::init()
{
    try {
        smartCard->initCard();
        smartCard->connect();
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
    }

    return true;
}

void MmtTlvDemuxer::setDemuxerHandler(DemuxerHandler& demuxerHandler)
{
    this->demuxerHandler = &demuxerHandler;
}

int MmtTlvDemuxer::processPacket(Common::ReadStream& stream)
{
    if (stream.leftBytes() < 4) {
        return -1;
    }

    if (!isVaildTlv(stream)) {
        stream.skip(1);
        return -2;
    }

    if (!tlv.unpack(stream)) {
        return -1;
    }

    if (stream.leftBytes() < tlv.getDataLength()) {
        return -1;
    }

    Common::ReadStream tlvDataStream(tlv.getData());

    switch (tlv.getPacketType()) {
    case TlvPacketType::TransmissionControlSignalPacket:
    {
        processTlvTable(tlvDataStream);
        break;
    }
    case TlvPacketType::Ipv6Packet:
    {
        IPv6Header ipv6(false);
        ipv6.unpack(tlvDataStream);
        if (ipv6.nexthdr == IPv6::PROTOCOL_UDP) {
            UDPHeader udpHeader;
            udpHeader.unpack(tlvDataStream);

            // NTP
            if (udpHeader.destination_port == IPv6::PORT_NTP) {
                NTPv4 ntp;
                if (!ntp.unpack(tlvDataStream)) {
                    break;
                }

                demuxerHandler->onNtp(std::make_shared<NTPv4>(ntp));

            }
        }
        break;
    }
    case TlvPacketType::HeaderCompressedIpPacket:
    {
        compressedIPPacket.unpack(tlvDataStream);
        mmt.unpack(tlvDataStream);

        if (mmt.extensionHeaderScrambling.has_value()) {
            if (mmt.extensionHeaderScrambling->encryptionFlag == EncryptionFlag::ODD ||
                mmt.extensionHeaderScrambling->encryptionFlag == EncryptionFlag::EVEN) {
                if (!acasCard->ready) {
                    return 1;
                }
                else {
                    mmt.decryptPayload(&acasCard->lastDecryptedEcm);
                }
            }
        }

        Common::ReadStream mmtpPayloadStream(mmt.payload);
        switch (mmt.payloadType) {
        case PayloadType::Mpu:
            processMpu(mmtpPayloadStream);
            break;
        case PayloadType::ContainsOneOrMoreControlMessage:
            processSignalingMessages(mmtpPayloadStream);
            break;
        }
        break;
    }
    }

    return 1;
}

void MmtTlvDemuxer::processPaMessage(Common::ReadStream& stream)
{
    PaMessage message;
    message.unpack(stream);

    Common::ReadStream nstream(message.table);
    while (!nstream.isEof()) {
        processMmtTable(nstream);
    }
}

void MmtTlvDemuxer::processM2SectionMessage(Common::ReadStream& stream)
{
    M2SectionMessage message;
    message.unpack(stream);
    processMmtTable(stream);
}

void MmtTlvDemuxer::processM2ShortSectionMessage(Common::ReadStream& stream)
{
    M2ShortSectionMessage message;
    message.unpack(stream);
    processMmtTable(stream);
}

void MmtTlvDemuxer::processTlvTable(Common::ReadStream& stream)
{
    uint8_t tableId = stream.peek8U();
    const auto table = TlvTableFactory::create(tableId);
    if (table == nullptr) {
        return;
    }

    table->unpack(stream);

    switch (tableId) {
    case MmtTableId::Ecm:
        if (demuxerHandler) {
            demuxerHandler->onNit(std::dynamic_pointer_cast<Nit>(table));
        }
        break;
    }
}

void MmtTlvDemuxer::processMmtTable(Common::ReadStream& stream)
{
    uint8_t tableId = stream.peek8U();
    const auto table = MmtTableFactory::create(tableId);
    if (table == nullptr) {
        stream.skip(stream.leftBytes());
        return;
    }

    table->unpack(stream);

    switch (tableId) {
    case MmtTableId::Mpt:
    {
        processMmtPackageTable(std::dynamic_pointer_cast<Mpt>(table));
        break;
    }
    case MmtTableId::Ecm:
    {
        processEcm(std::dynamic_pointer_cast<Ecm>(table));
        break;
    }
    }

    switch (tableId) {
    case MmtTableId::Ecm:
        if (demuxerHandler) {
            demuxerHandler->onEcm(std::dynamic_pointer_cast<Ecm>(table));
        }
        break;
    case MmtTableId::MhCdt:
        if (demuxerHandler) {
            demuxerHandler->onMhCdt(std::dynamic_pointer_cast<MhCdt>(table));
        }
        break;
    case MmtTableId::MhEitPf:
    case MmtTableId::MhEitS_1:
    case MmtTableId::MhEitS_2:
    case MmtTableId::MhEitS_3:
    case MmtTableId::MhEitS_4:
    case MmtTableId::MhEitS_5:
    case MmtTableId::MhEitS_6:
    case MmtTableId::MhEitS_7:
    case MmtTableId::MhEitS_8:
    case MmtTableId::MhEitS_9:
    case MmtTableId::MhEitS_10:
    case MmtTableId::MhEitS_11:
    case MmtTableId::MhEitS_12:
    case MmtTableId::MhEitS_13:
    case MmtTableId::MhEitS_14:
    case MmtTableId::MhEitS_15:
    case MmtTableId::MhEitS_16:
        if (demuxerHandler) {
            demuxerHandler->onMhEit(std::dynamic_pointer_cast<MhEit>(table));
        }
        break;
    case MmtTableId::MhSdt:
        if (demuxerHandler) {
            demuxerHandler->onMhSdt(std::dynamic_pointer_cast<MhSdt>(table));
        }
        break;
    case MmtTableId::MhTot:
        if (demuxerHandler) {
            demuxerHandler->onMhTot(std::dynamic_pointer_cast<MhTot>(table));
        }
        break;
    case MmtTableId::Mpt:
        if (demuxerHandler) {
            demuxerHandler->onMpt(std::dynamic_pointer_cast<Mpt>(table));
        }
        break;
    case MmtTableId::Plt:
        if (demuxerHandler) {
            demuxerHandler->onPlt(std::dynamic_pointer_cast<Plt>(table));
        }
        break;
    case MmtTableId::MhBit:
        if (demuxerHandler) {
            demuxerHandler->onMhBit(std::dynamic_pointer_cast<MhBit>(table));
        }
        break;
    }
}

void MmtTlvDemuxer::processMmtPackageTable(const std::shared_ptr<Mpt>& mpt)
{
    // Remove streams that do not exist in the MPT
    std::map<uint16_t, uint32_t> mapMpt; // pid, assetType
    for (auto& asset : mpt->assets) {
        for (auto& locationInfo : asset.locationInfos) {
            if (locationInfo.locationType == 0) {
                mapMpt[locationInfo.packetId] = asset.assetType;
            }
        }
    }

    if (mapMpt.size()) {
        for (auto it = mapStream.begin(); it != mapStream.end(); ) {
            auto mptIt = mapMpt.find(it->first);
            if (mptIt != mapMpt.end()) {
                if (mptIt->second != it->second->assetType) {
                    it = mapStream.erase(it);
                }
                else {
                    ++it;
                }
            }
            else {
                it = mapStream.erase(it);
            }
        }
    }

    mapStreamByStreamIdx.clear();

    int streamIndex = 0;
    for (auto& asset : mpt->assets) {
        std::shared_ptr<MmtStream> mmtStream;

        for (auto& locationInfo : asset.locationInfos) {
            if (locationInfo.locationType == 0) {
                if (asset.assetType == AssetType::hev1 ||
                    asset.assetType == AssetType::mp4a ||
                    asset.assetType == AssetType::stpp) {
                    mmtStream = getStream(locationInfo.packetId);
                    if (!mmtStream) {
                        mmtStream = std::make_shared<MmtStream>(locationInfo.packetId);
                        mapStream[locationInfo.packetId] = mmtStream;
                    }
                    mmtStream->assetType = asset.assetType;
                    mmtStream->streamIndex = streamIndex;

                    if (!mmtStream->mfuDataProcessor) {
                        mmtStream->mfuDataProcessor = MfuDataProcessorFactory::create(mmtStream->assetType);
                    }

                    mapStreamByStreamIdx[streamIndex] = mmtStream;

                    ++streamIndex;
                }
            }
        }

        if (!mmtStream) {
            continue;
        }

        for (auto& descriptor : asset.descriptors.list) {
            switch (descriptor->getDescriptorTag()) {
            case MpuTimestampDescriptor::kDescriptorTag:
                processMpuTimestampDescriptor(std::dynamic_pointer_cast<MpuTimestampDescriptor>(descriptor), mmtStream);
                break;
            case MpuExtendedTimestampDescriptor::kDescriptorTag:
                processMpuExtendedTimestampDescriptor(std::dynamic_pointer_cast<MpuExtendedTimestampDescriptor>(descriptor), mmtStream);
                break;
            case MhStreamIdentificationDescriptor::kDescriptorTag:
            {
                auto mmtDescriptor = std::dynamic_pointer_cast<MmtTlv::MhStreamIdentificationDescriptor>(descriptor);
                mmtStream->componentTag = mmtDescriptor->componentTag;
                break;
            }
            case VideoComponentDescriptor::kDescriptorTag:
            {
                auto mmtDescriptor = std::dynamic_pointer_cast<MmtTlv::VideoComponentDescriptor>(descriptor);
                mmtStream->videoComponentDescriptor = mmtDescriptor;
                break;
            }
            case MhAudioComponentDescriptor::kDescriptorTag:
            {
                auto mmtDescriptor = std::dynamic_pointer_cast<MmtTlv::MhAudioComponentDescriptor>(descriptor);
                mmtStream->mhAudioComponentDescriptor = mmtDescriptor;
                break;
            }
            }
        }
    }
}

void MmtTlvDemuxer::processMpuTimestampDescriptor(const std::shared_ptr<MpuTimestampDescriptor>& descriptor, std::shared_ptr<MmtStream>& mmtStream)
{
    for (auto& ts : descriptor->entries) {
        bool find = false;
        for (int i = 0; i < mmtStream->mpuTimestamps.size(); i++) {
            if (mmtStream->mpuTimestamps[i].mpuSequenceNumber == ts.mpuSequenceNumber) {
                mmtStream->mpuTimestamps[i].mpuPresentationTime = ts.mpuPresentationTime;
                find = true;
                break;
            }
        }
        if (find) {
            continue;
        }

        for (int i = 0; i < mmtStream->mpuTimestamps.size(); i++) {
            if (mmtStream->mpuTimestamps[i].mpuSequenceNumber < mmtStream->lastMpuSequenceNumber) {
                mmtStream->mpuTimestamps[i].mpuSequenceNumber = ts.mpuSequenceNumber;
                mmtStream->mpuTimestamps[i].mpuPresentationTime = ts.mpuPresentationTime;
                find = true;
                break;
            }
        }

        if (find) {
            continue;
        }

        if (mmtStream->mpuTimestamps.size() >= 100) {
            int minIndex = 0;
            uint32_t minMpuSequenceNumber = 0xFFFFFFFF;
            for (int i = 0; i < mmtStream->mpuTimestamps.size(); i++) {
                if (minMpuSequenceNumber > mmtStream->mpuTimestamps[i].mpuSequenceNumber) {
                    minIndex = i;
                    minMpuSequenceNumber = mmtStream->mpuTimestamps[i].mpuSequenceNumber;
                }
            }

            mmtStream->mpuTimestamps[minIndex].mpuSequenceNumber = ts.mpuSequenceNumber;
            mmtStream->mpuTimestamps[minIndex].mpuPresentationTime = ts.mpuPresentationTime;
        }
        else {
            mmtStream->mpuTimestamps.push_back(ts);
        }
    }
}

void MmtTlvDemuxer::processMpuExtendedTimestampDescriptor(const std::shared_ptr<MpuExtendedTimestampDescriptor>& descriptor, std::shared_ptr<MmtStream>& mmtStream)
{
    if (descriptor->timescaleFlag) {
        mmtStream->timeBase.num = 1;
        mmtStream->timeBase.den = descriptor->timescale;
    }

    for (auto& ts : descriptor->entries) {
        if (mmtStream->lastMpuSequenceNumber > ts.mpuSequenceNumber)
            continue;

        bool find = false;
        for (int i = 0; i < mmtStream->mpuExtendedTimestamps.size(); i++) {
            if (mmtStream->mpuExtendedTimestamps[i].mpuSequenceNumber == ts.mpuSequenceNumber) {
                mmtStream->mpuExtendedTimestamps[i].mpuDecodingTimeOffset = ts.mpuDecodingTimeOffset;
                mmtStream->mpuExtendedTimestamps[i].numOfAu = ts.numOfAu;
                mmtStream->mpuExtendedTimestamps[i].ptsOffsets = ts.ptsOffsets;
                mmtStream->mpuExtendedTimestamps[i].dtsPtsOffsets = ts.dtsPtsOffsets;
                find = true;
                break;
            }
        }
        if (find) {
            continue;
        }

        for (int i = 0; i < mmtStream->mpuExtendedTimestamps.size(); i++) {
            if (mmtStream->mpuExtendedTimestamps[i].mpuSequenceNumber < mmtStream->lastMpuSequenceNumber) {
                mmtStream->mpuExtendedTimestamps[i].mpuSequenceNumber = ts.mpuSequenceNumber;
                mmtStream->mpuExtendedTimestamps[i].mpuDecodingTimeOffset = ts.mpuDecodingTimeOffset;
                mmtStream->mpuExtendedTimestamps[i].numOfAu = ts.numOfAu;
                mmtStream->mpuExtendedTimestamps[i].ptsOffsets = ts.ptsOffsets;
                mmtStream->mpuExtendedTimestamps[i].dtsPtsOffsets = ts.dtsPtsOffsets;
                find = true;
                break;
            }
        }
        if (find) {
            continue;
        }

        if (mmtStream->mpuExtendedTimestamps.size() >= 100) {
            int minIndex = 0;
            uint32_t minMpuSequenceNumber = 0xFFFFFFFF;
            for (int i = 0; i < mmtStream->mpuExtendedTimestamps.size(); i++) {
                if (minMpuSequenceNumber > mmtStream->mpuExtendedTimestamps[i].mpuSequenceNumber) {
                    minIndex = i;
                    minMpuSequenceNumber = mmtStream->mpuExtendedTimestamps[i].mpuSequenceNumber;
                }
            }

            mmtStream->mpuExtendedTimestamps[minIndex].mpuSequenceNumber = ts.mpuSequenceNumber;
            mmtStream->mpuExtendedTimestamps[minIndex].mpuDecodingTimeOffset = ts.mpuDecodingTimeOffset;
            mmtStream->mpuExtendedTimestamps[minIndex].numOfAu = ts.numOfAu;
            mmtStream->mpuExtendedTimestamps[minIndex].ptsOffsets = ts.ptsOffsets;
            mmtStream->mpuExtendedTimestamps[minIndex].dtsPtsOffsets = ts.dtsPtsOffsets;
        }
        else {
            mmtStream->mpuExtendedTimestamps.push_back(ts);
        }
    }
}

void MmtTlvDemuxer::processEcm(std::shared_ptr<Ecm> ecm)
{
    try {
        acasCard->decryptEcm(ecm->ecmData);
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
    }
}

void MmtTlvDemuxer::clear()
{
    mapAssembler.clear();
    mfuData.clear();
    mapStream.clear();
    mapStreamByStreamIdx.clear();

    if (acasCard) {
        acasCard->clear();
    }
}

std::shared_ptr<FragmentAssembler> MmtTlvDemuxer::getAssembler(uint16_t pid)
{
    if (mapAssembler.find(pid) == mapAssembler.end()) {
        auto assembler = std::make_shared<FragmentAssembler>();
        mapAssembler[pid] = assembler;
        return assembler;
    }
    else {
        return mapAssembler[pid];
    }
}

std::shared_ptr<MmtStream> MmtTlvDemuxer::getStream(uint16_t pid)
{
    if (mapStream.find(pid) == mapStream.end()) {
        return nullptr;
    }
    else {
        return mapStream[pid];
    }
}

void MmtTlvDemuxer::processMpu(Common::ReadStream& stream)
{
    if (!mpu.unpack(stream))
        return;

    auto assembler = getAssembler(mmt.packetId);
    Common::ReadStream nstream(mpu.payload);
    std::shared_ptr<MmtStream> mmtStream = getStream(mmt.packetId);

    if (!mmtStream) {
        return;
    }

    if (mpu.aggregateFlag && mpu.fragmentationIndicator != FragmentationIndicator::NotFragmented) {
        return;
    }

    if (mpu.fragmentType != FragmentType::Mfu) {
        return;
    }

    if (assembler->state == FragmentAssembler::State::Init && !mmt.rapFlag) {
        return;
    }

    if (assembler->state == FragmentAssembler::State::Init) {
        mmtStream->lastMpuSequenceNumber = mpu.mpuSequenceNumber;
    }
    else if (mpu.mpuSequenceNumber == mmtStream->lastMpuSequenceNumber + 1) {
        mmtStream->lastMpuSequenceNumber = mpu.mpuSequenceNumber;
        mmtStream->auIndex = 0;
    }
    else if (mpu.mpuSequenceNumber != mmtStream->lastMpuSequenceNumber) {
        std::cerr << "drop found (" << mmtStream->lastMpuSequenceNumber << " != " << mpu.mpuSequenceNumber << ")" << std::endl;
        assembler->state = FragmentAssembler::State::Init;
        return;
    }

    assembler->checkState(mmt.packetSequenceNumber);

    mmtStream->rapFlag = mmt.rapFlag;
    

    if (mpu.aggregateFlag == 0) {
        DataUnit dataUnit;
        if (!dataUnit.unpack(nstream, mpu.timedFlag, mpu.aggregateFlag)) {
            return;
        }

        if (assembler->assemble(dataUnit.data, mpu.fragmentationIndicator, mmt.packetSequenceNumber)) {
            Common::ReadStream dataStream(assembler->data);
            processMfuData(dataStream);
            assembler->clear();
        }
    }
    else
    {
        while (!nstream.isEof()) {
            DataUnit dataUnit;
            if (!dataUnit.unpack(nstream, mpu.timedFlag, mpu.aggregateFlag)) {
                return;
            }

            if (assembler->assemble(dataUnit.data, mpu.fragmentationIndicator, mmt.packetSequenceNumber)) {
                Common::ReadStream dataStream(assembler->data);
                processMfuData(dataStream);
                assembler->clear();
            }
        }
    }
}

void MmtTlvDemuxer::processMfuData(Common::ReadStream& stream)
{
    std::shared_ptr<MmtStream> mmtStream = getStream(mmt.packetId);
    if (!mmtStream) {
        return;
    }

    if (!mmtStream->mfuDataProcessor) {
        return;
    }

    std::vector<uint8_t> data(stream.leftBytes());
    stream.read(data.data(), stream.leftBytes());

    const auto ret = mmtStream->mfuDataProcessor->process(mmtStream, data);
    if (ret.has_value()) {
        const auto& mfuData = ret.value();
        auto it = std::next(mapStream.begin(), mfuData.streamIndex);
        if (it == mapStream.end()) {
            return;
        }

        switch (mmtStream->assetType) {
        case AssetType::hev1:
            if(demuxerHandler) {
                demuxerHandler->onVideoData(it->second, std::make_shared<MfuData>(mfuData));
            }
            break;
        case AssetType::mp4a:
            if(demuxerHandler) {
                demuxerHandler->onAudioData(it->second, std::make_shared<MfuData>(mfuData));
            }
            break;
        case AssetType::stpp:
            if(demuxerHandler) {
                demuxerHandler->onSubtitleData(it->second, std::make_shared<MfuData>(mfuData));
            }
            break;
        case AssetType::aapp:
            if(demuxerHandler) {
                demuxerHandler->onApplicationData(it->second, std::make_shared<MfuData>(mfuData));
            }
            break;
        }
    }
}

void MmtTlvDemuxer::processSignalingMessages(Common::ReadStream& stream)
{
    SignalingMessage signalingMessage;

    if (!signalingMessage.unpack(stream)) {
        return;
    }

    auto assembler = getAssembler(mmt.packetId);
    assembler->checkState(mmt.packetSequenceNumber);

    if (!signalingMessage.aggregationFlag) {
        if (assembler->assemble(signalingMessage.payload, signalingMessage.fragmentationIndicator, mmt.packetSequenceNumber)) {
            Common::ReadStream messageStream(assembler->data);
            processSignalingMessage(messageStream);
            assembler->clear();
        }
    }
    else {
        if (signalingMessage.fragmentationIndicator != FragmentationIndicator::NotFragmented) {
            return;
        }

        Common::ReadStream nstream(signalingMessage.payload);
        while (nstream.isEof()) {
            uint32_t length;

            if (signalingMessage.lengthExtensionFlag)
                length = nstream.getBe32U();
            else
                length = nstream.getBe16U();

            if (assembler->assemble(signalingMessage.payload, signalingMessage.fragmentationIndicator, mmt.packetSequenceNumber)) {
                Common::ReadStream messageStream(assembler->data);
                processSignalingMessage(messageStream);
                assembler->clear();
            }
        }
    }

    return;
}

void MmtTlvDemuxer::processSignalingMessage(Common::ReadStream& stream)
{
    MmtMessageId id = static_cast<MmtMessageId>(stream.peekBe16U());

    switch (id) {
    case MmtMessageId::PaMessage:
        return processPaMessage(stream);
    case MmtMessageId::M2SectionMessage:
        return processM2SectionMessage(stream);
    case MmtMessageId::M2ShortSectionMessage:
        return processM2ShortSectionMessage(stream);
        break;
    }
}

bool MmtTlvDemuxer::isVaildTlv(Common::ReadStream& stream) const
{
    try {
        uint8_t bytes[2];
        stream.peek((char*)bytes, 2);

        // syncByte
        if (bytes[0] != 0x7F) {
            return false;
        }

        // packetType
        if (bytes[1] > 0x04 && bytes[1] < 0xFD) {
            return false;
        }
    }
    catch (const std::runtime_error&) {
        return false;
    }
    return true;
}

}