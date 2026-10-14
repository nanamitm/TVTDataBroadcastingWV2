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
constexpr uint8_t kTableCaption = 0xC1;
constexpr uint8_t kTableTaggedEmt = 0xC2;
constexpr uint8_t kTableEmt = 0xA6;
constexpr uint16_t kDescriptorEventMessage = 0xF003;
constexpr size_t kMaxQueuedCaptions = 64;
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
        hadApplication = this->publishedApplication.has_value() || this->IsApplicationLoadingLocked();
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
    this->captions.clear();
    this->eventMessages.clear();
    this->lastEventMessages.clear();
    this->items.clear();
    this->itemBytes = 0;
    this->application.reset();
    this->publishedApplication.reset();
    this->pathIndexDirty = true;
    this->filesGeneration++;
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
    bool loadingBefore;
    std::function<void()> callback;
    std::function<void()> captionNotify;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (pid != 0 && pid != this->pmtPid && pid != this->appPid)
        {
            return;
        }
        before = this->publishedApplication;
        loadingBefore = this->IsApplicationLoadingLocked();
        const uint64_t filesGenerationBefore = this->filesGeneration;
        const size_t captionCount = this->captions.size();
        const size_t eventMessageCount = this->eventMessages.size();
        this->FeedSectionData(pid, packet, this->sectionBuffers[pid]);
        this->UpdatePublishedApplicationLocked();
        if (before != this->publishedApplication || loadingBefore != this->IsApplicationLoadingLocked())
        {
            callback = this->applicationChangedCallback;
        }
        if ((this->captions.size() != captionCount && !this->captions.empty()) ||
            (this->eventMessages.size() != eventMessageCount && !this->eventMessages.empty()) ||
            this->filesGeneration != filesGenerationBefore)
        {
            captionNotify = this->messageCallback;
        }
    }
    if (callback)
    {
        callback();
    }
    if (captionNotify)
    {
        captionNotify();
    }
}

void AppReceiver::SetMessageCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(this->mutex);
    this->messageCallback = std::move(callback);
}

std::vector<AppReceiver::EventMessage> AppReceiver::TakeEventMessages()
{
    std::lock_guard<std::mutex> lock(this->mutex);
    return std::exchange(this->eventMessages, {});
}

std::vector<AppReceiver::Caption> AppReceiver::TakeCaptions()
{
    std::lock_guard<std::mutex> lock(this->mutex);
    return std::exchange(this->captions, {});
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
    if (tableId == kTableItem || tableId == kTableCaption || tableId == kTableTaggedEmt)
    {
        auto body = this->AssembleSections(section, size);
        if (body && tableId == kTableItem)
        {
            this->OnItem(*body);
        }
        else if (body && tableId == kTableCaption)
        {
            this->OnCaption(*body);
        }
        else if (body)
        {
            this->OnTaggedEmt(*body);
        }
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

// "http://host[:port]/path/" -> ("http://host[:port]", "path/"). Only http and
// https URLs whose host is made of URL-safe characters are accepted.
static std::optional<std::pair<std::string, std::string>> SplitHttpUrl(const std::string& url)
{
    size_t schemeEnd;
    if (url.rfind("http://", 0) == 0)
    {
        schemeEnd = 7;
    }
    else if (url.rfind("https://", 0) == 0)
    {
        schemeEnd = 8;
    }
    else
    {
        return std::nullopt;
    }
    const size_t pathStart = std::min(url.find('/', schemeEnd), url.size());
    const std::string authority = url.substr(schemeEnd, pathStart - schemeEnd);
    if (authority.empty() || authority.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.:") != std::string::npos)
    {
        return std::nullopt;
    }
    std::string path = pathStart < url.size() ? url.substr(pathStart + 1) : std::string();
    if (path.find_first_of("?#") != std::string::npos)
    {
        return std::nullopt;
    }
    return std::make_pair(url.substr(0, pathStart), path);
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
        std::string networkOrigin;
        bool hasTransport = false;
        std::string initialPath;
        while (descriptors.Left() >= 3)
        {
            const uint16_t tag = descriptors.U16();
            Reader body = descriptors.Sub(descriptors.U8());
            if (tag == kDescriptorTransportProtocol && !hasTransport)
            {
                const uint16_t protocolId = body.U16();
                body.U8(); // transport_protocol_label
                if (protocolId == 0x0005)
                {
                    // Broadcast: URL_base length and bytes, e.g. "sh4/40/001/".
                    base = body.String(body.U8());
                    hasTransport = true;
                }
                else if (protocolId == 0x0003)
                {
                    // HTTP: URL_base length and bytes, e.g. "http://example.jp/app/"
                    // (URL extensions follow and are not used).
                    if (auto url = SplitHttpUrl(body.String(body.U8())))
                    {
                        networkOrigin = url->first;
                        base = url->second;
                        hasTransport = true;
                    }
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
        app.networkOrigin = networkOrigin;
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
    this->filesGeneration++;
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
    this->filesGeneration++;
}

std::optional<std::vector<uint8_t>> AppReceiver::AssembleSections(const uint8_t* section, size_t size)
{
    // Items and captions number their sends separately.
    const uint32_t sequence = (section[0] << 16) | (section[3] << 8) | section[4];
    const uint8_t number = section[6];
    const uint8_t last = section[7];
    if (number > last)
    {
        return std::nullopt;
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
        return std::nullopt;
    }

    std::vector<uint8_t> body;
    for (auto& [_, part] : it->second.parts)
    {
        body.insert(body.end(), part.begin(), part.end());
    }
    this->pendingItems.erase(it);
    return body;
}

void AppReceiver::OnCaption(const std::vector<uint8_t>& body)
{
    // component_tag, data_type, tmd, pts_flag, pts (40 bits), data_size,
    // then a length-prefixed extension: language, resolution, reference start
    // time and the MFU header numbers.
    if (body.size() < 15)
    {
        return;
    }
    const uint32_t dataSize = (body[10] << 24) | (body[11] << 16) | (body[12] << 8) | body[13];
    const size_t extensionSize = body[14];
    const size_t headerSize = 15 + extensionSize;
    if (body.size() < headerSize || dataSize != body.size() - headerSize)
    {
        return;
    }
    Caption caption;
    caption.componentTag = (body[0] << 8) | body[1];
    caption.dataType = body[2];
    caption.tmd = body[3];
    if (body[4] & 0x80)
    {
        caption.pts = ((static_cast<uint64_t>(body[5]) << 32) | (body[6] << 24) | (body[7] << 16) | (body[8] << 8) | body[9]) & 0x1FFFFFFFFULL;
    }
    if (extensionSize >= 15)
    {
        const uint8_t* extension = body.data() + 15;
        caption.languageCode.assign(reinterpret_cast<const char*>(extension), 3);
        caption.resolution = extension[3];
        caption.referenceStartTimeSeconds = (extension[4] << 24) | (extension[5] << 16) | (extension[6] << 8) | extension[7];
        caption.referenceStartTimeFraction = (extension[8] << 24) | (extension[9] << 16) | (extension[10] << 8) | extension[11];
        caption.subtitleSequenceNumber = extension[12];
        caption.subsampleNumber = extension[13];
        caption.lastSubsampleNumber = extension[14];
    }
    caption.data.assign(body.begin() + headerSize, body.end());
    if (this->captions.size() >= kMaxQueuedCaptions)
    {
        this->captions.erase(this->captions.begin());
    }
    this->captions.push_back(std::move(caption));
}

void AppReceiver::OnTaggedEmt(const std::vector<uint8_t>& body)
{
    // EMT_tag, then the EMT section (ARIB STD-B60 11.2.1.1)
    if (body.size() < 1 + 12)
    {
        return;
    }
    const uint8_t emtTag = body[0];
    const uint8_t* emt = body.data() + 1;
    const size_t emtSize = body.size() - 1;
    if (emt[0] != kTableEmt || 3 + (((emt[1] & 0x0F) << 8) | emt[2]) != emtSize || Crc32Mpeg2(emt, emtSize) != 0)
    {
        return;
    }
    Reader descriptors(emt + 8, emtSize - 12);
    while (descriptors.Left() >= 3 && !descriptors.Failed())
    {
        const uint16_t tag = descriptors.U16();
        // Descriptors from 0xF000 have a 16-bit length.
        const size_t length = tag >= 0xF000 ? descriptors.U16() : descriptors.U8();
        Reader body2 = descriptors.Sub(length);
        if (tag != kDescriptorEventMessage)
        {
            continue;
        }
        EventMessage message;
        message.emtTag = emtTag;
        message.messageGroupId = body2.U16() >> 4;
        message.timeMode = body2.U8();
        // Every defined time_mode carries a 64-bit time (reserved for 0x00).
        if (message.timeMode > 0x05 || message.timeMode == 0x04)
        {
            continue;
        }
        message.time = (static_cast<uint64_t>(body2.U32()) << 32) | body2.U32();
        message.messageType = body2.U8();
        message.messageId = body2.U16();
        if (body2.Failed())
        {
            continue;
        }
        message.privateData.assign(body2.Current(), body2.Current() + body2.Left());

        // A listener runs again only for a new message_version (ARIB STD-B62 Volume 2 3.3.12.1);
        // EMTs repeat the same message every second.
        const auto key = std::make_tuple(emtTag, message.messageGroupId, static_cast<uint8_t>(message.messageId >> 8));
        const uint8_t version = message.messageId & 0xFF;
        auto last = this->lastEventMessages.find(key);
        if (last != this->lastEventMessages.end() && last->second.first == version && last->second.second == message.privateData)
        {
            continue;
        }
        this->lastEventMessages[key] = { version, message.privateData };
        if (this->eventMessages.size() >= kMaxQueuedCaptions)
        {
            this->eventMessages.erase(this->eventMessages.begin());
        }
        this->eventMessages.push_back(std::move(message));
    }
}

void AppReceiver::OnItem(const std::vector<uint8_t>& body)
{
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
    this->filesGeneration++;

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

bool AppReceiver::IsServableLocked(const std::string& path)
{
    this->RebuildPathIndexLocked();
    auto entry = this->pathIndex.find(JoinPath({ path }));
    return entry != this->pathIndex.end() && this->items.contains(entry->second.key);
}

void AppReceiver::UpdatePublishedApplicationLocked()
{
    if (this->publishedApplication == this->application)
    {
        return;
    }
    if (!this->application)
    {
        this->publishedApplication.reset();
        return;
    }
    // Files of a network application are fetched when the page asks for them.
    if (!this->application->networkOrigin.empty())
    {
        this->publishedApplication = this->application;
        return;
    }
    // Only checked while something is pending, and the path index is rebuilt
    // only after a table or item actually changed. Waits for every file the
    // tables list as well, so the first page does not load without its scripts.
    if (!this->IsServableLocked(this->application->entryPath))
    {
        return;
    }
    for (const auto& [_, entry] : this->pathIndex)
    {
        if (!this->items.contains(entry.key))
        {
            return;
        }
    }
    this->publishedApplication = this->application;
}

std::optional<AppReceiver::Application> AppReceiver::GetApplication() const
{
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->publishedApplication;
}

bool AppReceiver::IsApplicationLoadingLocked() const
{
    return this->application.has_value() && this->publishedApplication != this->application;
}

bool AppReceiver::IsApplicationLoading() const
{
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->IsApplicationLoadingLocked();
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

std::vector<std::pair<std::string, std::string>> AppReceiver::ListFileVersions()
{
    std::lock_guard<std::mutex> lock(this->mutex);
    this->RebuildPathIndexLocked();
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& [path, entry] : this->pathIndex)
    {
        if (this->items.contains(entry.key))
        {
            const auto& [componentTag, mpu, itemId] = entry.key;
            files.emplace_back(path, std::to_string(componentTag) + "-" + std::to_string(mpu) + "-" + std::to_string(itemId));
        }
    }
    return files;
}

uint64_t AppReceiver::GetFilesGeneration() const
{
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->filesGeneration;
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
