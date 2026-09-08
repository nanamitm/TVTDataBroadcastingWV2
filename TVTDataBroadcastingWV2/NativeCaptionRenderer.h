#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "thirdparty/json.hpp"

class NativeCaptionRenderer
{
public:
    using MessageCallback = std::function<void(const nlohmann::json&)>;

    explicit NativeCaptionRenderer(MessageCallback callback);
    ~NativeCaptionRenderer();

    NativeCaptionRenderer(const NativeCaptionRenderer&) = delete;
    NativeCaptionRenderer& operator=(const NativeCaptionRenderer&) = delete;

    bool Initialize();
    void Reset();
    void Push(int streamId, const std::vector<std::uint8_t>& data, std::optional<std::int64_t> pts90kHz);
    void Update(std::int64_t currentTimeMs, int frameWidth, int frameHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
