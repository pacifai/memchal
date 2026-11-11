#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <unistd.h>
#include <vector>
#include "record/verifier_stream.hpp"
#include "shared/chacha20.hpp"
#include "shared/protocol.hpp"
#include "shared/random.hpp"
#include "shared/socket.hpp"
#include "verifier/args.hpp"
#include "verifier/reference.hpp"

namespace mc {

// 64-bit values from ChaCha20 keystream under a fresh random key.
class KeyStream {
public:
    KeyStream() { random_bytes(key_, sizeof key_); }

    uint64_t next() {
        static const uint8_t zero_nonce[12] = {};
        if (used_ == 64) { chacha20_block(key_, zero_nonce, counter_++, block_); used_ = 0; }
        uint64_t v = 0;
        for (int b = 0; b < 8; ++b) v |= (uint64_t)block_[used_ + b] << (8 * b);
        used_ += 8;
        return v;
    }

private:
    uint8_t  key_[32];
    uint8_t  block_[64];
    uint32_t counter_ = 0;
    unsigned used_    = 64;
};

// Sends the hello and then the buffer or its recipe, and waits for the
// prover's ready message.
//   upload_s: time spent sending the buffer or recipe.
static inline bool hand_over(int fd, const Reference& ref, bool build, Ready* ready,
                             double* upload_s, char* err, size_t errlen) {
    Hello hello{ kMagic, (uint64_t)ref.bytes, kWireVersion, build ? kHelloBuild : 0u };
    if (!send_all(fd, &hello, sizeof hello)) { snprintf(err, errlen, "hello send failed"); return false; }
    const auto t0 = std::chrono::steady_clock::now();
    if (build) {
        RecipeMsg msg = msg_from_recipe(ref.recipe);
        if (!send_all(fd, &msg, sizeof msg)) { snprintf(err, errlen, "recipe send failed"); return false; }
    } else if (!send_all(fd, ref.words, ref.bytes)) {
        snprintf(err, errlen, "buffer send failed");
        return false;
    }
    *upload_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!recv_all(fd, ready, sizeof *ready) || ready->magic != kMagic) {
        snprintf(err, errlen, "no valid ready message");
        return false;
    }
    return true;
}

// Answers checked and answers found wrong.
// The longest pause between challenges, as a multiple of --gap-us.
static constexpr double kMaxGapFactor = 20;

struct Tally {
    uint64_t checked = 0;
    uint64_t errors  = 0;
};

// Issues challenges over one connection and checks the answers.
class Session {
public:
    Session(int fd, const Args& a, const Reference& ref, record::VerifierStream* stream)
        : fd_(fd), a_(a), ref_(ref), stream_(stream) {
        std::mt19937_64 rng(a.seed ^ 0x5555u);
        next_nonce_ = rng();
    }

    // Runs `count` challenges and adds the checks to `tally`. Returns false if the
    // connection drops.
    //   phase: "w" for warmup or "r" for measured, as the session file labels them.
    //   latencies_ns: when set, receives each round trip.
    bool run(uint32_t count, const char* phase, std::vector<double>* latencies_ns, Tally& tally) {
        std::vector<uint64_t> scratch;
        for (uint32_t i = 0; i < count; ++i) {
            Challenge ch{};
            ch.samples = a_.samples;
            ch.lanes   = a_.lanes;
            if (a_.chained) ch.flags |= kChallengeChained;
            if (a_.tree)    ch.flags |= kChallengeTreeFold;
            ch.nonce = a_.random_nonces ? nonces_.next() : next_nonce_++;

            const uint64_t seq = seq_++;
            const bool map = stream_ && stream_->wants_map(seq);
            const bool check = map || a_.verify_every <= 1 || checks_.next() % a_.verify_every == 0;

            const auto t0 = std::chrono::steady_clock::now();
            if (!send_all(fd_, &ch, sizeof ch)) return false;
            Response r{};
            if (!recv_all(fd_, &r, sizeof r)) return false;
            const double ns = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - t0).count();

            // The expected answer is computed only after the response arrives. Computing
            // it before sending would delay checked challenges, and the prover could
            // time that delay to learn which answers go unchecked.
            uint64_t expect = 0;
            if (map)        expect = answer(stream_->recorder(ref_.words), ref_.num_words, ch, scratch);
            else if (check) expect = answer(BufferLoad{ref_.words}, ref_.num_words, ch, scratch);
            const bool ok = !check || r.hash == expect;
            if (check) ++tally.checked;
            if (!ok) ++tally.errors;
            if (latencies_ns) latencies_ns->push_back(ns);
            if (stream_) stream_->challenge(seq, phase, ns / 1000.0, ch.nonce, r.hash, expect, ok,
                                            check, map);
            pause();
        }
        return true;
    }

    void terminate() {
        Challenge stop{};
        stop.flags = kChallengeTerminate;
        send_all(fd_, &stop, sizeof stop);
    }

private:
    // Waits an exponentially distributed time with mean gap_us. The draw comes from
    // the key stream, so the prover can't predict when the next challenge arrives
    // and move the buffer out of fast memory until just before it.
    void pause() {
        if (a_.gap_us == 0) return;
        const double u = (double)(gaps_.next() >> 11) * 0x1.0p-53;
        usleep((useconds_t)std::min(-std::log1p(-u) * a_.gap_us, kMaxGapFactor * a_.gap_us));
    }

    int                   fd_;
    const Args&           a_;
    const Reference&      ref_;
    record::VerifierStream* stream_;
    KeyStream             nonces_, checks_, gaps_;
    uint64_t              next_nonce_ = 0;
    uint64_t              seq_        = 0;
};

} // namespace mc
