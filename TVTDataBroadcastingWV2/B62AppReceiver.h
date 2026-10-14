#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace B62
{

// Rebuilds 4K data broadcasting (ARIB STD-B62) applications from the private
// sections that BonDriver_dantto4k / dantto4k put into the TS: an ES with
// stream_type 0x05 and registration descriptor 'D4KA' carrying MH-AIT, DDMT,
// DAMT, DCCT and EMT sections unchanged, plus items in table_id 0xC0 sections.
// See dantto4k docs/app-data-ts-transport.md for the format.
//
// FeedPacket is called on the TS thread; the other methods may be called from
// any thread.
class AppReceiver
{
public:
    struct File
    {
        std::string mimeType;
        std::vector<uint8_t> data;
    };

    struct Application
    {
        // Path of the first page, relative to the application root.
        std::string entryPath;
        uint32_t organizationId = 0;
        uint16_t applicationId = 0;
        uint8_t controlCode = 0;
        uint8_t aitVersion = 0;

        bool operator==(const Application&) const = default;
    };

    // A caption document (ARIB-TTML) or resource for applications that show
    // captions themselves (addCaptionListener).
    struct Caption
    {
        uint16_t componentTag = 0;
        uint8_t dataType = 0;
        uint8_t tmd = 0;
        // When the document's time zero is presented (90 kHz, TS timeline).
        std::optional<uint64_t> pts;
        // What ARIB TR-B39 8.10.1.1 hands to CaptionListener besides the data
        std::string languageCode;
        uint8_t resolution = 0;
        uint32_t referenceStartTimeSeconds = 0;
        uint32_t referenceStartTimeFraction = 0;
        uint8_t subtitleSequenceNumber = 0;
        uint8_t subsampleNumber = 0;
        uint8_t lastSubsampleNumber = 0;
        std::vector<uint8_t> data;
    };

    // An event message (ARIB STD-B60 11.2.2.2) from an EMT. Only new messages
    // and new versions of a message are reported.
    struct EventMessage
    {
        uint8_t emtTag = 0;
        uint16_t messageGroupId = 0;
        uint8_t timeMode = 0;
        // event_msg_UTC / NPT / relativeTime as transmitted (NTP format for UTC and relative)
        uint64_t time = 0;
        uint8_t messageType = 0;
        uint16_t messageId = 0;
        std::vector<uint8_t> privateData;
    };

    static constexpr uint32_t kFormatIdentifier = 0x44344B41; // 'D4KA'

    // Called on the TS thread when the application to run changes.
    void SetApplicationChangedCallback(std::function<void()> callback);
    // Forgets everything and follows the given service (-1: the first one).
    void Reset(int serviceId);
    void FeedPacket(const uint8_t* packet);
    // Called on the TS thread when captions or event messages are waiting in
    // TakeCaptions() / TakeEventMessages().
    void SetMessageCallback(std::function<void()> callback);
    std::vector<Caption> TakeCaptions();
    std::vector<EventMessage> TakeEventMessages();

    std::optional<Application> GetApplication() const;
    // path is relative to the application root, e.g. "sh4/40/001/startup/html/index.html".
    std::optional<File> GetFile(const std::string& path);
    std::vector<std::string> ListFiles();

private:
    struct SectionBuffer
    {
        std::vector<uint8_t> data;
        int continuityCounter = -1;
        bool synced = false;
    };

    struct DamtMpu
    {
        uint8_t sessionId = 0;
        bool indexItemFlag = false;
        std::optional<uint32_t> indexItemId;
        std::optional<uint16_t> nodeTag;
        // node_tag, item_id
        std::vector<std::pair<uint16_t, uint32_t>> items;
    };

    struct DdmtNode
    {
        std::string base;
        std::string path;
    };

    struct DdmtFile
    {
        uint16_t directoryNodeTag = 0;
        std::string fileName;
    };

    struct DdmtSection
    {
        // (session, node_tag)
        std::map<std::pair<uint8_t, uint16_t>, DdmtNode> nodes;
        std::map<std::pair<uint8_t, uint16_t>, DdmtFile> files;
    };

    // component_tag, mpu_sequence_number, item_id
    using ItemKey = std::tuple<uint16_t, uint32_t, uint32_t>;
    // table_id, table_id_extension, section_number
    using SectionKey = std::tuple<uint8_t, uint16_t, uint8_t>;

    struct PathEntry
    {
        ItemKey key;
        std::string mimeType;
        bool zlib = false;
        uint32_t originalSize = 0;
    };

    struct PendingItem
    {
        uint8_t lastSectionNumber = 0;
        std::map<uint8_t, std::vector<uint8_t>> parts;
    };

    void ResetLocked();
    void ResetApplicationLocked();
    void FeedSectionData(uint16_t pid, const uint8_t* packet, SectionBuffer& buffer);
    void OnSection(uint16_t pid, const uint8_t* section, size_t size);
    void OnPat(const uint8_t* section, size_t size);
    void OnPmt(const uint8_t* section, size_t size);
    void OnAppSection(const uint8_t* section, size_t size);
    void OnMhAit(const uint8_t* section, size_t size);
    void OnDdmt(const SectionKey& key, const uint8_t* section, size_t size);
    void OnDamt(const SectionKey& key, const uint8_t* section, size_t size);
    std::optional<std::vector<uint8_t>> AssembleSections(const uint8_t* section, size_t size);
    void OnItem(const std::vector<uint8_t>& body);
    void OnCaption(const std::vector<uint8_t>& body);
    void OnTaggedEmt(const std::vector<uint8_t>& body);
    void RebuildPathIndexLocked();
    bool IsServableLocked(const std::string& path);
    void UpdatePublishedApplicationLocked();

    mutable std::mutex mutex;
    std::function<void()> applicationChangedCallback;
    std::function<void()> messageCallback;
    int serviceId = -1;

    int pmtPid = -1;
    int appPid = -1;
    std::unordered_map<uint16_t, SectionBuffer> sectionBuffers;

    // CRC_32 of the last section seen per key, to skip unchanged repeats.
    std::map<SectionKey, uint32_t> sectionCrcs;
    std::map<SectionKey, std::map<std::pair<uint16_t, uint32_t>, DamtMpu>> damtSections;
    std::map<SectionKey, DdmtSection> ddmtSections;
    // (table_id << 16) | table_id_extension
    std::map<uint32_t, PendingItem> pendingItems;
    std::vector<Caption> captions;
    std::vector<EventMessage> eventMessages;
    // (EMT_tag, message_group_id, message_id upper 8 bits) -> last message_version and private data
    std::map<std::tuple<uint8_t, uint16_t, uint8_t>, std::pair<uint8_t, std::vector<uint8_t>>> lastEventMessages;
    std::map<ItemKey, std::vector<uint8_t>> items;
    size_t itemBytes = 0;

    // The AUTOSTART application of the latest MH-AIT.
    std::optional<Application> application;
    // What GetApplication() reports: application, once its first page can be
    // served. The MH-AIT arrives long before the files do.
    std::optional<Application> publishedApplication;

    bool pathIndexDirty = true;
    std::map<std::string, PathEntry> pathIndex;
    // path -> (raw item size, decoded file)
    std::unordered_map<std::string, std::pair<std::vector<uint8_t>, File>> fileCache;
};

}
