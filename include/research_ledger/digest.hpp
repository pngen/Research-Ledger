#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace research_ledger {

// SHA-256 digest, implemented in this repository so that integrity checks,
// record chaining and replay digests do not depend on an external library.
class Digest {
public:
    static constexpr std::size_t kBytes = 32;

    constexpr Digest() noexcept = default;
    explicit Digest(std::array<std::uint8_t, kBytes> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }
    [[nodiscard]] bool is_zero() const noexcept;

    friend bool operator==(const Digest& lhs, const Digest& rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }
    friend bool operator!=(const Digest& lhs, const Digest& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const Digest& lhs, const Digest& rhs) noexcept {
        return lhs.bytes_ < rhs.bytes_;
    }

private:
    std::array<std::uint8_t, kBytes> bytes_{};
};

// Streaming SHA-256. The finalization is idempotent: calling finish() twice
// returns the same digest and never resumes updating.
class DigestBuilder {
public:
    DigestBuilder() noexcept;

    void update(std::span<const std::byte> bytes) noexcept;
    void update(std::string_view text) noexcept;
    void update_u8(std::uint8_t value) noexcept;
    void update_u16(std::uint16_t value) noexcept;
    void update_u32(std::uint32_t value) noexcept;
    void update_u64(std::uint64_t value) noexcept;
    void update_i64(std::int64_t value) noexcept;
    void update_bytes(const Digest& digest) noexcept;

    [[nodiscard]] Digest finish() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool finished_ = false;
    Digest result_{};
};

Digest sha256(std::span<const std::byte> bytes) noexcept;
Digest sha256(std::string_view text) noexcept;

// 64 lowercase hex characters, or an empty string for a zero digest.
std::string to_hex(const Digest& digest);
std::optional<Digest> digest_from_hex(std::string_view text);

// Chained digest used by the ledger: digest(previous_chain || new_payload).
Digest chain_digest(const Digest& previous, const Digest& next);

}  // namespace research_ledger
