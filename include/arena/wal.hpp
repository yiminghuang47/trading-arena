// wal.hpp -- write-ahead log + replay support.
//
// Records every inbound (owner, order) event to a binary file BEFORE it is
// matched. Re-feeding the log through a fresh engine reproduces the exact same
// outbound stream -- a persisted determinism proof and a debugging tool. The
// header carries the game seed and revealed value V (so settlement can be
// reconstructed) plus a digest of the live outbound stream, which replay
// recomputes and checks for byte-identical equality.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "arena/protocol.hpp"
#include "arena/types.hpp"

namespace arena{
    // FNV-1a 64-bit rolling digest over the outbound message bytes.
    struct Digest{
        std::uint64_t h = 0xcbf29ce484222325ULL;
        void update(const void* p, std::size_t n) {
            const auto* b = static_cast<const unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ULL; }
        }
        template <class T>
        void update(const T& v) { update(&v, sizeof(T)); }
    };

    inline constexpr std::uint32_t kWalVersion = 1;

    #pragma pack(push, 1)
    struct WalHeader{
        char          magic[4];      // "AWAL"
        std::uint32_t version;
        std::uint64_t seed;          // game seed
        Price         value;         // revealed terminal value V
        std::uint16_t n_bots;        // participant count
        std::uint64_t record_count;  // patched on finish()
        std::uint64_t out_digest;    // patched on finish(): digest of outbound stream
    };
    struct WalRecord{
        BotId              owner;
        wire::InboundOrder msg;
    };
    #pragma pack(pop)

    static_assert(sizeof(WalHeader) == 38, "WalHeader layout");
    static_assert(sizeof(WalRecord) == sizeof(BotId) + sizeof(wire::InboundOrder),
                  "WalRecord must be tightly packed");

    // Append-only writer. The header is written up front and patched on finish().
    class WalWriter{
        public:
        WalWriter(const std::string& path, std::uint64_t seed, Price value,
                  std::uint16_t n_bots) {
            f_ = std::fopen(path.c_str(), "wb+");
            if (!f_) { std::perror("wal: open for write"); std::exit(1); }
            h_ = WalHeader{{'A', 'W', 'A', 'L'}, kWalVersion, seed, value, n_bots, 0, 0};
            std::fwrite(&h_, sizeof(h_), 1, f_);
        }
        ~WalWriter() { if (f_) std::fclose(f_); }
        WalWriter(const WalWriter&) = delete;
        WalWriter& operator=(const WalWriter&) = delete;

        void append(BotId owner, const wire::InboundOrder& msg) {
            WalRecord r{owner, msg};
            std::fwrite(&r, sizeof(r), 1, f_);
            ++h_.record_count;
        }

        // Patch the header with the final record count + outbound digest, then close.
        void finish(std::uint64_t out_digest) {
            h_.out_digest = out_digest;
            std::fseek(f_, 0, SEEK_SET);
            std::fwrite(&h_, sizeof(h_), 1, f_);
            std::fclose(f_);
            f_ = nullptr;
        }

        std::uint64_t record_count() const { return h_.record_count; }

        private:
        std::FILE* f_ = nullptr;
        WalHeader  h_{};
    };

    // Reads the whole log into memory (validates magic + version).
    class WalReader{
        public:
        WalHeader              header{};
        std::vector<WalRecord> records;

        explicit WalReader(const std::string& path) {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) { std::perror("wal: open for read"); std::exit(1); }
            if (std::fread(&header, sizeof(header), 1, f) != 1)
                die(f, "short read on header");
            if (header.magic[0] != 'A' || header.magic[1] != 'W' ||
                header.magic[2] != 'A' || header.magic[3] != 'L')
                die(f, "bad magic");
            if (header.version != kWalVersion)
                die(f, "unsupported version");
            records.resize(header.record_count);
            if (header.record_count &&
                std::fread(records.data(), sizeof(WalRecord), header.record_count, f) !=
                    header.record_count)
                die(f, "short read on records");
            std::fclose(f);
        }

        private:
        [[noreturn]] static void die(std::FILE* f, const char* what) {
            std::fprintf(stderr, "wal: %s\n", what);
            std::fclose(f);
            std::exit(1);
        }
    };
}  // namespace arena
