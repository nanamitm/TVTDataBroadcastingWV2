#include "pch.h"
#include "B62AppReceiver.h"
#include "ZlibInflate.h"

namespace B62
{

namespace
{

constexpr uint8_t kTableMhAit = 0x9C;
constexpr uint8_t kTableDdmt = 0xA3;
constexpr uint8_t kTableDamt = 0xA4;
constexpr uint8_t kTableItem = 0xC0;
constexpr uint16_t kDescriptorTransportProtocol = 0x802A;
constexpr uint16_t kDescriptorSimpleApplicationLocation = 0x802B;
constexpr uint8_t kControlCodeAutostart = 0x01;
constexpr uint8_t kNoCompression = 0xFF;
constexpr uint8_t kCompressionZlib = 0x00;
constexpr size_t kMaxItemBytes = 64 * 1024 * 1024;
constexpr size_t kMaxFileSize = 16 * 1024 * 1024;
constexpr size_t kMaxIndexItemSize = 1024 * 1024;
constexpr size_t kMaxPendingItems = 16;

uint32_t Crc32Mpeg2(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++)
    {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
        }
    }
    return crc;
}

// Bounds-checked big-endian reader for untrusted section data. Reads past the
// end set failed and return zero.
class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : data(data), size(size) {}

    bool Failed() const { return failed; }
    size_t Left() const { return failed ? 0 : size - pos; }
    const uint8_t* Current() const { return data + pos; }

    uint8_t U8()
    {
        if (Left() < 1) { failed = true; return 0; }
        return data[pos++];
    }
    uint16_t U16() { uint16_t v = U8(); return static_cast<uint16_t>((v << 8) | U8()); }
    uint32_t U32() { uint32_t v = U16(); return (v << 16) | U16(); }
    std::string String(size_t length)
    {
        if (Left() < length) { failed = true; return {}; }
        std::string s(reinterpret_cast<const char*>(data + pos), length);
        pos += length;
        return s;
    }
    void Skip(size_t length)
    {
        if (Left() < length) { failed = true; return; }
        pos += length;
    }
    Reader Sub(size_t length)
    {
        if (Left() < length) { failed = true; return Reader(data, 0); }
        Reader r(data + pos, length);
        pos += length;
        return r;
    }

private:
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool failed = false;
};

// Joins broadcast path pieces, dropping empty, "." and ".." components so the
// result can only name something inside the application root.
std::string JoinPath(std::initializer_list<std::string_view> parts)
{
    std::string result;
    for (auto part : parts)
    {
        size_t start = 0;
        while (start <= part.size())
        {
            size_t end = part.find('/', start);
            if (end == std::string_view::npos)
            {
                end = part.size();
            }
            auto component = part.substr(start, end - start);
            if (!component.empty() && component != "." && component != "..")
            {
                if (!result.empty())
                {
                    result += '/';
                }
                result += component;
            }
            start = end + 1;
        }
    }
    return result;
}

std::string GuessMimeType(const std::string& path)
{
    auto dot = path.rfind('.');
    if (dot == std::string::npos)
    {
        return "application/octet-stream";
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::unordered_map<std::string, std::string> types = {
        { "html", "text/html" }, { "htm", "text/html" }, { "js", "application/javascript" },
        { "css", "text/css" }, { "json", "application/json" }, { "svg", "image/svg+xml" },
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" },
        { "webp", "image/webp" }, { "woff", "font/woff" }, { "woff2", "font/woff2" }, { "ttf", "font/ttf" },
        { "otf", "font/otf" }, { "mp3", "audio/mpeg" }, { "mp4", "video/mp4" }, { "aac", "audio/aac" },
        { "xml", "application/xml" }, { "txt", "text/plain" },
    };
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

struct IndexEntry
{
    std::string fileName;
    std::string itemType;
    uint8_t compressionType = kNoCompression;
    uint32_t originalSize = 0;
};

// Index item (ARIB STD-B60 10.2.5): item_id -> file name, type and compression.
bool ParseIndexItem(const std::vector<uint8_t>& data, std::map<uint32_t, IndexEntry>& entries)
{
    Reader r(data.data(), data.size());
    const uint16_t count = r.U16();
    std::map<uint32_t, IndexEntry> parsed;
    for (int i = 0; i < count && !r.Failed(); i++)
    {
        IndexEntry entry;
        const uint32_t itemId = r.U32();
        r.U32(); // item_size
        r.U8();  // item_version
        entry.fileName = r.String(r.U8());
        if (r.U8() & 0x80)
        {
            r.U32(); // item_checksum
        }
        entry.itemType = r.String(r.U8());
        entry.compressionType = r.U8();
        if (entry.compressionType != kNoCompression)
        {
            entry.originalSize = r.U32();
        }
        parsed[itemId] = std::move(entry);
    }
    if (r.Failed() || r.Left() != 0 || parsed.empty())
    {
        return false;
    }
    entries = std::move(parsed);
    return true;
}

}

void AppReceiver::SetApplicationChangedCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(this->mutex);
    this->applicationChangedCallback = std::move(callback);
}

void AppReceiver::Reset(int newServiceId)
{
    bool hadApplication;
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        hadApplication = this->application.has_value();
        this->serviceId = newServiceId;
        this->ResetLocked();
        callback = this->applicationChangedCallback;
    }
    if (hadApplication && callback)
    {
        callback();
    }
}

void AppReceiver::ResetLocked()
{
    this->pmtPid = -1;
    this->appPid = -1;
    this->sectionBuffers.clear();
    this->ResetApplicationLocked();
}

void AppReceiver::ResetApplicationLocked()
{
    this->sectionCrcs.clear();
    this->damtSections.clear();
    this->ddmtSections.clear();
    this->pendingItems.clear();
    this->items.clear();
    this->itemBytes = 0;
    this->application.reset();
    this->pathIndexDirty = true;
    this->pathIndex.clear();
    this->fileCache.clear();
}

void AppReceiver::FeedPacket(const uint8_t* packet)
{
    if (packet[0] != 0x47 || (packet[1] & 0x80))
    {
        return;
    }
    const uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];

    std::optional<Application> before;
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (pid != 0 && pid != this->pmtPid && pid != this->appPid)
        {
            return;
        }
        before = this->application;
        this->FeedSectionData(pid, packet, this->sectionBuffers[pid]);
        if (before == this->application)
        {
            return;
        }
        callback = this->applicationChangedCallback;
    }
    if (callback)
    {
        callback();
    }
}

void AppReceiver::FeedSectionData(uint16_t pid, const uint8_t* packet, SectionBuffer& buffer)
{
    const int adaptationFieldControl = (packet[3] >> 4) & 0x03;
    if (!(adaptationFieldControl & 1))
    {
        return;
    }
    const int continuityCounter = packet[3] & 0x0F;
    if (buffer.continuityCounter >= 0 && continuityCounter == buffer.continuityCounter)
    {
        return; // duplicate packet
    }
    const bool continuous = buffer.continuityCounter < 0 || continuityCounter == ((buffer.continuityCounter + 1) & 0x0F);
    buffer.continuityCounter = continuityCounter;
    if (!continuous)
    {
        buffer.data.clear();
        buffer.synced = false;
    }

    size_t offset = 4;
    if (adaptationFieldControl & 2)
    {
        offset += 1 + packet[4];
    }
    if (offset >= 188)
    {
        return;
    }
    const uint8_t* payload = packet + offset;
    size_t payloadSize = 188 - offset;

    if (packet[1] & 0x40)
    {
        const size_t pointer = payload[0];
        if (1 + pointer > payloadSize)
        {
            buffer.data.clear();
            buffer.synced = false;
            return;
        }
        if (buffer.synced)
        {
            buffer.data.insert(buffer.data.end(), payload + 1, payload + 1 + pointer);
        }
        else
        {
            buffer.data.clear();
        }
        // Sections completed by the bytes before the pointer are handled below;
        // anything left over is the unfinished tail of a lost section.
        auto flush = [&](bool keepTail) {
            size_t pos = 0;
            while (buffer.data.size() - pos >= 3 && buffer.data[pos] != 0xFF)
            {
                const size_t length = 3 + (((buffer.data[pos + 1] & 0x0F) << 8) | buffer.data[pos + 2]);
                if (buffer.data.size() - pos < length)
                {
                    break;
                }
                this->OnSection(pid, buffer.data.data() + pos, length);
                pos += length;
            }
            if (keepTail && pos < buffer.data.size() && buffer.data[pos] != 0xFF)
            {
                buffer.data.erase(buffer.data.begin(), buffer.data.begin() + pos);
            }
            else
            {
                buffer.data.clear();
            }
        };
        flush(false);
        buffer.data.assign(payload + 1 + pointer, payload + payloadSize);
        buffer.synced = true;
        flush(true);
        return;
    }

    if (!buffer.synced)
    {
        return;
    }
    buffer.data.insert(buffer.data.end(), payload, payload + payloadSize);
    if (buffer.data.size() > 4096 + 188)
    {
        buffer.data.clear();
        buffer.synced = false;
        return;
    }
    size_t pos = 0;
    while (buffer.data.size() - pos >= 3 && buffer.data[pos] != 0xFF)
    {
        const size_t length = 3 + (((buffer.data[pos + 1] & 0x0F) << 8) | buffer.data[pos + 2]);
        if (buffer.data.size() - pos < length)
        {
            break;
        }
        this->OnSection(pid, buffer.data.data() + pos, length);
        pos += length;
    }
    if (pos < buffer.data.size() && buffer.data[pos] == 0xFF)
    {
        buffer.data.clear();
    }
    else
    {
        buffer.data.erase(buffer.data.begin(), buffer.data.begin() + pos);
    }
}

void AppReceiver::OnSection(uint16_t pid, const uint8_t* section, size_t size)
{
    // Every section used here has section_syntax_indicator=1 and a CRC_32.
    if (size < 12 || !(section[1] & 0x80) || Crc32Mpeg2(section, size) != 0)
    {
        return;
    }
    if (pid == 0)
    {
        this->OnPat(section, size);
    }
    else if (pid == this->pmtPid)
    {
        this->OnPmt(section, size);
    }
    else if (pid == this->appPid)
    {
        this->OnAppSection(section, size);
    }
}

void AppReceiver::OnPat(const uint8_t* section, size_t size)
{
    if (section[0] != 0x00)
    {
        return;
    }
    Reader r(section + 8, size - 12);
    while (r.Left() >= 4)
    {
        const uint16_t programNumber = r.U16();
        const uint16_t pid = r.U16() & 0x1FFF;
        if (programNumber == 0)
        {
            continue;
        }
        if (this->serviceId < 0 || programNumber == this->serviceId)
        {
            if (this->pmtPid != pid)
            {
                this->sectionBuffers.erase(static_cast<uint16_t>(this->pmtPid));
                this->pmtPid = pid;
            }
            return;
        }
    }
}

void AppReceiver::OnPmt(const uint8_t* section, size_t size)
{
    if (section[0] != 0x02)
    {
        return;
    }
    const uint16_t programNumber = (section[3] << 8) | section[4];
    if (this->serviceId >= 0 && programNumber != this->serviceId)
    {
        return;
    }
    Reader r(section + 8, size - 12);
    r.U16(); // PCR_PID
    r.Skip(r.U16() & 0x0FFF);
    int found = -1;
    while (r.Left() >= 5 && found < 0)
    {
        const uint8_t streamType = r.U8();
        const uint16_t pid = r.U16() & 0x1FFF;
        Reader descriptors = r.Sub(r.U16() & 0x0FFF);
        while (descriptors.Left() >= 2)
        {
            const uint8_t tag = descriptors.U8();
            Reader body = descriptors.Sub(descriptors.U8());
            if (streamType == 0x05 && tag == 0x05 && body.Left() >= 4 && body.U32() == kFormatIdentifier)
            {
                found = pid;
                break;
            }
        }
    }
    if (found == 0 || found == this->pmtPid)
    {
        found = -1;
    }
    if (found != this->appPid)
    {
        // Called while the PMT's own section buffer is in use, so only other
        // PIDs' buffers may be dropped here.
        if (this->appPid >= 0)
        {
            this->sectionBuffers.erase(static_cast<uint16_t>(this->appPid));
        }
        this->ResetApplicationLocked();
        this->appPid = found;
    }
}

void AppReceiver::OnAppSection(const uint8_t* section, size_t size)
{
    const uint8_t tableId = section[0];
    if (tableId == kTableItem)
    {
        this->OnItemSection(section, size);
        return;
    }

    const SectionKey key{ tableId, static_cast<uint16_t>((section[3] << 8) | section[4]), section[6] };
    const uint32_t crc = (section[size - 4] << 24) | (section[size - 3] << 16) | (section[size - 2] << 8) | section[size - 1];
    auto [it, inserted] = this->sectionCrcs.try_emplace(key, crc);
    if (!inserted)
    {
        if (it->second == crc)
        {
            return; // unchanged repeat
        }
        it->second = crc;
    }

    switch (tableId)
    {
    case kTableMhAit:
        this->OnMhAit(section, size);
        break;
    case kTableDdmt:
        this->OnDdmt(key, section, size);
        break;
    case kTableDamt:
        this->OnDamt(key, section, size);
        break;
    }
}

void AppReceiver::OnMhAit(const uint8_t* section, size_t size)
{
    Reader r(section + 3, size - 7);
    r.U16(); // application_type
    const uint8_t version = (r.U8() >> 1) & 0x1F;
    r.U8(); // section_number
    r.U8(); // last_section_number
    r.Skip(r.U16() & 0x0FFF); // common descriptors
    Reader loop = r.Sub(r.U16() & 0x0FFF);

    std::optional<Application> selected;
    while (loop.Left() >= 9 && !loop.Failed())
    {
        Application app;
        app.organizationId = loop.U32();
        app.applicationId = loop.U16();
        app.controlCode = loop.U8();
        app.aitVersion = version;
        Reader descriptors = loop.Sub(loop.U16() & 0x0FFF);

        std::string base;
        std::string initialPath;
        while (descriptors.Left() >= 3)
        {
            const uint16_t tag = descriptors.U16();
            Reader body = descriptors.Sub(descriptors.U8());
            if (tag == kDescriptorTransportProtocol)
            {
                const uint16_t protocolId = body.U16();
                body.U8(); // transport_protocol_label
                if (protocolId == 0x0005)
                {
                    // Broadcast: URL_base length and bytes, e.g. "sh4/40/001/".
                    base = body.String(body.U8());
                }
            }
            else if (tag == kDescriptorSimpleApplicationLocation)
            {
                initialPath = body.String(body.Left());
            }
        }
        if (loop.Failed() || initialPath.empty())
        {
            continue;
        }
        app.entryPath = JoinPath({ base, initialPath });
        if (app.controlCode == kControlCodeAutostart && !selected)
        {
            selected = app;
        }
    }
    if (!loop.Failed())
    {
        this->application = selected;
    }
}

void AppReceiver::OnDdmt(const SectionKey& key, const uint8_t* section, size_t size)
{
    Reader r(section + 3, size - 7);
    const uint8_t session = r.U8();
    r.U8();  // reserved
    r.U8();  // version_number, current_next_indicator
    r.U16(); // section_number, last_section_number
    const std::string base = r.String(r.U8());
    DdmtSection parsed;
    const uint8_t nodeCount = r.U8();
    for (int i = 0; i < nodeCount && !r.Failed(); i++)
    {
        const uint16_t nodeTag = r.U16();
        r.U8(); // directory_node_version
        DdmtNode node{ base, r.String(r.U8()) };
        const uint16_t fileCount = r.U16();
        for (int j = 0; j < fileCount && !r.Failed(); j++)
        {
            const uint16_t fileNodeTag = r.U16();
            parsed.files[{ session, fileNodeTag }] = DdmtFile{ nodeTag, r.String(r.U8()) };
        }
        parsed.nodes[{ session, nodeTag }] = std::move(node);
    }
    if (r.Failed())
    {
        return;
    }
    this->ddmtSections[key] = std::move(parsed);
    this->pathIndexDirty = true;
}

void AppReceiver::OnDamt(const SectionKey& key, const uint8_t* section, size_t size)
{
    Reader r(section + 3, size - 7);
    const uint8_t session = r.U8();
    r.U8();  // reserved
    r.U8();  // version_number, current_next_indicator
    r.U16(); // section_number, last_section_number
    r.U32(); // transaction_id
    const uint16_t componentTag = r.U16();
    r.U32(); // download_id
    std::map<std::pair<uint16_t, uint32_t>, DamtMpu> parsed;
    const uint8_t mpuCount = r.U8();
    for (int i = 0; i < mpuCount && !r.Failed(); i++)
    {
        DamtMpu mpu;
        mpu.sessionId = session;
        const uint32_t mpuSequenceNumber = r.U32();
        r.U32(); // MPU_size
        const uint8_t flags = r.U8();
        mpu.indexItemFlag = flags & 0x80;
        if (mpu.indexItemFlag && (flags & 0x40))
        {
            mpu.indexItemId = r.U32();
        }
        const uint16_t itemCount = r.U16();
        for (int j = 0; j < itemCount && !r.Failed(); j++)
        {
            const uint16_t nodeTag = r.U16();
            if (mpu.indexItemFlag)
            {
                continue;
            }
            const uint32_t itemId = r.U32();
            r.U32(); // item_size
            r.U8();  // item_version
            if (r.U8() & 0x80)
            {
                r.U32(); // item_checksum
            }
            r.Skip(r.U8()); // item_info
            mpu.items.emplace_back(nodeTag, itemId);
        }
        // MPU_info: an MPU node descriptor gives the directory of index items.
        Reader info = r.Sub(r.U8());
        if (info.Left() == 5)
        {
            info.Skip(3);
            mpu.nodeTag = info.U16();
        }
        parsed[{ componentTag, mpuSequenceNumber }] = std::move(mpu);
    }
    if (r.Failed())
    {
        return;
    }
    this->damtSections[key] = std::move(parsed);
    this->pathIndexDirty = true;
}

void AppReceiver::OnItemSection(const uint8_t* section, size_t size)
{
    const uint16_t sequence = (section[3] << 8) | section[4];
    const uint8_t number = section[6];
    const uint8_t last = section[7];
    if (number > last)
    {
        return;
    }

    auto it = this->pendingItems.find(sequence);
    if (it == this->pendingItems.end() || it->second.lastSectionNumber != last)
    {
        if (this->pendingItems.size() >= kMaxPendingItems)
        {
            this->pendingItems.erase(this->pendingItems.begin());
        }
        it = this->pendingItems.insert_or_assign(sequence, PendingItem{ last, {} }).first;
    }
    it->second.parts[number].assign(section + 8, section + size - 4);
    if (it->second.parts.size() != static_cast<size_t>(last) + 1)
    {
        return;
    }

    std::vector<uint8_t> body;
    for (auto& [_, part] : it->second.parts)
    {
        body.insert(body.end(), part.begin(), part.end());
    }
    this->pendingItems.erase(it);
    if (body.size() < 14)
    {
        return;
    }
    const uint16_t componentTag = (body[0] << 8) | body[1];
    const uint32_t mpu = (body[2] << 24) | (body[3] << 16) | (body[4] << 8) | body[5];
    const uint32_t itemId = (body[6] << 24) | (body[7] << 16) | (body[8] << 8) | body[9];
    const uint32_t itemSize = (body[10] << 24) | (body[11] << 16) | (body[12] << 8) | body[13];
    if (itemSize != body.size() - 14)
    {
        return;
    }

    auto& slot = this->items[{ componentTag, mpu, itemId }];
    if (slot.size() == itemSize && std::equal(slot.begin(), slot.end(), body.begin() + 14))
    {
        return; // periodic resend
    }
    this->itemBytes -= slot.size();
    slot.assign(body.begin() + 14, body.end());
    this->itemBytes += slot.size();
    this->pathIndexDirty = true;

    if (this->itemBytes > kMaxItemBytes)
    {
        // Something is not being pruned; start over rather than grow forever.
        this->items.clear();
        this->itemBytes = 0;
        this->fileCache.clear();
    }
}

void AppReceiver::RebuildPathIndexLocked()
{
    if (!this->pathIndexDirty)
    {
        return;
    }
    this->pathIndexDirty = false;
    this->pathIndex.clear();

    std::map<std::pair<uint8_t, uint16_t>, DdmtNode> nodes;
    std::map<std::pair<uint8_t, uint16_t>, DdmtFile> files;
    for (const auto& [_, ddmt] : this->ddmtSections)
    {
        nodes.insert(ddmt.nodes.begin(), ddmt.nodes.end());
        files.insert(ddmt.files.begin(), ddmt.files.end());
    }
    std::map<std::pair<uint16_t, uint32_t>, DamtMpu> mpus;
    for (const auto& [_, damt] : this->damtSections)
    {
        mpus.insert(damt.begin(), damt.end());
    }

    auto directoryOf = [&](uint8_t session, uint16_t nodeTag) -> std::optional<std::string> {
        auto node = nodes.find({ session, nodeTag });
        if (node == nodes.end())
        {
            return std::nullopt;
        }
        return JoinPath({ node->second.base, node->second.path });
    };

    for (const auto& [mpuKey, mpu] : mpus)
    {
        const auto [componentTag, mpuSequenceNumber] = mpuKey;
        if (!mpu.indexItemFlag)
        {
            for (const auto& [nodeTag, itemId] : mpu.items)
            {
                auto file = files.find({ mpu.sessionId, nodeTag });
                if (file == files.end())
                {
                    continue;
                }
                auto directory = directoryOf(mpu.sessionId, file->second.directoryNodeTag);
                if (!directory)
                {
                    continue;
                }
                const auto path = JoinPath({ *directory, file->second.fileName });
                this->pathIndex[path] = PathEntry{ { componentTag, mpuSequenceNumber, itemId }, GuessMimeType(path), false, 0 };
            }
            continue;
        }

        if (!mpu.nodeTag)
        {
            continue;
        }
        auto directory = directoryOf(mpu.sessionId, *mpu.nodeTag);
        if (!directory)
        {
            continue;
        }

        // Without index_item_id, any item of the MPU that parses as an index.
        std::map<uint32_t, IndexEntry> entries;
        bool parsed = false;
        for (auto it = this->items.lower_bound({ componentTag, mpuSequenceNumber, 0 });
            !parsed && it != this->items.end() && std::get<0>(it->first) == componentTag && std::get<1>(it->first) == mpuSequenceNumber; ++it)
        {
            if (mpu.indexItemId && std::get<2>(it->first) != *mpu.indexItemId)
            {
                continue;
            }
            // index_item_compression_flag=3 has been seen on plain index items,
            // so try the bytes as they are before inflating.
            parsed = ParseIndexItem(it->second, entries);
            if (!parsed)
            {
                std::vector<uint8_t> inflated;
                parsed = zlibInflate(it->second, inflated, kMaxIndexItemSize) && ParseIndexItem(inflated, entries);
            }
        }
        for (const auto& [itemId, entry] : entries)
        {
            const auto path = JoinPath({ *directory, entry.fileName });
            this->pathIndex[path] = PathEntry{
                { componentTag, mpuSequenceNumber, itemId },
                entry.itemType.empty() ? GuessMimeType(path) : entry.itemType,
                entry.compressionType == kCompressionZlib,
                entry.originalSize,
            };
        }
    }

    // Items of MPUs the DAMT no longer lists are never served again.
    if (!mpus.empty())
    {
        for (auto it = this->items.begin(); it != this->items.end();)
        {
            if (mpus.contains({ std::get<0>(it->first), std::get<1>(it->first) }))
            {
                ++it;
                continue;
            }
            this->itemBytes -= it->second.size();
            it = this->items.erase(it);
        }
    }
    std::erase_if(this->fileCache, [&](const auto& entry) { return !this->pathIndex.contains(entry.first); });
}

std::optional<AppReceiver::Application> AppReceiver::GetApplication() const
{
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->application;
}

std::optional<AppReceiver::File> AppReceiver::GetFile(const std::string& path)
{
    std::lock_guard<std::mutex> lock(this->mutex);
    this->RebuildPathIndexLocked();
    auto entry = this->pathIndex.find(JoinPath({ path }));
    if (entry == this->pathIndex.end())
    {
        return std::nullopt;
    }
    auto item = this->items.find(entry->second.key);
    if (item == this->items.end())
    {
        return std::nullopt;
    }

    auto cached = this->fileCache.find(entry->first);
    if (cached != this->fileCache.end() && cached->second.first == item->second)
    {
        return cached->second.second;
    }

    File file{ entry->second.mimeType, {} };
    if (entry->second.zlib)
    {
        const size_t limit = entry->second.originalSize ? std::min<size_t>(entry->second.originalSize, kMaxFileSize) : kMaxFileSize;
        if (!zlibInflate(item->second, file.data, limit))
        {
            return std::nullopt;
        }
    }
    else
    {
        file.data = item->second;
    }
    this->fileCache[entry->first] = { item->second, file };
    return file;
}

std::vector<std::string> AppReceiver::ListFiles()
{
    std::lock_guard<std::mutex> lock(this->mutex);
    this->RebuildPathIndexLocked();
    std::vector<std::string> paths;
    for (const auto& [path, entry] : this->pathIndex)
    {
        if (this->items.contains(entry.key))
        {
            paths.push_back(path);
        }
    }
    return paths;
}

}
