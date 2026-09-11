#include "research_ledger/digest.hpp"

#include <algorithm>
#include <cstring>

namespace research_ledger {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
}};

constexpr std::array<std::uint32_t, 8> kInitialState{{
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu,
    0x5be0cd19u,
}};

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
    return (value >> bits) | (value << (32u - bits));
}

}  // namespace

bool Digest::is_zero() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

DigestBuilder::DigestBuilder() noexcept : state_(kInitialState) {}

void DigestBuilder::compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
        schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                          (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                          (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                          static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                                 (schedule[index - 15] >> 3);
        const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                                 (schedule[index - 2] >> 10);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < 64; ++index) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[index] + schedule[index];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void DigestBuilder::update(std::span<const std::byte> bytes) noexcept {
    if (finished_) {
        return;
    }
    total_bytes_ += bytes.size();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t take = std::min(bytes.size() - offset, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes.data() + offset, take);
        buffered_ += take;
        offset += take;
        if (buffered_ == buffer_.size()) {
            compress(buffer_.data());
            buffered_ = 0;
        }
    }
}

void DigestBuilder::update(std::string_view text) noexcept {
    update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void DigestBuilder::update_u8(std::uint8_t value) noexcept {
    const std::byte byte{value};
    update(std::span<const std::byte>(&byte, 1));
}

void DigestBuilder::update_u16(std::uint16_t value) noexcept {
    const std::byte bytes[2] = {std::byte(value & 0xffu), std::byte((value >> 8) & 0xffu)};
    update(std::span<const std::byte>(bytes, 2));
}

void DigestBuilder::update_u32(std::uint32_t value) noexcept {
    const std::byte bytes[4] = {std::byte(value & 0xffu), std::byte((value >> 8) & 0xffu),
                                std::byte((value >> 16) & 0xffu), std::byte((value >> 24) & 0xffu)};
    update(std::span<const std::byte>(bytes, 4));
}

void DigestBuilder::update_u64(std::uint64_t value) noexcept {
    const std::byte bytes[8] = {std::byte(value & 0xffu),         std::byte((value >> 8) & 0xffu),
                                std::byte((value >> 16) & 0xffu), std::byte((value >> 24) & 0xffu),
                                std::byte((value >> 32) & 0xffu), std::byte((value >> 40) & 0xffu),
                                std::byte((value >> 48) & 0xffu), std::byte((value >> 56) & 0xffu)};
    update(std::span<const std::byte>(bytes, 8));
}

void DigestBuilder::update_i64(std::int64_t value) noexcept {
    update_u64(static_cast<std::uint64_t>(value));
}

void DigestBuilder::update_bytes(const Digest& digest) noexcept {
    update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(digest.data()),
                                     Digest::kBytes));
}

Digest DigestBuilder::finish() noexcept {
    if (finished_) {
        return result_;
    }
    const std::uint64_t total_bits = total_bytes_ * 8ull;
    const std::byte padding{0x80};
    update(std::span<const std::byte>(&padding, 1));
    const std::byte zero{0x00};
    while (buffered_ != 56) {
        update(std::span<const std::byte>(&zero, 1));
    }
    std::byte length_bytes[8];
    for (std::size_t index = 0; index < 8; ++index) {
        length_bytes[index] = std::byte((total_bits >> (56u - index * 8u)) & 0xffull);
    }
    update(std::span<const std::byte>(length_bytes, 8));

    std::array<std::uint8_t, Digest::kBytes> bytes{};
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xffu);
        bytes[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xffu);
        bytes[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xffu);
        bytes[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xffu);
    }
    finished_ = true;
    result_ = Digest(bytes);
    return result_;
}

Digest sha256(std::span<const std::byte> bytes) noexcept {
    DigestBuilder builder;
    builder.update(bytes);
    return builder.finish();
}

Digest sha256(std::string_view text) noexcept {
    DigestBuilder builder;
    builder.update(text);
    return builder.finish();
}

std::string to_hex(const Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(Digest::kBytes * 2);
    for (const std::uint8_t byte : digest.bytes()) {
        text.push_back(kHex[(byte >> 4) & 0x0fu]);
        text.push_back(kHex[byte & 0x0fu]);
    }
    return text;
}

std::optional<Digest> digest_from_hex(std::string_view text) {
    if (text.size() != Digest::kBytes * 2) {
        return std::nullopt;
    }
    std::array<std::uint8_t, Digest::kBytes> bytes{};
    auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < Digest::kBytes; ++index) {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return Digest(bytes);
}

Digest chain_digest(const Digest& previous, const Digest& next) {
    DigestBuilder builder;
    builder.update_bytes(previous);
    builder.update_bytes(next);
    return builder.finish();
}

}  // namespace research_ledger
